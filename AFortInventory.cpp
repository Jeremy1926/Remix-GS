#include "pch.h"
#include "Shared.h"
#include "AFortInventory.h"
#include "AFortPlayerController.h"
#include <objbase.h>

INIT_MODULE(AFortInventory);
INIT_POSTLOAD(AFortInventory);

void SetStateValueConstObject(FFortItemEntry& Entry, const FGameplayTag& StateType, const UObject* Object)
{
    auto& ConstObjects = Entry.StateValuesConstObject;

    for (auto& ConstObject : ConstObjects)
    {
        if (ConstObject.StateType.TagName.ComparisonIndex == StateType.TagName.ComparisonIndex)
        {
            ConstObject.ObjectData = TWeakObjectPtr<UObject>((UObject*)Object);
            return;
        }
    }

    FFortItemEntryStateValueConstObject NewConstObject{};

    NewConstObject.StateType.TagName.ComparisonIndex = StateType.TagName.ComparisonIndex;
    NewConstObject.ObjectData = TWeakObjectPtr<UObject>((UObject*)Object);
    ConstObjects.Add(NewConstObject);
}

void CopyWeaponModsFrom(FFortItemEntry& Entry, const FFortItemEntry& Source)
{
    if (&Source == &Entry)
        return;

    auto& WeaponModSlots = Entry.WeaponModSlots;
    auto& SourceWeaponModSlots = Source.WeaponModSlots;

    if (SourceWeaponModSlots.Num() > 0)
    {
        if (WeaponModSlots.Data)
            FMemory::Free(WeaponModSlots.Data);

        WeaponModSlots.Data = nullptr;

        if (SourceWeaponModSlots.Data)
        {
            WeaponModSlots.Data = (FFortWeaponModSlot*)FMemory::Malloc(uint64(SourceWeaponModSlots.Num()) * sizeof(FFortWeaponModSlot));
            memcpy(WeaponModSlots.Data, SourceWeaponModSlots.Data, uint64(SourceWeaponModSlots.Num()) * sizeof(FFortWeaponModSlot));
        }

        WeaponModSlots.NumElements = WeaponModSlots.MaxElements = SourceWeaponModSlots.Num();
    }

    auto& ConstObjects = Entry.StateValuesConstObject;
    auto& SourceConstObjects = Source.StateValuesConstObject;

    if (SourceConstObjects.Num() > 0)
    {
        if (ConstObjects.Data)
            FMemory::Free(ConstObjects.Data);

        ConstObjects.Data = nullptr;

        if (SourceConstObjects.Data)
        {
            ConstObjects.Data = (FFortItemEntryStateValueConstObject*)FMemory::Malloc(uint64(SourceConstObjects.Num()) * sizeof(FFortItemEntryStateValueConstObject));
            memcpy(ConstObjects.Data, SourceConstObjects.Data, uint64(SourceConstObjects.Num()) * sizeof(FFortItemEntryStateValueConstObject));
        }

        ConstObjects.NumElements = ConstObjects.MaxElements = SourceConstObjects.Num();
    }
}

static void InitializeWeaponModSlots(FFortItemEntry& Entry, const UFortWeaponItemDefinition* WeaponDefinition)
{
    auto& WeaponModSlots = Entry.WeaponModSlots;

    if (!WeaponDefinition || WeaponModSlots.Num() > 0)
        return;

    auto DefaultWeaponModSlots = WeaponDefinition->GetWeaponModSlots();

    if (DefaultWeaponModSlots.Num() <= 0)
    {
        DefaultWeaponModSlots.Free();
        return;
    }

    if (WeaponModSlots.Data)
        FMemory::Free(WeaponModSlots.Data);

    WeaponModSlots.Data = DefaultWeaponModSlots.Data;
    WeaponModSlots.NumElements = DefaultWeaponModSlots.Num();
    WeaponModSlots.MaxElements = DefaultWeaponModSlots.Max();
}

bool CanBeDropped(const UFortItemDefinition* ItemDefinition)
{
    static auto PickupDataStruct = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortItemComponentData_Pickup");

    for (auto& ItemData : ItemDefinition->DataList)
    {
        if (ItemData.ScriptStruct == PickupDataStruct)
            return ((FFortItemComponentData_Pickup*)ItemData.StructMemory)->bCanBeDroppedFromInventory;
    }

    return false;
}

UFortWorldItem* GiveItem(AFortInventory* Inventory, const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, int Level, bool bNotifyPlayer, bool bUpdateInventory, int PhantomReserveAmmo,
                         TArray<FFortItemEntryStateValue> StateValues, const FFortItemEntry* SourceEntry)
{
    if (!Inventory || !ItemDefinition || !Count)
        return nullptr;

    auto MaxStack = ItemDefinition->GetMaxStackSize(nullptr);

    for (int i = 0; MaxStack > 1 && Count > 0 && i < Inventory->Inventory.ReplicatedEntries.Num(); i++)
    {
        auto& ReplicatedEntry = Inventory->Inventory.ReplicatedEntries[i];

        if (ReplicatedEntry.ItemDefinition != ItemDefinition || ReplicatedEntry.Count >= MaxStack)
            continue;

        for (auto Instance : Inventory->Inventory.ItemInstances)
        {
            if (Instance->ItemEntry.ItemGuid != ReplicatedEntry.ItemGuid)
                continue;

            auto Added = (std::min)(Count, MaxStack - ReplicatedEntry.Count);

            ReplicatedEntry.Count += Added;
            Instance->ItemEntry.Count = ReplicatedEntry.Count;
            Count -= Added;

            if (bUpdateInventory)
            {
                UpdateEntry(Inventory, ReplicatedEntry);
                Instance->ItemEntry.bIsDirty = true;
            }

            if (Count <= 0)
                return Instance;

            break;
        }
    }

    auto Item = (UFortWorldItem*)ItemDefinition->CreateTemporaryItemInstanceBP(Count, Level);

    if (Inventory->Owner && Inventory->Owner->IsA(AFortPlayerController::StaticClass()))
        Item->SetOwningControllerForTemporaryItem((AFortPlayerController*)Inventory->Owner);

    Item->ItemEntry.ParentInventory = Inventory;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.PhantomReserveAmmo = PhantomReserveAmmo;

    if (SourceEntry)
        CopyWeaponModsFrom(Item->ItemEntry, *SourceEntry);

    if (ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()))
        InitializeWeaponModSlots(Item->ItemEntry, (UFortWeaponItemDefinition*)ItemDefinition);

    if (StateValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(sizeof(FFortItemEntryStateValue) * StateValues.Num());
        memcpy(NewData, StateValues.Data, sizeof(FFortItemEntryStateValue) * StateValues.Num());
        Item->ItemEntry.StateValues.NumElements = StateValues.Num();
        Item->ItemEntry.StateValues.MaxElements = StateValues.Num();
        Item->ItemEntry.StateValues.Data = (FFortItemEntryStateValue*)NewData;
    }

    if (Item->ItemEntry.ItemGuid.A == 0 && Item->ItemEntry.ItemGuid.B == 0 && Item->ItemEntry.ItemGuid.C == 0 && Item->ItemEntry.ItemGuid.D == 0)
    {
        CoCreateGuid((GUID*)&Item->ItemEntry.ItemGuid);

        if (Item->ItemEntry.TrackerGuid.A == 0 && Item->ItemEntry.TrackerGuid.B == 0 && Item->ItemEntry.TrackerGuid.C == 0 && Item->ItemEntry.TrackerGuid.D == 0)
            CoCreateGuid((GUID*)&Item->ItemEntry.TrackerGuid);
    }

    static auto ConstructItemEntry = (FFortItemEntry* (*)(FFortItemEntry*))(ImageBase + 0x2287D6C);
    static auto AssignItemEntry = (FFortItemEntry* (*)(FFortItemEntry*, const FFortItemEntry*))(ImageBase + 0x16E78D8);

    auto& ReplicatedEntries = Inventory->Inventory.ReplicatedEntries;
    ReplicatedEntries.Reserve(1);

    auto& ReplicatedEntry = *ConstructItemEntry(&ReplicatedEntries.Data[ReplicatedEntries.NumElements++]);
    AssignItemEntry(&ReplicatedEntry, &Item->ItemEntry);
    ReplicatedEntry.ParentInventory = Item->ItemEntry.ParentInventory;
    ReplicatedEntry.bIsReplicatedCopy = true;
    Inventory->Inventory.ItemInstances.Add(Item);

    if (bUpdateInventory)
    {
        Inventory->bRequiresLocalUpdate = true;
        Inventory->bRequiresSaving = true;

        Inventory->HandleInventoryLocalUpdate();

        ReplicatedEntry.bIsDirty = false;
        Inventory->Inventory.MarkItemDirty(ReplicatedEntry);
        Inventory->ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    ((bool (*)(const UFortWorldItem*, const void*))Item->VTable[0xB2])(Item, Inventory->Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()));

    return Item;
}

UFortWorldItem* GiveItem(AFortInventory* Inventory, FFortItemEntry& Entry, int Count, bool bNotifyPlayer, bool bUpdateInventory)
{
    if (Count == -1)
        Count = Entry.Count;

    return GiveItem(Inventory, (UFortItemDefinition*)Entry.ItemDefinition, Count, Entry.LoadedAmmo, Entry.Level, bNotifyPlayer, bUpdateInventory, Entry.PhantomReserveAmmo, Entry.StateValues, &Entry);
}

static void SetRequiresUpdate(AFortInventory* Inventory)
{
    Inventory->Inventory.MarkArrayDirty();
    Inventory->bRequiresLocalUpdate = true;
    Inventory->bRequiresSaving = true;
    Inventory->HandleInventoryLocalUpdate();

    Inventory->ForceNetUpdate();
}

void Update(AFortInventory* Inventory, FFortItemEntry* Entry)
{
    if (!Entry)
        return SetRequiresUpdate(Inventory);

    if (Entry->bIsReplicatedCopy)
    {
        Entry->bIsDirty = false;
        Inventory->Inventory.MarkItemDirty(*Entry);
        SetRequiresUpdate(Inventory);
        return;
    }

    if (Entry->ItemGuid.A == 0 && Entry->ItemGuid.B == 0 && Entry->ItemGuid.C == 0 && Entry->ItemGuid.D == 0)
        return;

    for (int i = 0; i < Inventory->Inventory.ReplicatedEntries.Num(); i++)
    {
        auto& ReplicatedEntry = Inventory->Inventory.ReplicatedEntries[i];

        if (ReplicatedEntry.ItemGuid == Entry->ItemGuid)
        {
            static auto AssignItemEntry = (FFortItemEntry* (*)(FFortItemEntry*, const FFortItemEntry*))(ImageBase + 0x16E78D8);

            AssignItemEntry(&ReplicatedEntry, Entry);
            ReplicatedEntry.bIsDirty = false;
            Inventory->Inventory.MarkItemDirty(ReplicatedEntry);
            SetRequiresUpdate(Inventory);
            break;
        }
    }
}

void RemoveWeaponAbilities(AFortWeapon* Weapon)
{
    static auto ClearAbility = (void (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle&))(ImageBase + 0x7AE54BC);
    auto PlayerController = Weapon->Instigator ? (AFortPlayerControllerAthena*)Weapon->Instigator->Controller : nullptr;
    if (!PlayerController || !PlayerController->PlayerState || !PlayerController->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()))
        return;
    auto AbilitySystemComponent = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->AbilitySystemComponent;

    if (Weapon->PrimaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->PrimaryAbilitySpecHandle);
        Weapon->PrimaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->SecondaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->SecondaryAbilitySpecHandle);
        Weapon->SecondaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ReloadAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ReloadAbilitySpecHandle);
        Weapon->ReloadAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ImpactAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ImpactAbilitySpecHandle);
        Weapon->ImpactAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->EquippedAbilityHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilityHandles.Num(); i++)
        {
            auto& Handle = Weapon->EquippedAbilityHandles[i];

            if (Handle.Handle != -1)
                ClearAbility(AbilitySystemComponent, Handle);
        }
        Weapon->EquippedAbilityHandles.Clear();
    }

    if (Weapon->EquippedAbilitySetHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilitySetHandles.Num(); i++)
            UFortKismetLibrary::UnequipFortAbilitySet(Weapon->EquippedAbilitySetHandles[i]);

        if (PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()))
            PlayerController->AppliedInGameModifierAbilitySetHandles.Reset();
        Weapon->EquippedAbilitySetHandles.Clear();
    }
}

void Remove(AFortInventory* Inventory, FGuid Guid)
{
    int32 ItemEntryIdx = -1;
    int32 ItemInstanceIdx = -1;

    for (int i = 0; i < Inventory->Inventory.ReplicatedEntries.Num(); i++)
    {
        if (Inventory->Inventory.ReplicatedEntries[i].ItemGuid == Guid)
        {
            ItemEntryIdx = i;
            break;
        }
    }

    for (int i = 0; i < Inventory->Inventory.ItemInstances.Num(); i++)
    {
        if (Inventory->Inventory.ItemInstances[i]->ItemEntry.ItemGuid == Guid)
        {
            ItemInstanceIdx = i;
            break;
        }
    }

    auto Instance = ItemInstanceIdx != -1 ? Inventory->Inventory.ItemInstances.Data[ItemInstanceIdx] : nullptr;

    if (ItemEntryIdx != -1)
    {
        static auto DestroyItemEntry = (void (*)(FFortItemEntry*))(ImageBase + 0x16E77DC);

        DestroyItemEntry(&Inventory->Inventory.ReplicatedEntries.Data[ItemEntryIdx]);
        Inventory->Inventory.ReplicatedEntries.Remove(ItemEntryIdx);
    }
    if (ItemInstanceIdx != -1)
        Inventory->Inventory.ItemInstances.Remove(ItemInstanceIdx);

    Inventory->bRequiresLocalUpdate = true;
    Inventory->bRequiresSaving = true;

    Inventory->HandleInventoryLocalUpdate();

    Inventory->Inventory.MarkArrayDirty();
    Inventory->ForceNetUpdate();

    if (Instance && Instance->ItemEntry.ItemDefinition)
        ((bool (*)(const UFortWorldItem*, const void*, uint32_t))Instance->VTable[0xB3])(Instance, Inventory->Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()), Instance->ItemEntry.Count);
}

bool RemoveInventoryItem(AFortInventory* Inventory, FGuid& ItemGuid, int32 Count, bool bForceRemoval, bool bForcePersistWhenEmpty)
{
    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;

    for (auto Instance : Inventory->Inventory.ItemInstances)
    {
        if (Instance->ItemEntry.ItemGuid == ItemGuid)
        {
            Item = Instance;
            break;
        }
    }

    for (auto& Entry : Inventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemGuid == ItemGuid)
        {
            ItemEntry = &Entry;
            break;
        }
    }

    if (!Item)
        return false;

    ItemEntry->Count -= (std::max)(Count, 0);

    if ((Count < 0 || ItemEntry->Count <= 0 || bForceRemoval) && !(bForcePersistWhenEmpty && Count >= 0 && !bForceRemoval))
        Remove(Inventory, ItemGuid);
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(Inventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }

    return true;
}

FFortRangedWeaponStats* GetStats(const UFortWeaponItemDefinition* WeaponDefinition)
{
    if (!WeaponDefinition || !WeaponDefinition->WeaponStatHandle.DataTable)
        return nullptr;

    for (auto& Pair : WeaponDefinition->WeaponStatHandle.DataTable->RowMap)
    {
        if (WeaponDefinition->WeaponStatHandle.RowName == Pair.Key() && Pair.Value())
            return (FFortRangedWeaponStats*)Pair.Value();
    }

    return nullptr;
}

FFortItemEntry* MakeItemEntry(const UFortItemDefinition* ItemDefinition, int32 Count, int32 Level)
{
    auto ItemEntry = (FFortItemEntry*)malloc(sizeof(FFortItemEntry));
    memset((PBYTE)ItemEntry, 0, sizeof(FFortItemEntry));

    ItemEntry->MostRecentArrayReplicationKey = -1;
    ItemEntry->ReplicationID = -1;
    ItemEntry->ReplicationKey = -1;

    auto ItemDef = ItemDefinition;
    if (ItemDef->IsA(UFortWorldMultiItemDefinition::StaticClass()))
    {
        auto MultiWorldItemDef = (UFortWorldMultiItemDefinition*)ItemDef;

        ItemDef = MultiWorldItemDef->ItemInfos[rand() % MultiWorldItemDef->ItemInfos.Num()].ItemDefinition.Get();
    }
    ItemEntry->ItemDefinition = (UItemDefinitionBase*)ItemDef;
    ItemEntry->Count = Count;
    ItemEntry->Durability = 1.f;
    ItemEntry->GameplayAbilitySpecHandle.Handle = -1;
    ItemEntry->ParentInventory.ObjectIndex = -1;
    ItemEntry->Level = Level;
    if (auto Weapon = ItemDef->IsA(UFortGadgetItemDefinition::StaticClass()) ? ((UFortGadgetItemDefinition*)ItemDef)->GetWeaponItemDefinition() : ItemDef->IsA(UFortWeaponItemDefinition::StaticClass()) ? (UFortWeaponItemDefinition*)ItemDef : nullptr)
    {
        auto Stats = GetStats(Weapon);
        if (Stats)
        {
            ItemEntry->LoadedAmmo = Stats->ClipSize;

            if (Weapon->bUsesPhantomReserveAmmo)
                ItemEntry->PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
        }
    }
    ItemEntry->PickupVariantIndex = -1;
    ItemEntry->ItemVariantDataMappingIndex = -1;
    ItemEntry->OrderIndex = -1;

    return ItemEntry;
}

bool IsPrimaryQuickbar(const UFortItemDefinition* ItemDefinition)
{
    return ItemDefinition->ItemType == EFortItemType::WeaponHarvest || ItemDefinition->ItemType == EFortItemType::WorldResource || ItemDefinition->ItemType == EFortItemType::Ammo ||
                   ItemDefinition->ItemType == EFortItemType::Trap || ItemDefinition->ItemType == EFortItemType::BuildingPiece || ItemDefinition->ItemType == EFortItemType::EditTool ||
                   ItemDefinition->ItemType == EFortItemType::Ingredient
               ? false
               : true;
}

bool TakesQuickbarSlot(const UFortItemDefinition* ItemDefinition)
{
    return IsPrimaryQuickbar(ItemDefinition) && UFortItemFunctionLibrary::IsInventorySizeLimited(ItemDefinition);
}

void UpdateEntry(AFortInventory* Inventory, FFortItemEntry& Entry)
{
    if (!Inventory)
        return;

    Update(Inventory, &Entry);
}

int32 AddItemCount(AFortInventory* Inventory, const UFortItemDefinition* ItemDefinition, int32 Count)
{
    if (!Inventory || !ItemDefinition || Count <= 0)
        return 0;

    auto MaxStack = ItemDefinition->GetMaxStackSize(nullptr);
    FFortItemEntry* ItemEntry = nullptr;

    for (auto& ReplicatedEntry : Inventory->Inventory.ReplicatedEntries)
    {
        if (ReplicatedEntry.ItemDefinition == ItemDefinition)
        {
            ItemEntry = &ReplicatedEntry;
            break;
        }
    }

    if (!ItemEntry)
    {
        auto Given = MaxStack > 0 && Count > MaxStack ? MaxStack : Count;
        GiveItem(Inventory, ItemDefinition, Given);
        return Given;
    }

    auto NewCount = MaxStack > 0 && ItemEntry->Count + Count > MaxStack ? MaxStack : ItemEntry->Count + Count;
    auto Added = NewCount - ItemEntry->Count;

    if (Added <= 0)
        return 0;

    ItemEntry->Count = NewCount;

    for (auto& ItemInstance : Inventory->Inventory.ItemInstances)
    {
        if (ItemInstance->ItemEntry.ItemDefinition == ItemDefinition)
        {
            ItemInstance->ItemEntry.Count = NewCount;
            break;
        }
    }

    UpdateEntry(Inventory, *ItemEntry);
    return Added;
}

static bool SetLoadedAmmo(UFortWorldItem* Item, int LoadedAmmo)
{
    if (!Item || Item->ItemEntry.LoadedAmmo == LoadedAmmo)
        return false;

    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.bIsDirty = true;

    auto Inventory = (AFortInventory*)Item->ItemEntry.ParentInventory.Get();

    if (!Inventory)
    {
        auto Controller = Item->GetOwningController();

        if (Controller && Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
            Inventory = (AFortInventory*)Controller->WorldInventory.ObjectPointer;
    }

    if (!Inventory)
        return true;

    FFortItemEntry* repEnt = nullptr;

    for (auto& ReplicatedEntry : Inventory->Inventory.ReplicatedEntries)
    {
        if (ReplicatedEntry.ItemGuid == Item->ItemEntry.ItemGuid)
        {
            repEnt = &ReplicatedEntry;
            break;
        }
    }

    if (!repEnt)
        return true;

    repEnt->LoadedAmmo = LoadedAmmo;
    UpdateEntry(Inventory, *repEnt);

    return true;
}

static bool SetPhantomReserveAmmo(UFortWorldItem* Item, unsigned int PhantomReserveAmmo)
{
    if (!Item || Item->ItemEntry.PhantomReserveAmmo == (int32)PhantomReserveAmmo)
        return false;

    Item->ItemEntry.PhantomReserveAmmo = PhantomReserveAmmo;
    Item->ItemEntry.bIsDirty = true;

    auto Inventory = (AFortInventory*)Item->ItemEntry.ParentInventory.Get();

    if (!Inventory)
    {
        auto Controller = Item->GetOwningController();

        if (Controller && Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
            Inventory = (AFortInventory*)Controller->WorldInventory.ObjectPointer;
    }

    if (!Inventory)
        return true;

    FFortItemEntry* repEnt = nullptr;

    for (auto& ReplicatedEntry : Inventory->Inventory.ReplicatedEntries)
    {
        if (ReplicatedEntry.ItemGuid == Item->ItemEntry.ItemGuid)
        {
            repEnt = &ReplicatedEntry;
            break;
        }
    }

    if (!repEnt)
        return true;

    repEnt->PhantomReserveAmmo = PhantomReserveAmmo;
    UpdateEntry(Inventory, *repEnt);

    return true;
}

static void RemoveItem(UObject* Context, FFrame& Stack, bool* Ret)
{
    FScriptInterface InventoryOwner{};
    FGuid ItemGuid{};
    int32 Count = 0;
    bool bForceRemoval = false;
    bool bForcePersistWhenEmpty = false;
    Stack.StepCompiledIn(&InventoryOwner);
    Stack.StepCompiledIn(&ItemGuid);
    Stack.StepCompiledIn(&Count);
    Stack.StepCompiledIn(&bForceRemoval);
    Stack.StepCompiledIn(&bForcePersistWhenEmpty);
    Stack.IncrementCode();

    auto Owner = InventoryOwner.ObjectPointer;
    auto Interface = InventoryOwner.InterfacePointer;
    *Ret = Owner && Interface && Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass());

    if (!*Ret || !Owner->IsA(AActor::StaticClass()) || ((AActor*)Owner)->Role != ENetRole::ROLE_Authority)
        return;

    auto RemoveInventoryItem = (bool (*)(void*, FGuid&, int32, bool, bool))(*(void***)Interface)[0x3D];
    RemoveInventoryItem(Interface, ItemGuid, Count, bForceRemoval, bForcePersistWhenEmpty);
}

static void GiveItemToInventoryOwner(UObject* Context, FFrame& Stack, UFortWorldItem** Ret)
{
    TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner;
    UFortItemDefinition* ItemDefinition;
    FGuid ItemVariantGuid;
    int32 NumberToGive;
    bool bNotifyPlayer;
    int32 ItemLevel;
    int32 PickupInstigatorHandle;
    bool bUseItemPickupAnalyticEvent;
    int32 WeaponAmmoOverride;

    Stack.StepCompiledIn(&InventoryOwner);
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&ItemVariantGuid);
    Stack.StepCompiledIn(&NumberToGive);
    Stack.StepCompiledIn(&bNotifyPlayer);
    Stack.StepCompiledIn(&ItemLevel);
    Stack.StepCompiledIn(&PickupInstigatorHandle);
    Stack.StepCompiledIn(&bUseItemPickupAnalyticEvent);
    Stack.StepCompiledIn(&WeaponAmmoOverride);
    Stack.IncrementCode();

    *Ret = nullptr;

    auto Owner = InventoryOwner.ObjectPointer;

    if (!Owner || !ItemDefinition)
        return;

    if (Owner->IsA(AFortAthenaAIBotController::StaticClass()))
    {
        auto BotInventory = (AFortInventory*)((AFortAthenaAIBotController*)Owner)->Inventory.ObjectPointer;

        if (!BotInventory)
            return;

        auto Stats = ItemDefinition->IsA(UFortWeaponRangedItemDefinition::StaticClass()) ? GetStats((UFortWeaponItemDefinition*)ItemDefinition) : nullptr;
        auto LoadedAmmo = WeaponAmmoOverride != -1 ? WeaponAmmoOverride : (Stats ? Stats->ClipSize : 0);

        *Ret = GiveItem(BotInventory, ItemDefinition, NumberToGive, LoadedAmmo, ItemLevel != -1 ? ItemLevel : 0);
        return;
    }

    auto PlayerController = Owner->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Owner : nullptr;
    auto WorldInventory = PlayerController ? (AFortInventory*)PlayerController->WorldInventory.ObjectPointer : nullptr;

    if (!WorldInventory)
        return;

    auto ItemEntry = MakeItemEntry(ItemDefinition, NumberToGive, ItemLevel);

    if (WeaponAmmoOverride != -1)
        ItemEntry->LoadedAmmo = WeaponAmmoOverride;

    InternalPickup(PlayerController, ItemEntry);
    free(ItemEntry);

    for (auto Item : WorldInventory->Inventory.ItemInstances)
    {
        if (Item->ItemEntry.ItemDefinition == ItemDefinition)
        {
            *Ret = Item;
            break;
        }
    }
}

static int32 RemoveItemFromPlayer(AFortInventory* WorldInventory, UFortWorldItem* Item, FFortItemEntry* ItemEntry, int32 AmountToRemove)
{
    auto RemoveCount = (std::max)(AmountToRemove, 0);
    ItemEntry->Count -= RemoveCount;

    if (AmountToRemove < 0 || ItemEntry->Count <= 0)
    {
        RemoveCount += ItemEntry->Count;
        Remove(WorldInventory, ItemEntry->ItemGuid);
    }
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(WorldInventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }

    return RemoveCount;
}

static void K2_RemoveItemFromPlayer(UObject* Context, FFrame& Stack, int32* Ret)
{
    AFortPlayerControllerAthena* PlayerController;
    UFortItemDefinition* ItemDefinition;
    FGuid ItemVariantGuid;
    int32 AmountToRemove;
    bool bForceRemoval;

    Stack.StepCompiledIn(&PlayerController);
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&ItemVariantGuid);
    Stack.StepCompiledIn(&AmountToRemove);
    Stack.StepCompiledIn(&bForceRemoval);
    Stack.IncrementCode();

    *Ret = 0;

    if (!PlayerController || !ItemDefinition)
        return;

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;

    for (auto Instance : WorldInventory->Inventory.ItemInstances)
    {
        if (Instance->ItemEntry.ItemDefinition == ItemDefinition)
        {
            Item = Instance;
            break;
        }
    }

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition == ItemDefinition)
        {
            ItemEntry = &Entry;
            break;
        }
    }

    if (Item)
        *Ret = RemoveItemFromPlayer(WorldInventory, Item, ItemEntry, AmountToRemove);
}

static void K2_RemoveItemFromPlayerByGuid(UObject* Context, FFrame& Stack, int32* Ret)
{
    AFortPlayerControllerAthena* PlayerController;
    FGuid ItemGuid;
    int32 AmountToRemove;
    bool bForceRemoval;

    Stack.StepCompiledIn(&PlayerController);
    Stack.StepCompiledIn(&ItemGuid);
    Stack.StepCompiledIn(&AmountToRemove);
    Stack.StepCompiledIn(&bForceRemoval);
    Stack.IncrementCode();

    *Ret = 0;

    if (!PlayerController)
        return;

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;

    for (auto Instance : WorldInventory->Inventory.ItemInstances)
    {
        if (Instance->ItemEntry.ItemGuid == ItemGuid)
        {
            Item = Instance;
            break;
        }
    }

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemGuid == ItemGuid)
        {
            ItemEntry = &Entry;
            break;
        }
    }

    if (Item)
        *Ret = RemoveItemFromPlayer(WorldInventory, Item, ItemEntry, AmountToRemove);
}

void AFortInventory__Init()
{
    ExecHook(UInventoryManagementLibrary::StaticClass()->FindFunction("RemoveItem"), RemoveItem);
}

void AFortInventory__PostLoad()
{
    Hook<UFortWorldItem>(uint32(0xC4), SetLoadedAmmo);
    Hook<UFortWorldItem>(uint32(0xC5), SetPhantomReserveAmmo);

    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("GiveItemToInventoryOwner"), GiveItemToInventoryOwner);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_RemoveItemFromPlayer"), K2_RemoveItemFromPlayer);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_RemoveItemFromPlayerByGuid"), K2_RemoveItemFromPlayerByGuid);
}
