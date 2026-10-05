#pragma once
#include "pch.h"

void InternalPickup(AController* Controller, FFortItemEntry* PickupEntry);
void DropItemsAsPickups(TArray<UFortWorldItem*>& ItemsToDropViaPickup, AFortPawn* DestructionPawn, AFortInventory* Inventory, AActor* PickupPostSpawnObject, void (*PickupPostSpawn)(AActor*, AFortPickup*, int32));
const UClass* ApplyOutsideSafeZoneEffect(UAbilitySystemComponent* AbilitySystemComponent, bool bApply = true);
