// Copyright Natali Caggiano. All Rights Reserved.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/MCPToolRegistry.h"
#include "MCP/Tools/MCPTool_MechanicPreflight.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MechanicPreflightTests
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> MakeParams(
		const FString& TargetMap,
		const FString& MechanicName,
		const TArray<FString>& Inputs,
		const TArray<FString>& AnimationRoles = {})
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("inspect"));
		Params->SetStringField(TEXT("target_map"), TargetMap);
		Params->SetStringField(TEXT("mechanic_name"), MechanicName);
		Params->SetArrayField(TEXT("requested_inputs"), MakeStringArray(Inputs));
		Params->SetArrayField(TEXT("requested_animation_roles"), MakeStringArray(AnimationRoles));
		Params->SetBoolField(TEXT("include_animation_preflight"), AnimationRoles.Num() > 0);
		Params->SetBoolField(TEXT("export_report"), false);
		return Params;
	}

	TSharedPtr<FJsonObject> RunPreflight(
		FAutomationTestBase& Test,
		const FString& TargetMap,
		const FString& MechanicName,
		const TArray<FString>& Inputs,
		const TArray<FString>& AnimationRoles = {})
	{
		FMCPTool_MechanicPreflight Tool;
		const FMCPToolResult Result = Tool.Execute(MakeParams(TargetMap, MechanicName, Inputs, AnimationRoles));
		Test.TestTrue(TEXT("mechanic_preflight should succeed"), Result.bSuccess);
		Test.TestTrue(TEXT("mechanic_preflight should return data"), Result.Data.IsValid());
		return Result.Data;
	}

	bool JsonValueContains(const TSharedPtr<FJsonValue>& Value, const FString& Needle)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		FString StringValue;
		if (Value->TryGetString(StringValue))
		{
			return StringValue.Contains(Needle, ESearchCase::IgnoreCase);
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ObjectValue = Value->AsObject();
			if (ObjectValue.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ObjectValue->Values)
				{
					if (Pair.Key.Contains(Needle, ESearchCase::IgnoreCase) || JsonValueContains(Pair.Value, Needle))
					{
						return true;
					}
				}
			}
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
			if (Value->TryGetArray(ArrayValue) && ArrayValue != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Entry : *ArrayValue)
				{
					if (JsonValueContains(Entry, Needle))
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	bool ArrayTextContains(const TSharedPtr<FJsonObject>& Object, const FString& ArrayField, const FString& Needle)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(ArrayField, Values) || Values == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (JsonValueContains(Value, Needle))
			{
				return true;
			}
		}

		return false;
	}

	bool ConflictContains(const TSharedPtr<FJsonObject>& Object, const FString& Needle)
	{
		return ArrayTextContains(Object, TEXT("conflicts"), Needle);
	}

	bool HasBlockingShiftConflict(const TSharedPtr<FJsonObject>& Object)
	{
		const TArray<TSharedPtr<FJsonValue>>* Conflicts = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("conflicts"), Conflicts) || Conflicts == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ConflictValue : *Conflicts)
		{
			const TSharedPtr<FJsonObject> Conflict = ConflictValue.IsValid() ? ConflictValue->AsObject() : nullptr;
			if (!Conflict.IsValid())
			{
				continue;
			}

			const FString Severity = Conflict->GetStringField(TEXT("severity"));
			const FString Reason = Conflict->GetStringField(TEXT("reason"));
			if ((Severity.Equals(TEXT("high"), ESearchCase::IgnoreCase) || Severity.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
				&& Reason.Contains(TEXT("Shift"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_InfoAndRegistry,
	"OsvayderUE.MechanicPreflight.InfoAndRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_CombatWallrunSpaceShiftBlocks,
	"OsvayderUE.MechanicPreflight.CombatWallrunSpaceShiftBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_PlatformingWallrunShiftBlocks,
	"OsvayderUE.MechanicPreflight.PlatformingWallrunShiftBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_PlatformingWallrunJumpOnlyNoShiftBlock,
	"OsvayderUE.MechanicPreflight.PlatformingWallrunJumpOnlyNoShiftBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_UnsupportedMapBlocks,
	"OsvayderUE.MechanicPreflight.UnsupportedMapBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_MissingGameMapBlocks,
	"OsvayderUE.MechanicPreflight.MissingGameMapBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_CombatPossessionYReported,
	"OsvayderUE.MechanicPreflight.CombatPossessionYReported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_CombatTelekinesisETReported,
	"OsvayderUE.MechanicPreflight.CombatTelekinesisETReported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanicPreflight_AnimationRolesStayManualBlocker,
	"OsvayderUE.MechanicPreflight.AnimationRolesStayManualBlocker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMechanicPreflight_InfoAndRegistry::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	IMCPTool* Tool = Registry.FindTool(TEXT("mechanic_preflight"));
	TestNotNull(TEXT("mechanic_preflight should be registered"), Tool);
	if (!Tool)
	{
		return false;
	}

	const FMCPToolInfo Info = Tool->GetInfo();
	TestEqual(TEXT("tool name should match"), Info.Name, FString(TEXT("mechanic_preflight")));
	TestTrue(TEXT("tool should be read-only"), Info.Annotations.bReadOnlyHint);
	TestFalse(TEXT("tool should not be destructive"), Info.Annotations.bDestructiveHint);
	TestTrue(TEXT("description should mention mutates_assets=false"), Info.Description.Contains(TEXT("mutates_assets=false"), ESearchCase::IgnoreCase));
	return true;
}

bool FMechanicPreflight_CombatWallrunSpaceShiftBlocks::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Combat/Lvl_Combat"),
		TEXT("wallrun"),
		{TEXT("Space"), TEXT("LeftShift")},
		{TEXT("wallrun_left"), TEXT("wallrun_right"), TEXT("wall_jump")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("tool should explicitly be read-only"), Data->GetBoolField(TEXT("read_only")));
	TestFalse(TEXT("tool should not mutate assets"), Data->GetBoolField(TEXT("mutates_assets")));
	TestFalse(TEXT("Combat Space+Shift wallrun should block implementation"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention flight"), MechanicPreflightTests::ConflictContains(Data, TEXT("flight")));
	TestTrue(TEXT("conflict should mention Space"), MechanicPreflightTests::ConflictContains(Data, TEXT("Space")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should exist"), Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestFalse(TEXT("runtime proof should be false"), (*ProofMatrix)->GetBoolField(TEXT("runtime_proof")));
		TestFalse(TEXT("visual proof should be false"), (*ProofMatrix)->GetBoolField(TEXT("visual_proof")));
	}

	return true;
}

bool FMechanicPreflight_PlatformingWallrunShiftBlocks::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Platforming/Lvl_Platforming"),
		TEXT("wallrun"),
		{TEXT("LeftShift")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("Platforming Shift wallrun should block implementation"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention dash"), MechanicPreflightTests::ConflictContains(Data, TEXT("dash")));
	TestTrue(TEXT("conflict should mention Shift"), MechanicPreflightTests::ConflictContains(Data, TEXT("Shift")));
	TestTrue(TEXT("conflict should suggest Jump-hold"), MechanicPreflightTests::ConflictContains(Data, TEXT("Jump-hold")));
	TestTrue(TEXT("conflict should suggest non-Shift option"), MechanicPreflightTests::ConflictContains(Data, TEXT("non-Shift")));
	return true;
}

bool FMechanicPreflight_PlatformingWallrunJumpOnlyNoShiftBlock::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Platforming/Lvl_Platforming"),
		TEXT("wallrun"),
		{TEXT("Space")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Platforming Space-only wallrun should not block"), Data->GetBoolField(TEXT("may_implement")));
	TestFalse(TEXT("Platforming Space-only wallrun should not report blocking Shift conflict"), MechanicPreflightTests::HasBlockingShiftConflict(Data));
	TestTrue(TEXT("facts should mention double jump context"), MechanicPreflightTests::ArrayTextContains(Data, TEXT("mechanic_facts"), TEXT("double jump")));
	TestTrue(TEXT("facts should mention wall jump context"), MechanicPreflightTests::ArrayTextContains(Data, TEXT("mechanic_facts"), TEXT("wall jump")));
	return true;
}

bool FMechanicPreflight_UnsupportedMapBlocks::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"),
		TEXT("wallrun"),
		{TEXT("Space")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("unsupported map surface should not green-light implementation"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention unsupported context"), MechanicPreflightTests::ConflictContains(Data, TEXT("unsupported")));
	TestTrue(TEXT("conflict should mention unknown target context"), MechanicPreflightTests::ConflictContains(Data, TEXT("unknown target map context")));
	TestTrue(TEXT("required choice should suggest discovery pass"), MechanicPreflightTests::ArrayTextContains(Data, TEXT("required_user_choice"), TEXT("discovery pass")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should exist"), Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestTrue(TEXT("unsupported map should set manual blocker"), (*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
	}
	return true;
}

bool FMechanicPreflight_MissingGameMapBlocks::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Combat/Lvl_Combat_DoesNotExist"),
		TEXT("wallrun"),
		{TEXT("Space")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("missing /Game map file should not green-light implementation"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention missing map file"), MechanicPreflightTests::ConflictContains(Data, TEXT("missing")));
	TestTrue(TEXT("conflict should mention level context"), MechanicPreflightTests::ConflictContains(Data, TEXT("level context")));
	TestTrue(TEXT("required choice should suggest existing supported map"), MechanicPreflightTests::ArrayTextContains(Data, TEXT("required_user_choice"), TEXT("existing supported target map")));

	const TSharedPtr<FJsonObject>* ProofMatrix = nullptr;
	TestTrue(TEXT("proof_matrix should exist"), Data->TryGetObjectField(TEXT("proof_matrix"), ProofMatrix) && ProofMatrix && (*ProofMatrix).IsValid());
	if (ProofMatrix && *ProofMatrix)
	{
		TestTrue(TEXT("missing map should set manual blocker"), (*ProofMatrix)->GetBoolField(TEXT("manual_blocker")));
	}
	return true;
}

bool FMechanicPreflight_CombatPossessionYReported::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Combat/Lvl_Combat"),
		TEXT("possession"),
		{TEXT("Y")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("Combat Y possession collision should block new ownership"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention body possession"), MechanicPreflightTests::ConflictContains(Data, TEXT("body possession")));

	const TSharedPtr<FJsonObject>* OccupiedKeys = nullptr;
	TestTrue(TEXT("occupied_keys should exist"), Data->TryGetObjectField(TEXT("occupied_keys"), OccupiedKeys) && OccupiedKeys && (*OccupiedKeys).IsValid());
	if (OccupiedKeys && *OccupiedKeys)
	{
		const TArray<TSharedPtr<FJsonValue>>* YOwners = nullptr;
		TestTrue(TEXT("Y owner should be present"), (*OccupiedKeys)->TryGetArrayField(TEXT("Y"), YOwners) && YOwners && YOwners->Num() > 0);
	}
	return true;
}

bool FMechanicPreflight_CombatTelekinesisETReported::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Combat/Lvl_Combat"),
		TEXT("telekinesis"),
		{TEXT("E"), TEXT("T")});
	if (!Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("Combat E/T telekinesis collision should block new ownership"), Data->GetBoolField(TEXT("may_implement")));
	TestTrue(TEXT("conflict should mention E/T ownership"), MechanicPreflightTests::ConflictContains(Data, TEXT("E/T")));
	TestTrue(TEXT("conflict should mention enemy telekinesis"), MechanicPreflightTests::ConflictContains(Data, TEXT("enemy telekinesis")));
	return true;
}

bool FMechanicPreflight_AnimationRolesStayManualBlocker::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Data = MechanicPreflightTests::RunPreflight(
		*this,
		TEXT("/Game/Variant_Combat/Lvl_Combat"),
		TEXT("wallrun"),
		{TEXT("Space")},
		{TEXT("wallrun_left"), TEXT("wallrun_right"), TEXT("wall_jump")});
	if (!Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* AnimationFacts = nullptr;
	TestTrue(TEXT("animation_facts should exist"), Data->TryGetObjectField(TEXT("animation_facts"), AnimationFacts) && AnimationFacts && (*AnimationFacts).IsValid());
	if (!AnimationFacts || !(*AnimationFacts).IsValid())
	{
		return false;
	}

	TestFalse(TEXT("animation preflight should not claim visual proof"), (*AnimationFacts)->GetBoolField(TEXT("visual_proof")));
	TestFalse(TEXT("animation preflight should not claim production-ready visuals"), (*AnimationFacts)->GetBoolField(TEXT("production_ready_visual_claim")));

	const TSharedPtr<FJsonObject>* Summary = nullptr;
	TestTrue(TEXT("composed animation preflight summary should exist"),
		(*AnimationFacts)->TryGetObjectField(TEXT("animation_preflight_summary"), Summary) && Summary && (*Summary).IsValid());
	if (Summary && *Summary)
	{
		TestTrue(TEXT("summary should include manual asset dependency blocker or placeholder/manual boundary"),
			MechanicPreflightTests::ArrayTextContains(*Summary, TEXT("blocker_codes"), TEXT("manual_asset_dependency_blocker"))
			|| (*Summary)->GetStringField(TEXT("proof_classification")).Contains(TEXT("manual"), ESearchCase::IgnoreCase));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
