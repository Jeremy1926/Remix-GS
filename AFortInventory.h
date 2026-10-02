#pragma once
#include "pch.h"

UFortWorldItem* GiveItem(AFortInventory* Inventory, const UFortItemDefinition* ItemDefinition, int Count = 1, int LoadedAmmo = 0, int Level = 0, bool bNotifyPlayer = true, bool bUpdateInventory = true, int PhantomReserveAmmo = 0,
                         TArray<FFortItemEntryStateValue> StateValues = {}, const FFortItemEntry* SourceEntry = nullptr);
UFortWorldItem* GiveItem(AFortInventory* Inventory, FFortItemEntry& Entry, int Count = -1, bool bNotifyPlayer = true, bool bUpdateInventory = true);
void Update(AFortInventory* Inventory, FFortItemEntry* Entry);
void Remove(AFortInventory* Inventory, FGuid Guid);
FFortItemEntry* MakeItemEntry(const UFortItemDefinition* ItemDefinition, int32 Count, int32 Level);
FFortRangedWeaponStats* GetStats(const UFortWeaponItemDefinition* WeaponDefinition);
bool IsPrimaryQuickbar(const UFortItemDefinition* ItemDefinition);
void UpdateEntry(AFortInventory* Inventory, FFortItemEntry& Entry);
int32 AddItemCount(AFortInventory* Inventory, const UFortItemDefinition* ItemDefinition, int32 Count);
void RemoveWeaponAbilities(AFortWeapon* Weapon);
void SetStateValueConstObject(FFortItemEntry& Entry, const FGameplayTag& StateType, const UObject* Object);
void CopyWeaponModsFrom(FFortItemEntry& Entry, const FFortItemEntry& Source);
bool CanBeDropped(const UFortItemDefinition* ItemDefinition);
