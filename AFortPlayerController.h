#pragma once
#include "pch.h"

void InternalPickup(AFortPlayerControllerAthena* PlayerController, FFortItemEntry* PickupEntry);
const UClass* ApplyOutsideSafeZoneEffect(UAbilitySystemComponent* AbilitySystemComponent, bool bApply = true);
