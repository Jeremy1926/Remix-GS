#include "pch.h"
#include "Shared.h"
#include "Config.h"
#include "AFortPlayerController.h"
#include "AFortInventory.h"
#include "AFortPickup.h"
#include "AFortGameMode.h"
#include "UFortQuestManager.h"
#include "UFortControllerComponent_TournamentStatsTracker.h"
#include "SDK/SDK/FortniteGame_parameters.hpp"

INIT_POSTLOAD(AFortPlayerController);

static void (*GetPlayerViewPointOG)(AFortPlayerControllerAthena*, FVector&, FRotator&);
static void (*OnPawnDiedOG)(AFortPlayerControllerAthena*, AFortPawn*, float, const FGameplayTagContainer&, const FGameplayEffectContextHandle&, AController*, AActor*, AActor*);
static void (*ClientOnPawnDiedOG)(AFortPlayerControllerAthena*, FFortPlayerDeathReport&);
static void (*K2_OnUnEquipOG)(AFortWeapon*);
static void (*ServerRequestSeatChangeOG)(UObject*, FFrame&, void*);

static void InitializePlayerGameplayAbilities(AFortPlayerControllerAthena* PlayerController)
{
    static auto InitializePlayerGameplayAbilities_ = (void (*)(void*))(ImageBase + 0x986F474);

    if (auto Interface = PlayerController->PlayerState->GetInterface(IFortAbilitySystemInterface::StaticClass()))
        InitializePlayerGameplayAbilities_(Interface);
}

static FGameplayTagContainer GetOwnedGameplayTags(UObject* Object)
{
    FGameplayTagContainer OwnedTags{};

    if (auto Interface = Object ? Object->GetInterface(IGameplayTagAssetInterface::StaticClass()) : nullptr)
        ((void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2])(Interface, &OwnedTags);

    return OwnedTags;
}

static void GetPlayerViewPoint(AFortPlayerControllerAthena* PlayerController, FVector& Location, FRotator& Rotation)
{
    if (auto ViewTarget = PlayerController->GetViewTarget())
    {
        ViewTarget->GetActorEyesViewPoint(&Location, &Rotation);
        return;
    }

    GetPlayerViewPointOG(PlayerController, Location, Rotation);
}

static void ApplyCosmeticLoadoutToPawn(AFortPlayerControllerAthena* PlayerController, APawn* Pawn)
{
    if (!Pawn || !Pawn->IsA(AFortPlayerPawnAthena::StaticClass()) || !PlayerController->CosmeticLoadoutPC.Character)
        return;

    ((void (*)(APawn*, const FFortAthenaLoadout*, bool, UObject*))Pawn->VTable[0x29E])(Pawn, &PlayerController->CosmeticLoadoutPC, true, nullptr);
}

struct FLateGameItem
{
    UFortItemDefinition* ItemDefinition;
    int32 Count;
    float Weight;
};

struct FLateGamePool
{
    std::vector<FLateGameItem> Shotguns;
    std::vector<FLateGameItem> Rifles;
    std::vector<FLateGameItem> Utilities;
    std::vector<FLateGameItem> Heals;
    std::vector<FLateGameItem> Traps;
    std::vector<FLateGameItem> Ammo;
};

static void AddLateGameItem(std::vector<FLateGameItem>& Items, UFortItemDefinition* ItemDefinition, int32 Count, float Weight)
{
    for (auto& Item : Items)
    {
        if (Item.ItemDefinition != ItemDefinition)
            continue;

        Item.Count = (std::max)(Item.Count, Count);
        Item.Weight += Weight;
        return;
    }

    Items.push_back({ ItemDefinition, Count, Weight });
}

static FLateGamePool& GetLateGamePool()
{
    static FLateGamePool Pool;
    static bool bBuilt = false;

    if (bBuilt)
        return Pool;

    bBuilt = true;

    auto LightAmmo = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight");
    auto MediumAmmo = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
    auto HeavyAmmo = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy");
    auto ShellsAmmo = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells");

    for (auto& [PackageId, Packages] : LootPackageMap)
    {
        for (auto Package : Packages)
        {
            if (!Package || Package->Weight <= 0.f || Package->LootPackageCall.Num() > 1)
                continue;

            auto ItemDefinition = Package->ItemDefinition.Get();

            if (!ItemDefinition)
                continue;

            auto Count = Package->Count > 0 ? Package->Count : 1;

            if (ItemDefinition->IsA(UFortAmmoItemDefinition::StaticClass()))
                AddLateGameItem(Pool.Ammo, ItemDefinition, Count, Package->Weight);
            else if (ItemDefinition->ItemType == EFortItemType::Trap)
                AddLateGameItem(Pool.Traps, ItemDefinition, Count, Package->Weight);
            else if (ItemDefinition->ItemType == EFortItemType::Consumable)
                AddLateGameItem(Pool.Heals, ItemDefinition, Count, Package->Weight);
            else if (ItemDefinition->IsA(UFortWeaponRangedItemDefinition::StaticClass()))
            {
                auto AmmoDefinition = ((UFortWorldItemDefinition*)ItemDefinition)->GetAmmoWorldItemDefinition_BP();

                if (AmmoDefinition && AmmoDefinition == ShellsAmmo)
                    AddLateGameItem(Pool.Shotguns, ItemDefinition, 1, Package->Weight);
                else if (AmmoDefinition && AmmoDefinition == MediumAmmo)
                    AddLateGameItem(Pool.Rifles, ItemDefinition, 1, Package->Weight);
                else if (AmmoDefinition && (AmmoDefinition == LightAmmo || AmmoDefinition == HeavyAmmo))
                    AddLateGameItem(Pool.Utilities, ItemDefinition, 1, Package->Weight);
            }
        }
    }

    return Pool;
}

static const FLateGameItem* PickLateGameItem(const std::vector<FLateGameItem>& Items)
{
    auto bHasRare = std::any_of(Items.begin(), Items.end(), [](const FLateGameItem& Item) { return Item.ItemDefinition->Rarity >= EFortRarity::Rare; });
    float TotalWeight = 0.f;

    for (auto& Item : Items)
    {
        if (!bHasRare || Item.ItemDefinition->Rarity >= EFortRarity::Rare)
            TotalWeight += Item.Weight;
    }

    auto Roll = (float)UKismetMathLibrary::RandomFloatInRange(0., TotalWeight);
    const FLateGameItem* Picked = nullptr;

    for (auto& Item : Items)
    {
        if (bHasRare && Item.ItemDefinition->Rarity < EFortRarity::Rare)
            continue;

        Picked = &Item;
        Roll -= Item.Weight;

        if (Roll <= 0.f)
            break;
    }

    return Picked;
}

static int32 GetLateGameCount(const UFortItemDefinition* ItemDefinition, int32 Min, int32 Max)
{
    auto MaxStack = ItemDefinition->GetMaxStackSize(nullptr);

    if (MaxStack > 0 && Max > MaxStack)
        Max = MaxStack;

    return UKismetMathLibrary::RandomIntegerInRange((std::min)(Min, Max), Max);
}

static void GiveLateGameLoadout(AFortPlayerControllerAthena* PlayerController)
{
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto& Pool = GetLateGamePool();
    std::vector<FLateGameItem> Loadout;

    if (LateGameLoadout.empty())
    {
        for (auto Items : { &Pool.Shotguns, &Pool.Rifles, &Pool.Utilities, &Pool.Heals, &Pool.Heals, &Pool.Traps })
        {
            if (auto Item = PickLateGameItem(*Items))
                Loadout.push_back(*Item);
        }
    }
    else
    {
        for (auto& [Path, Count] : LateGameLoadout)
        {
            if (auto ItemDefinition = FindObject<UFortItemDefinition>(Path.c_str()))
                Loadout.push_back({ ItemDefinition, Count, 0.f });
        }
    }

    for (auto ResourceType : { EFortResourceType::Wood, EFortResourceType::Stone, EFortResourceType::Metal })
    {
        if (auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(ResourceType))
            GiveItem(WorldInventory, Resource, Resource->GetMaxStackSize(nullptr));
    }

    for (auto& Ammo : Pool.Ammo)
        GiveItem(WorldInventory, Ammo.ItemDefinition, GetLateGameCount(Ammo.ItemDefinition, Ammo.Count * 4, Ammo.Count * 10));

    for (auto& Item : Loadout)
    {
        auto WeaponDefinition = Item.ItemDefinition->IsA(UFortGadgetItemDefinition::StaticClass()) ? ((UFortGadgetItemDefinition*)Item.ItemDefinition)->GetWeaponItemDefinition()
                                : Item.ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()) ? (UFortWeaponItemDefinition*)Item.ItemDefinition
                                                                                                      : nullptr;
        auto Stats = GetStats(WeaponDefinition);

        GiveItem(WorldInventory, Item.ItemDefinition, Item.Count, Stats ? Stats->ClipSize : 0);
    }
}

static void ServerAcknowledgePossession(UObject* Context, FFrame& Stack)
{
    APawn* P;

    Stack.StepCompiledIn(&P);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!P)
        return;

    auto bAlreadyAcknowledged = PlayerController->AcknowledgedPawn == P;

    ((void (*)(AFortPlayerControllerAthena*, APawn*))AFortPlayerController::GetDefaultObj()->VTable[0x135])(PlayerController, P);

    if (bAlreadyAcknowledged || P != PlayerController->Pawn)
        return;

    ApplyCosmeticLoadoutToPawn(PlayerController, P);

    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Num = WorldInventory->Inventory.ReplicatedEntries.Num();

    ApplyOutsideSafeZoneEffect(((AFortPlayerStateAthena*)PlayerController->PlayerState)->AbilitySystemComponent);
    InitializePlayerGameplayAbilities(PlayerController);

    if (Num != 0)
    {
        if (bLateGame)
        {
            std::vector<FGuid> GuidsToRemove;

            for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
            {
                if (CanBeDropped((UFortItemDefinition*)Entry.ItemDefinition))
                    GuidsToRemove.push_back(Entry.ItemGuid);
            }

            for (auto& Guid : GuidsToRemove)
                Remove(WorldInventory, Guid);

            ((AFortPawn*)P)->SetShield(100.f);
            GiveLateGameLoadout(PlayerController);
        }

        return;
    }

    static auto DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

    auto Pickaxe = PlayerController->CosmeticLoadoutPC.Pickaxe && PlayerController->CosmeticLoadoutPC.Pickaxe->WeaponDefinition ? (UFortItemDefinition*)PlayerController->CosmeticLoadoutPC.Pickaxe->WeaponDefinition
                                                                                                                                 : DefaultPickaxe;

    if (GameMode->StartingItems.Num() == 0)
    {
        static auto WallBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall");
        static auto FloorBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor");
        static auto StairBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W");
        static auto ConeBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS");
        static auto EditTool = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/EditTool.EditTool");

        GiveItem(WorldInventory, Pickaxe);
        GiveItem(WorldInventory, WallBuild);
        GiveItem(WorldInventory, FloorBuild);
        GiveItem(WorldInventory, StairBuild);
        GiveItem(WorldInventory, ConeBuild);
        GiveItem(WorldInventory, EditTool);
    }
    else
    {
        GiveItem(WorldInventory, Pickaxe);

        for (auto& StartingItem : GameMode->StartingItems)
        {
            auto Item = (UFortItemDefinition*)StartingItem.Item;

            if (Item && Item->ItemType == EFortItemType::WeaponHarvest)
                continue;

            if (StartingItem.Count && !Item->IsA(UFortSmartBuildingItemDefinition::StaticClass()))
                GiveItem(WorldInventory, Item, StartingItem.Count);
        }
    }

    UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization((AFortPlayerState*)PlayerController->PlayerState);
}

static void ServerExecuteInventoryItem(UObject* Context, FFrame& Stack)
{
    FGuid ItemGuid;

    Stack.StepCompiledIn(&ItemGuid);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !PlayerController->MyFortPawn)
        return;

    FFortItemEntry* Entry = nullptr;

    for (auto& ReplicatedEntry : ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries)
    {
        if (ReplicatedEntry.ItemGuid == ItemGuid)
        {
            Entry = &ReplicatedEntry;
            break;
        }
    }

    if (!Entry)
        return;

    auto RealDef = (UFortItemDefinition*)Entry->ItemDefinition;
    auto Gadget = RealDef->IsA(UFortGadgetItemDefinition::StaticClass()) ? (UFortGadgetItemDefinition*)RealDef : nullptr;
    auto UncastedDef = Gadget ? Gadget->GetWeaponItemDefinition() : RealDef;

    if (!UncastedDef || !UncastedDef->IsA(UFortWeaponItemDefinition::StaticClass()))
        return;

    auto ItemDefinition = (UFortWeaponItemDefinition*)UncastedDef;
    auto Weapon = PlayerController->MyFortPawn->EquipWeaponDefinition(ItemDefinition, ItemGuid, Entry->TrackerGuid, false);

    if (!Weapon)
        return;

    if (Weapon->IsA(AFortDecoTool::StaticClass()))
    {
        ((AFortDecoTool*)Weapon)->SetDecoObjectPreview(ItemDefinition, true);

        if (Weapon->IsA(AFortDecoTool_ContextTrap::StaticClass()))
            ((AFortDecoTool_ContextTrap*)Weapon)->ContextTrapItemDefinition = (UFortContextTrapItemDefinition*)ItemDefinition;
    }

    Weapon->ForceNetUpdate();

    if (Gadget ? !Gadget->bValidForLastEquipped : !ItemDefinition->bValidForLastEquipped)
        return;

    *(FGuid*)(uint64(&PlayerController->MyFortPawn->VehicleInputComponent) - 0x20) = ItemGuid;
}

static void SetEditingPlayer(ABuildingSMActor* Building, AFortPlayerStateAthena* NewEditingPlayer)
{
    if (Building->Role != ENetRole::ROLE_Authority || (Building->EditingPlayer && NewEditingPlayer))
        return;

    Building->SetNetDormancy(NewEditingPlayer ? ENetDormancy::DORM_Awake : ENetDormancy::DORM_DormantAll);
    Building->ForceNetUpdate();

    auto Owner = Building->EditingPlayer ? Building->EditingPlayer->Owner : (NewEditingPlayer ? NewEditingPlayer->Owner : nullptr);

    if (!Building->EditingPlayer && !NewEditingPlayer)
    {
        Building->EditingPlayer = NewEditingPlayer;
        Building->OnRep_EditingPlayer();
        return;
    }

    if (Owner && Owner->IsA(AFortPlayerControllerAthena::StaticClass()))
    {
        Building->EditingPlayer = NewEditingPlayer;
        Building->OnRep_EditingPlayer();
    }
}

static void ServerCreateBuildingActor(UObject* Context, FFrame& Stack)
{
    FCreateBuildingActorData CreateBuildingData;

    Stack.StepCompiledIn(&CreateBuildingData);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto BuildingClassIt = BuildingClassMap.find(CreateBuildingData.BuildingClassHandle);
    auto BuildingClass = BuildingClassIt != BuildingClassMap.end() ? BuildingClassIt->second : nullptr;
    FVector BuildLoc = CreateBuildingData.BuildLoc;
    FRotator BuildRot = CreateBuildingData.BuildRot;
    auto bMirrored = CreateBuildingData.bMirrored;
    auto BuildingClassData = CreateBuildingData.BuildingClassData;

    if (!BuildingClass)
        return;

    BuildingClassData.BuildingClass = BuildingClass;

    static auto CanAffordToPlaceBuildableClass = (bool (*)(AFortPlayerControllerAthena*, FBuildingClassData, bool))(ImageBase + 0x9FF8A14);
    static auto CantBuild = (int64 (*)(UWorld*, TSubclassOf<AActor>&, FVector, FRotator, bool, TArray<ABuildingSMActor*>*, char*))(ImageBase + 0x987C5E0);
    static auto PayBuildableClassPlacementCost = (int32 (*)(AFortPlayerControllerAthena*, FBuildingClassData))(ImageBase + 0xA0279B8);

    if (!CanAffordToPlaceBuildableClass(PlayerController, BuildingClassData, false))
        return;

    TSubclassOf<AActor> BuildingActorClass = BuildingClass;
    TArray<ABuildingSMActor*> BuildingsToRemove;
    char bCanBuild;

    if (CantBuild(UWorld::GetWorld(), BuildingActorClass, BuildLoc, BuildRot, bMirrored, &BuildingsToRemove, &bCanBuild))
        return;

    for (auto BuildingToRemove : BuildingsToRemove)
        BuildingToRemove->K2_DestroyActor();

    BuildingsToRemove.Free();

    auto Building = SpawnActorUnfinished<ABuildingSMActor>(BuildingClass, BuildLoc, BuildRot, PlayerController);

    if (!Building)
        return;

    Building->InitializeKismetSpawnedBuildingActor(Building, PlayerController, true, nullptr, false);
    FinishSpawnActor(Building, BuildLoc, BuildRot);

    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    Building->CurrentBuildingLevel = BuildingClassData.UpgradeLevel;
    Building->OnRep_CurrentBuildingLevel();
    Building->SetMirrored(bMirrored);
    Building->bPlayerPlaced = true;
    Building->Team = (EFortTeam)PlayerState->TeamIndex;
    Building->TeamIndex = (uint8)Building->Team;

    if (!PlayerController->bBuildFree)
        PayBuildableClassPlacementCost(PlayerController, BuildingClassData);

    SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::Build, 1, true);
}

static void ServerBeginEditingBuildingActor(UObject* Context, FFrame& Stack)
{
    ABuildingSMActor* BuildingActorToEdit;

    Stack.StepCompiledIn(&BuildingActorToEdit);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !PlayerController->MyFortPawn || !BuildingActorToEdit || !BuildingActorToEdit->IsA(ABuildingSMActor::StaticClass()))
        return;

    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    if (!PlayerState)
        return;

    SetEditingPlayer(BuildingActorToEdit, PlayerState);

    auto Pawn = PlayerController->MyFortPawn;

    if (!Pawn->CurrentWeapon || !Pawn->CurrentWeapon->IsA(AFortWeap_EditingTool::StaticClass()))
    {
        for (auto& Entry : ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries)
        {
            if (Entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass())
            {
                Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Entry.ItemDefinition, Entry.ItemGuid, Entry.TrackerGuid, false);
                break;
            }
        }
    }

    if (Pawn->CurrentWeapon && Pawn->CurrentWeapon->IsA(AFortWeap_EditingTool::StaticClass()))
    {
        auto EditTool = (AFortWeap_EditingTool*)Pawn->CurrentWeapon;
        EditTool->EditActor = BuildingActorToEdit;
        EditTool->ForceNetUpdate();
        EditTool->OnRep_EditActor();
    }
}

static void ServerEditBuildingActor(UObject* Context, FFrame& Stack)
{
    ABuildingSMActor* BuildingActorToEdit;
    TSubclassOf<ABuildingSMActor> NewBuildingClass;
    uint8 RotationIterations;
    bool bMirrored;

    Stack.StepCompiledIn(&BuildingActorToEdit);
    Stack.StepCompiledIn(&NewBuildingClass);
    Stack.StepCompiledIn(&RotationIterations);
    Stack.StepCompiledIn(&bMirrored);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !BuildingActorToEdit || !NewBuildingClass || !BuildingActorToEdit->IsA(ABuildingSMActor::StaticClass()) || !NewBuildingClass->IsSubclassOf(ABuildingSMActor::StaticClass()) ||
        !((ABuildingSMActor*)NewBuildingClass->ClassDefaultObject)->bIsPlayerBuildable ||
        BuildingActorToEdit->EditingPlayer != PlayerController->PlayerState || BuildingActorToEdit->bDestroyed)
        return;

    SetEditingPlayer(BuildingActorToEdit, nullptr);

    static auto ReplaceBuildingActor = (ABuildingSMActor* (*)(ABuildingSMActor*, uint32, TSubclassOf<ABuildingSMActor>&, uint32, int32, bool, AFortPlayerControllerAthena*))(ImageBase + 0x933D860);

    if (auto NewBuild = ReplaceBuildingActor(BuildingActorToEdit, 1, NewBuildingClass, BuildingActorToEdit->CurrentBuildingLevel, RotationIterations, bMirrored, PlayerController))
        NewBuild->bPlayerPlaced = true;
}

static void ServerEndEditingBuildingActor(UObject* Context, FFrame& Stack)
{
    ABuildingSMActor* BuildingActorToStopEditing;

    Stack.StepCompiledIn(&BuildingActorToStopEditing);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !PlayerController->MyFortPawn || !BuildingActorToStopEditing || !BuildingActorToStopEditing->IsA(ABuildingSMActor::StaticClass()) ||
        BuildingActorToStopEditing->EditingPlayer != PlayerController->PlayerState || BuildingActorToStopEditing->bDestroyed)
        return;

    SetEditingPlayer(BuildingActorToStopEditing, nullptr);

    FFortItemEntry* EditToolEntry = nullptr;

    for (auto& Entry : ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass())
        {
            EditToolEntry = &Entry;
            break;
        }
    }

    if (!EditToolEntry)
        return;

    for (auto Weapon : PlayerController->MyFortPawn->CurrentWeaponList)
    {
        if (Weapon->ItemEntryGuid != EditToolEntry->ItemGuid)
            continue;

        if (auto EditTool = (AFortWeap_EditingTool*)Weapon)
        {
            EditTool->EditActor = nullptr;
            EditTool->ForceNetUpdate();
            EditTool->OnRep_EditActor();
        }

        break;
    }
}

static void ServerRepairBuildingActor(UObject* Context, FFrame& Stack)
{
    ABuildingSMActor* BuildingActorToRepair;

    Stack.StepCompiledIn(&BuildingActorToRepair);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !BuildingActorToRepair || !BuildingActorToRepair->IsA(ABuildingSMActor::StaticClass()))
        return;

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Price = (int32)std::floor((10.f * (1.f - BuildingActorToRepair->GetHealthPercent())) * 0.75f);
    auto ResourceItemDefinition = UFortKismetLibrary::K2_GetResourceItemDefinition(BuildingActorToRepair->ResourceType);
    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;

    for (auto Instance : WorldInventory->Inventory.ItemInstances)
    {
        if (Instance->ItemEntry.ItemDefinition == ResourceItemDefinition)
        {
            Item = Instance;
            break;
        }
    }

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition == ResourceItemDefinition)
        {
            ItemEntry = &Entry;
            break;
        }
    }

    if (!Item || ItemEntry->Count - Price < 0)
        return;

    ItemEntry->Count -= Price;

    if (ItemEntry->Count <= 0)
        Remove(WorldInventory, ItemEntry->ItemGuid);
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(WorldInventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }

    BuildingActorToRepair->RepairBuilding(PlayerController, Price);
}

static void ServerAttemptInventoryDrop(UObject* Context, FFrame& Stack)
{
    FGuid ItemGuid;
    int32 Count;
    bool bTrash;

    Stack.StepCompiledIn(&ItemGuid);
    Stack.StepCompiledIn(&Count);
    Stack.StepCompiledIn(&bTrash);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !PlayerController->Pawn)
        return;

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Pawn = PlayerController->Pawn;
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

    if (!Item || !ItemEntry)
        return;

    if (Count <= 0 || Count > ItemEntry->Count)
        Count = ItemEntry->Count;

    ItemEntry->Count -= Count;

    auto FinalLoc = Pawn->K2_GetActorLocation();

    static auto WID_Launcher_Petrol = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Weapons/Prototype/WID_Launcher_Petrol.WID_Launcher_Petrol");

    if (ItemEntry->ItemDefinition == WID_Launcher_Petrol)
    {
        static auto BGA_Petrol_PickupClass = FindObject<UClass>(L"/Game/Athena/Items/Weapons/Prototype/PetrolPump/BGA_Petrol_Pickup.BGA_Petrol_Pickup_C");

        SpawnActor<AActor>(BGA_Petrol_PickupClass, FinalLoc);
        Remove(WorldInventory, ItemGuid);
        return;
    }

    FinalLoc = FinalLoc + Pawn->GetActorForwardVector() * 450.f;
    FinalLoc.Z += 50.f;

    auto RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
    auto FinalAngle = (RandomAngleVariation + 360.f / ((float)rand() * 0.00015259254737998596f)) * 0.017453292519943295f;

    FinalLoc.X += cos(FinalAngle) * 100.f;
    FinalLoc.Y += sin(FinalAngle) * 100.f;

    SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *ItemEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, PlayerController->MyFortPawn,
                Count, true, true, true, nullptr, FinalLoc);

    if (ItemEntry->Count <= 0)
        Remove(WorldInventory, ItemGuid);
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(WorldInventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }
}

static void ServerPlayEmoteItem(UObject* Context, FFrame& Stack)
{
    UFortMontageItemDefinitionBase* EmoteAsset;
    float EmoteRandomNumber;

    Stack.StepCompiledIn(&EmoteAsset);
    Stack.StepCompiledIn(&EmoteRandomNumber);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    if (!PlayerController || !PlayerController->MyFortPawn || !EmoteAsset)
        return;

    auto Pawn = (AFortPlayerPawnAthena*)PlayerController->MyFortPawn;
    auto AbilitySystemComponent = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->AbilitySystemComponent;
    auto QuestManager = PlayerController->GetQuestManager(ESubGame::Athena);
    UObject* AbilityToUse = nullptr;

    if (PlayerController->Pawn->IsA(AFortCharacterVehicle::StaticClass()))
        AbilitySystemComponent = ((AFortCharacterVehicle*)PlayerController->Pawn)->OverrideAbilitySystemComponent;

    if (EmoteAsset->IsA(UAthenaSprayItemDefinition::StaticClass()))
    {
        static auto SprayAbilityClass = FindObject<UClass>(L"/Game/Abilities/Sprays/GAB_Spray_Generic.GAB_Spray_Generic_C");
        AbilityToUse = SprayAbilityClass->ClassDefaultObject;

        SendStatEvent(QuestManager, PlayerController, EFortQuestObjectiveStatEvent::Spray, 1, true);
    }
    else if (EmoteAsset->IsA(UAthenaToyItemDefinition::StaticClass()))
    {
        auto ToySpawnAbility = ((UAthenaToyItemDefinition*)EmoteAsset)->ToySpawnAbility.Get();
        AbilityToUse = ToySpawnAbility ? ToySpawnAbility->ClassDefaultObject : nullptr;

        SendStatEvent(QuestManager, PlayerController, EFortQuestObjectiveStatEvent::Toy, 1, true);
    }
    else if (EmoteAsset->IsA(UAthenaDanceItemDefinition::StaticClass()))
    {
        static auto EmoteAbilityClass = FindObject<UClass>(L"/Game/Abilities/Emotes/GAB_Emote_Generic.GAB_Emote_Generic_C");

        auto DanceAsset = (UAthenaDanceItemDefinition*)EmoteAsset;
        auto CustomAbility = DanceAsset->CustomDanceAbility.Get();

        Pawn->bMovingEmote = DanceAsset->bMovingEmote;
        Pawn->EmoteWalkSpeed = DanceAsset->WalkForwardSpeed;
        Pawn->bMovingEmoteForwardOnly = DanceAsset->bMoveForwardOnly;
        Pawn->bMovingEmoteFollowingOnly = DanceAsset->bMoveFollowingOnly;
        AbilityToUse = (CustomAbility ? CustomAbility : EmoteAbilityClass)->ClassDefaultObject;

        SendStatEvent(QuestManager, PlayerController, EFortQuestObjectiveStatEvent::Emote, 1, true);
    }

    if (!AbilityToUse)
        return;

    static auto ConstructAbilitySpec = (void (*)(FGameplayAbilitySpec*, const UObject*, int, int, UObject*))(ImageBase + 0x7B33CD8);
    static auto GiveAbilityAndActivateOnce = (void (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle*, FGameplayAbilitySpec*, void*))(ImageBase + 0x7AF6ED8);

    auto Spec = (FGameplayAbilitySpec*)calloc(1, sizeof(FGameplayAbilitySpec));
    FGameplayAbilitySpecHandle Handle;

    ConstructAbilitySpec(Spec, AbilityToUse, 1, -1, EmoteAsset);
    GiveAbilityAndActivateOnce(AbilitySystemComponent, &Handle, Spec, nullptr);

    free(Spec);

    auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
    Pawn->LastReplicatedEmoteExecuted = EmoteAsset;
    Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);
}

const UClass* ApplyOutsideSafeZoneEffect(UAbilitySystemComponent* AbilitySystemComponent, bool bApply)
{
    static auto Effect = FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C");

    for (auto& ActiveEffect : AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal)
    {
        if (ActiveEffect.Spec.Def && ActiveEffect.Spec.Def->IsA(Effect))
            return Effect;
    }

    if (!bApply)
        return nullptr;

    auto ActiveEffectHandle = AbilitySystemComponent->BP_ApplyGameplayEffectToSelf((UClass*)Effect, 0.f, FGameplayEffectContextHandle());

    AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(ActiveEffectHandle, FGameplayTag{ FName(L"SetByCaller.StormCampingDamage") }, 1.f);

    return ActiveEffectHandle.Handle != -1 ? Effect : nullptr;
}

static void ApplySiphon(AFortPlayerStateAthena* KillerPlayerState, AFortPlayerPawnAthena* KillerPawn, AFortPlayerControllerAthena* KillerPlayerController)
{
    static UClass* HealthEffect = nullptr;
    static UClass* ShieldEffect = nullptr;
    static bool bResolvedEffects = false;

    if (!bResolvedEffects)
    {
        bResolvedEffects = true;
        HealthEffect = FindObject<UClass>(L"/Game/Athena/Playlists/Showdown/Siphon/GE_SD_SiphonEffect_Heal.GE_SD_SiphonEffect_Heal_C");
        ShieldEffect = FindObject<UClass>(L"/Game/Athena/Playlists/Showdown/Siphon/GE_SD_SiphonEffect_Shield.GE_SD_SiphonEffect_Shield_C");

        if (!HealthEffect || !ShieldEffect)
        {
            HealthEffect = FindObject<UClass>(L"/Game/Athena/Heroes/Abilities/Siphon/GE_SiphonEffect_Heal.GE_SiphonEffect_Heal_C");
            ShieldEffect = FindObject<UClass>(L"/Game/Athena/Heroes/Abilities/Siphon/GE_SiphonEffect_Shield.GE_SiphonEffect_Shield_C");
        }
    }

    auto AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent;

    if (!KillerPawn->IsDBNO())
    {
        auto Health = KillerPawn->GetHealth();
        auto MaxHealth = KillerPawn->GetMaxHealth();
        auto Shield = KillerPawn->GetShield();
        auto MaxShield = KillerPawn->GetMaxShield();
        auto Pool = 50.f;
        float HealthHeal = 0.f;
        float ShieldHeal = 0.f;

        if (MaxHealth > Health)
        {
            HealthHeal = MaxHealth - Health < Pool ? MaxHealth - Health : Pool;
            Pool -= ceilf(HealthHeal);
        }

        if (MaxShield > Shield && Pool > 0.f)
            ShieldHeal = floorf(Pool) < MaxShield - Shield ? floorf(Pool) : MaxShield - Shield;

        if (HealthHeal > 0.f)
        {
            if (HealthEffect)
                AbilitySystemComponent->BP_ApplyGameplayEffectSpecToSelf(UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(
                    AbilitySystemComponent->MakeOutgoingSpec(HealthEffect, 1.f, AbilitySystemComponent->MakeEffectContext()), FGameplayTag{ FName(L"SetByCaller.Healing") }, HealthHeal));

            if (KillerPawn->GetHealth() < Health + HealthHeal - 0.5f)
                KillerPawn->SetHealth(Health + HealthHeal);
        }

        if (ShieldHeal > 0.f)
        {
            if (ShieldEffect)
                AbilitySystemComponent->BP_ApplyGameplayEffectSpecToSelf(UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(
                    AbilitySystemComponent->MakeOutgoingSpec(ShieldEffect, 1.f, AbilitySystemComponent->MakeEffectContext()), FGameplayTag{ FName(L"SetByCaller.Shield") }, ShieldHeal));

            if (KillerPawn->GetShield() < Shield + ShieldHeal - 0.5f)
                KillerPawn->SetShield(Shield + ShieldHeal);
        }
    }

    static auto SiphonCueTag = FGameplayTag{ FName(L"GameplayCue.Shield.PotionConsumed") };

    auto EffectContext = AbilitySystemComponent->MakeEffectContext();
    AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(SiphonCueTag, FPredictionKey{}, EffectContext);
    AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(SiphonCueTag, FPredictionKey{}, EffectContext);

    auto WorldInventory = KillerPlayerController ? (AFortInventory*)KillerPlayerController->WorldInventory.ObjectPointer : nullptr;

    if (!WorldInventory)
        return;

    for (auto ResourceType : { EFortResourceType::Wood, EFortResourceType::Stone, EFortResourceType::Metal })
        AddItemCount(WorldInventory, UFortKismetLibrary::K2_GetResourceItemDefinition(ResourceType), 50);
}

static void GiveVictoryCrown(AFortPlayerControllerAthena* PlayerController)
{
    auto Crown = FindObject<UFortItemDefinition>(L"/VictoryCrownsGameplay/Items/AGID_VictoryCrown.AGID_VictoryCrown");
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;

    if (!Crown || !WorldInventory)
        return;

    TArray<FFortItemEntryStateValue> StateValues{};
    FFortItemEntryStateValue Value{};

    Value.IntValue = 1;
    Value.StateType = EFortItemEntryState::ShouldShowItemToast;
    StateValues.Add(Value);

    GiveItem(WorldInventory, Crown, 1, 0, 0, true, true, 0, StateValues);
    StateValues.Free();
}

static void OnPawnDied(AFortPlayerControllerAthena* PlayerController, AFortPawn* DeadPawn, float Damage, const FGameplayTagContainer& DamageTags, const FGameplayEffectContextHandle& EffectContext,
                       AController* InstigatedBy, AActor* DamageCauser, AActor* KillerActor)
{
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    if (PlayerState && DeadPawn)
    {
        PlayerState->DeathInfo.DeathLocation = DeadPawn->K2_GetActorLocation();
        PlayerState->DeathInfo.bInitialized = true;
    }

    OnPawnDiedOG(PlayerController, DeadPawn, Damage, DamageTags, EffectContext, InstigatedBy, DamageCauser, KillerActor);
}

static void ClientOnPawnDied(AFortPlayerControllerAthena* PlayerController, FFortPlayerDeathReport& DeathReport)
{
    if (!PlayerController)
        return ClientOnPawnDiedOG(PlayerController, DeathReport);

    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Pawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;

    if (WorldInventory && Pawn && Pawn->bShouldDropItemsOnDeath)
    {
        std::vector<FGuid> GuidsToRemove;

        for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
        {
            if (!CanBeDropped((UFortItemDefinition*)Entry.ItemDefinition))
                continue;

            SpawnPickup(Pawn->K2_GetActorLocation(), Entry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::PlayerElimination, PlayerController->MyFortPawn);
            GuidsToRemove.push_back(Entry.ItemGuid);
        }

        for (auto& Guid : GuidsToRemove)
            Remove(WorldInventory, Guid);
    }

    auto KillerPlayerState = (AFortPlayerStateAthena*)DeathReport.KillerPlayerState;
    auto KillerPawn = (AFortPlayerPawnAthena*)DeathReport.KillerPawn.Get();
    auto KillerPlayerController = KillerPlayerState && KillerPlayerState->Owner && KillerPlayerState->Owner->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)KillerPlayerState->Owner : nullptr;

    PlayerState->PawnDeathLocation = Pawn ? Pawn->K2_GetActorLocation() : FVector();

    auto& DeathInfo = PlayerState->DeathInfo;

    memset((PBYTE)&DeathInfo, 0, sizeof(FDeathInfo));
    DeathInfo.bDBNO = Pawn ? Pawn->IsDBNO() : false;
    DeathInfo.DeathLocation = PlayerState->PawnDeathLocation;
    if (Pawn)
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(DeathInfo.DeathTags, *(FGameplayTagContainer*)(uint64(&Pawn->MoveSoundStimulusBroadcastInterval) + 0x10));
    DeathInfo.DeathClassSlot = (uint8)-1;
    DeathInfo.DeathCause = AFortPlayerStateAthena::ToDeathCause(DeathReport.Tags, DeathInfo.bDBNO);
    DeathInfo.FinisherOrDowner = KillerPlayerState ? KillerPlayerState : PlayerState;
    UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(DeathInfo.FinisherOrDownerTags, KillerPawn ? KillerPawn->GameplayTags : Pawn->GameplayTags);
    UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(DeathInfo.VictimTags, Pawn->GameplayTags);
    DeathInfo.Distance = Pawn ? (DeathInfo.DeathCause != EDeathCause::FallDamage ? (KillerPawn ? KillerPawn->GetDistanceTo(Pawn) : 0) : ((AFortPlayerPawnAthena*)PlayerController->MyFortPawn)->LastFallDistance) : 0;
    DeathInfo.bInitialized = true;
    PlayerState->OnRep_DeathInfo();

    if (KillerPlayerState && KillerPawn && KillerPawn->Controller && KillerPawn->Controller != PlayerController)
    {
        KillerPlayerState->KillScore++;
        KillerPlayerState->OnRep_Kills();
        KillerPlayerState->TeamKillScore++;
        KillerPlayerState->OnRep_TeamKillScore();
        KillerPlayerState->ClientReportKill(PlayerState);
        KillerPlayerState->ClientReportTeamKill(KillerPlayerState->TeamKillScore);
    }

    if (Pawn && KillerPlayerState && KillerPlayerState->AbilitySystemComponent && KillerPawn && KillerPawn->Controller != PlayerController)
        ApplySiphon(KillerPlayerState, KillerPawn, KillerPlayerController);

    ClientOnPawnDiedOG(PlayerController, DeathReport);
}

static void StartRespawnTimer(AFortPlayerControllerAthena* PlayerController, float RespawnTime, bool bNextRespawnInAir)
{
    static auto StartRespawnTimerOG = (void (*)(AFortPlayerControllerAthena*, float, bool))(ImageBase + 0x9031E9C);
    static auto SetServerTimeForRespawn = (void (*)(AFortPlayerControllerAthena*, float))(ImageBase + 0x903208C);

    SetServerTimeForRespawn(PlayerController, (float)(UWorld::GetWorld()->GameState->GetServerWorldTimeSeconds() + RespawnTime));
    PlayerController->ClientSetDeathReport(PlayerController->PlayerDeathReport);

    StartRespawnTimerOG(PlayerController, RespawnTime, bNextRespawnInAir);
}

static void (*OnPawnDeathNoRespawnOG)(AFortGameModeAthena*, AFortPlayerPawnAthena*);
static void OnPawnDeathNoRespawn(AFortGameModeAthena* GameMode, AFortPlayerPawnAthena* Pawn)
{
    OnPawnDeathNoRespawnOG(GameMode, Pawn);

    auto PlayerController = Pawn->Controller && Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Pawn->Controller : nullptr;

    if (!PlayerController)
        return;

    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
    auto& DeathReport = PlayerController->PlayerDeathReport;
    auto KillerPlayerState = (AFortPlayerStateAthena*)DeathReport.KillerPlayerState;
    auto KillerPawn = (AFortPlayerPawnAthena*)DeathReport.KillerPawn.Get();
    auto DeathReportDamageCauser = DeathReport.DamageCauser.Get();
    auto KillerPlayerController = KillerPlayerState && KillerPlayerState->Owner && KillerPlayerState->Owner->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)KillerPlayerState->Owner : nullptr;
    auto DeathCause = PlayerState->DeathInfo.DeathCause;

    AFortWeapon* DamageCauser = nullptr;

    if (DeathReportDamageCauser && DeathReportDamageCauser->IsA(AFortProjectileBase::StaticClass()))
    {
        auto Owner = DeathReportDamageCauser->Owner;

        if (Owner && Owner->IsA(AFortWeapon::StaticClass()))
            DamageCauser = (AFortWeapon*)Owner;
        else if (Owner && Owner->IsA(AFortPlayerControllerAthena::StaticClass()) && ((AFortPlayerControllerAthena*)Owner)->Pawn)
            DamageCauser = ((AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn)->CurrentWeapon;
        else if (Owner && Owner->IsA(AFortPlayerPawnAthena::StaticClass()))
            DamageCauser = ((AFortPlayerPawnAthena*)Owner)->CurrentWeapon;
    }
    else if (DeathReportDamageCauser && DeathReportDamageCauser->IsA(AFortWeapon::StaticClass()))
        DamageCauser = (AFortWeapon*)DeathReportDamageCauser;

    static auto RemoveFromAlivePlayers = (void (*)(AFortGameModeAthena*, AFortPlayerControllerAthena*, AFortPlayerStateAthena*, AFortPlayerPawnAthena*, UFortItemDefinition*, EDeathCause, char))(ImageBase + 0x8F9B900);

    static auto StartEndGamePhaseTeam = (bool (*)(AFortGameModeAthena*, uint8, AFortPlayerStateAthena*, int32, APawn*, UFortWeaponItemDefinition*, EDeathCause))(ImageBase + 0x8F9F004);

    auto KillerWeapon = DamageCauser && DamageCauser->IsA(AFortWeapon::StaticClass()) ? DamageCauser->WeaponData : nullptr;

    RemoveFromAlivePlayers(GameMode, PlayerController, KillerPlayerState == PlayerState ? nullptr : KillerPlayerState, KillerPawn, KillerWeapon, DeathCause, 0);

    auto Playlist = GameState->CurrentPlaylistInfo.BasePlaylist;
    auto TeamAliveIndex = Playlist ? PlayerState->TeamIndex - Playlist->DefaultFirstTeam : -1;

    if (PlayerState->Place == 0 && GameMode->TeamAlivePlayers.IsValidIndex(TeamAliveIndex) && GameMode->TeamAlivePlayers[TeamAliveIndex] <= 0)
        StartEndGamePhaseTeam(GameMode, PlayerState->TeamIndex, nullptr, GameState->TeamsLeft + 1, KillerPawn, KillerWeapon, DeathCause);

    TournamentPlayerEliminated(GameMode, PlayerState, KillerPlayerState == PlayerState ? nullptr : KillerPlayerState);
    SendPlacementUpdate(KillerPlayerController);

    Pawn->CharacterMovement->DisableMovement();

    if (KillerPlayerState && KillerPlayerState != PlayerState && KillerPlayerState->Place == 1)
    {
        if (KillerPlayerController)
        {
            KillerPlayerController->PlayWinEffects(KillerPawn, KillerWeapon, DeathCause, false, ERoundVictoryAnimation::Default);
            KillerPlayerController->ClientNotifyWon(KillerPawn, KillerWeapon, DeathCause);
            KillerPlayerController->ClientNotifyTeamWon(KillerPawn, KillerWeapon, DeathCause);
            GiveVictoryCrown(KillerPlayerController);
        }

        GameState->WinningTeam = KillerPlayerState->TeamIndex;
        GameState->OnRep_WinningTeam();
        GameState->WinningPlayerState = KillerPlayerState;
        GameState->OnRep_WinningPlayerState();
    }

    for (auto& PlacedPlayer : GameState->PlayerArray)
    {
        auto PlacedPlayerState = (AFortPlayerStateAthena*)PlacedPlayer;

        if (PlacedPlayerState && PlacedPlayerState->IsA(AFortPlayerStateAthena::StaticClass()) && PlacedPlayerState->Place > 0 && PlacedPlayerState->Owner && PlacedPlayerState->Owner->IsA(AFortPlayerControllerAthena::StaticClass()))
            EndBattleRoyaleGame((AFortPlayerControllerAthena*)PlacedPlayerState->Owner);
    }
}

static void ServerSpectateActor(AFortPlayerControllerAthena* PlayerController, AActor* NewViewTarget, bool bAllowStateChange)
{
    static auto SpectatingName = FName(L"Spectating");

    if (!PlayerController || (!bAllowStateChange && PlayerController->StateName != SpectatingName))
        return;

    auto PlayerState = (AFortPlayerStateZone*)PlayerController->PlayerState;

    if (!PlayerState || !PlayerState->IsA(AFortPlayerStateZone::StaticClass()) || !NewViewTarget)
        return;

    auto TargetPawnPlayerState = NewViewTarget->IsA(APawn::StaticClass()) ? ((APawn*)NewViewTarget)->PlayerState : nullptr;
    auto TargetPlayerState = TargetPawnPlayerState && TargetPawnPlayerState->IsA(AFortPlayerStateZone::StaticClass()) ? (AFortPlayerStateZone*)TargetPawnPlayerState : nullptr;

    if (TargetPlayerState && UFortRuntimeOptions::GetDefaultObj()->bEnableSpectatorUpdates)
    {
        static auto BeginSpectating = (void (*)(AFortPlayerStateZone*, AFortPlayerStateZone*))(ImageBase + 0xA0A3A64);
        BeginSpectating(PlayerState, TargetPlayerState);
    }

    ((void (*)(AFortPlayerControllerAthena*, FName))PlayerController->VTable[0xED])(PlayerController, SpectatingName);
    PlayerController->ClientGotoState(SpectatingName);

    FViewTargetTransitionParams TransitionParams{};
    TransitionParams.BlendTime = 0.f;
    TransitionParams.BlendFunction = EViewTargetBlendFunction::VTBlend_Cubic;
    TransitionParams.BlendExp = 2.f;
    TransitionParams.bLockOutgoing = false;

    PlayerController->SetViewTargetWithBlend(NewViewTarget, TransitionParams.BlendTime, TransitionParams.BlendFunction, TransitionParams.BlendExp, false);
    PlayerController->ClientSetViewTarget(NewViewTarget, TransitionParams);
}

void InternalPickup(AFortPlayerControllerAthena* PlayerController, FFortItemEntry* PickupEntry)
{
    if (!PickupEntry || !PickupEntry->ItemDefinition)
        return;

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Pawn = PlayerController->Pawn;
    auto MyFortPawn = PlayerController->MyFortPawn;
    auto PickupDefinition = (UFortItemDefinition*)PickupEntry->ItemDefinition;
    auto MaxStack = PickupDefinition->GetMaxStackSize(nullptr);
    int ItemCount = 0;

    auto TakesQuickbarSlot = [](const UFortItemDefinition* ItemDefinition)
    {
        return IsPrimaryQuickbar(ItemDefinition) && UFortItemFunctionLibrary::IsInventorySizeLimited(ItemDefinition);
    };

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (TakesQuickbarSlot((UFortItemDefinition*)Entry.ItemDefinition))
            ItemCount++;
    }

    auto FinalLoc = Pawn ? Pawn->K2_GetActorLocation() : FVector();
    auto ForwardVector = Pawn ? Pawn->GetActorForwardVector() : FVector();

    ForwardVector.Z = 0.0f;
    ForwardVector.Normalize();

    FinalLoc = FinalLoc + ForwardVector * 450.f;
    FinalLoc.Z += 50.f;

    auto FinalAngle = (((float)rand() * 0.00109866634f) - 18.f) * 0.017453292519943295f;

    FinalLoc.X += cos(FinalAngle) * 100.f;
    FinalLoc.Y += sin(FinalAngle) * 100.f;

    auto SendCollectStat = [&](int Count)
    {
        auto TargetTags = GetOwnedGameplayTags(PickupDefinition);

        SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::Collect, Count, true, PickupDefinition, TargetTags);

        TargetTags.GameplayTags.Free();
        TargetTags.ParentTags.Free();
    };

    auto DropLocation = [&]() { return Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50); };

    auto GiveOrSwap = [&]()
    {
        if (ItemCount >= 5 && TakesQuickbarSlot(PickupDefinition))
        {
            if (!MyFortPawn || !MyFortPawn->CurrentWeapon)
                return;

            if (!IsPrimaryQuickbar(MyFortPawn->CurrentWeapon->WeaponData))
            {
                SpawnPickup(DropLocation(), *PickupEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
                return;
            }

            for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
            {
                if (Entry.ItemGuid == MyFortPawn->CurrentWeapon->ItemEntryGuid)
                {
                    SpawnPickup(DropLocation(), Entry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
                    break;
                }
            }

            Remove(WorldInventory, MyFortPawn->CurrentWeapon->ItemEntryGuid);

            auto Item = GiveItem(WorldInventory, *PickupEntry, PickupEntry->Count, true);
            PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
            PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
        }
        else
            GiveItem(WorldInventory, *PickupEntry, PickupEntry->Count, true);

        SendCollectStat(PickupEntry->Count);
    };

    if (MaxStack <= 1)
        return GiveOrSwap();

    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;
    FFortItemEntry* FullItemEntry = nullptr;

    for (auto Instance : WorldInventory->Inventory.ItemInstances)
    {
        if (Instance->ItemEntry.ItemDefinition == PickupEntry->ItemDefinition && Instance->ItemEntry.Count < MaxStack)
        {
            Item = Instance;
            break;
        }
    }

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition != PickupEntry->ItemDefinition)
            continue;

        if (!ItemEntry && Entry.Count < MaxStack)
            ItemEntry = &Entry;
        else if (!FullItemEntry && Entry.Count >= MaxStack)
            FullItemEntry = &Entry;
    }

    if (!Item)
    {
        if (!ItemEntry && FullItemEntry)
        {
            SpawnPickup(DropLocation(), *PickupEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, MyFortPawn, PickupEntry->Count, true, true, true, nullptr, FinalLoc);
            return;
        }

        if (PickupEntry->Count > MaxStack)
        {
            auto OriginalCount = PickupEntry->Count;
            PickupEntry->Count = MaxStack;

            SpawnPickup(DropLocation(), *PickupEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, MyFortPawn, OriginalCount - MaxStack, true, true, true, nullptr, FinalLoc);
        }

        return GiveOrSwap();
    }

    auto OriginalItemCount = ItemEntry->Count;

    if ((ItemEntry->Count += PickupEntry->Count) > MaxStack)
    {
        auto OriginalCount = ItemEntry->Count;
        ItemEntry->Count = MaxStack;

        SpawnPickup(DropLocation(), *PickupEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, MyFortPawn, OriginalCount - MaxStack, true, true, true, nullptr, FinalLoc);
    }

    bool bFound = false;

    for (auto& StateValue : ItemEntry->StateValues)
    {
        if (StateValue.StateType == EFortItemEntryState::ShouldShowItemToast)
        {
            StateValue.IntValue = 1;
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        FFortItemEntryStateValue Value{};
        Value.IntValue = 1;
        Value.StateType = EFortItemEntryState::ShouldShowItemToast;
        ItemEntry->StateValues.Add(Value);
    }

    SendCollectStat(ItemEntry->Count - OriginalItemCount);
    Item->ItemEntry.Count = ItemEntry->Count;
    UpdateEntry(WorldInventory, *ItemEntry);
}

static void ServerDropAllItems(UObject* Context, FFrame& Stack)
{
    UFortItemDefinition* IgnoreItemDef;

    Stack.StepCompiledIn(&IgnoreItemDef);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;

    if (!WorldInventory || !PlayerController->MyFortPawn)
        return;

    auto Loc = PlayerController->MyFortPawn->K2_GetActorLocation();
    std::vector<FGuid> GuidsToRemove;

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition == IgnoreItemDef || !CanBeDropped((UFortItemDefinition*)Entry.ItemDefinition))
            continue;

        SpawnPickup(Loc, Entry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, PlayerController->MyFortPawn);
        GuidsToRemove.push_back(Entry.ItemGuid);
    }

    for (auto& Guid : GuidsToRemove)
        Remove(WorldInventory, Guid);
}

static void K2_OnUnEquip(AFortWeapon* Weapon)
{
    RemoveWeaponAbilities(Weapon);

    K2_OnUnEquipOG(Weapon);
}

static void SpawnToyInstance(UObject* Context, FFrame& Stack, AActor** Ret)
{
    TSubclassOf<AActor> ToyClass;
    FTransform SpawnPosition;

    Stack.StepCompiledIn(&ToyClass);
    Stack.StepCompiledIn(&SpawnPosition);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto Toy = SpawnActor<AActor>(ToyClass, SpawnPosition, PlayerController);

    PlayerController->ActiveToyInstances.Add(Toy);

    *Ret = Toy;
}

static void ServerCraftSchematic(UObject* Context, FFrame& Stack)
{
    FString ItemId;
    int32 PostCraftSlot;
    int32 CraftAmount;
    EFortItemTier RequestedTier;
    bool bIsQuickCrafted;

    Stack.StepCompiledIn(&ItemId);
    Stack.StepCompiledIn(&PostCraftSlot);
    Stack.StepCompiledIn(&CraftAmount);
    Stack.StepCompiledIn(&RequestedTier);
    Stack.StepCompiledIn(&bIsQuickCrafted);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto SchematicName = ItemId.ToString();
    ItemId.Free();

    if (SchematicName.rfind("Schematic:", 0) == 0)
        SchematicName.erase(0, 10);

    auto Schematic = UObject::FindObjectFast<UFortSchematicItemDefinition>(SchematicName);
    auto ResultItemDef = Schematic ? Schematic->GetResultWorldItemDefinition() : nullptr;

    if (!ResultItemDef)
        return;

    auto NewEntry = MakeItemEntry(ResultItemDef, Schematic->GetQuantityProduced(), 0);

    InternalPickup(PlayerController, NewEntry);
    free(NewEntry);
}

static void ServerRequestSeatChange(UObject* Context, FFrame& Stack)
{
    int32 TargetSeatIndex;

    Stack.StepCompiledIn(&TargetSeatIndex);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto Pawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;
    auto Vehicle = Pawn ? Pawn->GetVehicleActor() : nullptr;

    if (!Vehicle && Pawn && Pawn->IsA(AFortCharacterVehicle::StaticClass()))
        Vehicle = Pawn;

    auto SeatWeaponComponent = Vehicle ? (UFortVehicleSeatWeaponComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatWeaponComponent::StaticClass()) : nullptr;

    if (!SeatWeaponComponent)
        return callOG(PlayerController, Stack.CurrentNativeFunction, ServerRequestSeatChange, TargetSeatIndex);

    auto SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());
    int32 SeatIndex = -1;
    UFortWeaponItemDefinition* OldWeapon = nullptr;

    for (int i = 0; i < SeatComponent->PlayerSlots.Num(); i++)
    {
        if (SeatComponent->PlayerSlots[i].Player == PlayerController->MyFortPawn)
        {
            SeatIndex = i;
            break;
        }
    }

    for (auto& WeaponDefinition : SeatWeaponComponent->WeaponSeatDefinitions)
    {
        if (WeaponDefinition.SeatIndex == SeatIndex)
        {
            OldWeapon = WeaponDefinition.VehicleWeapon;
            break;
        }
    }

    callOG(PlayerController, Stack.CurrentNativeFunction, ServerRequestSeatChange, TargetSeatIndex);

    if (!OldWeapon)
        return;

    if (auto LastItem = Pawn->PreviousWeapon)
    {
        PlayerController->ServerExecuteInventoryItem(LastItem->ItemEntryGuid);
        PlayerController->ClientEquipItem(LastItem->ItemEntryGuid, true);
        return;
    }

    for (auto& Entry : ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemDefinition->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
        {
            PlayerController->ServerExecuteInventoryItem(Entry.ItemGuid);
            PlayerController->ClientEquipItem(Entry.ItemGuid, true);
            break;
        }
    }
}

static void ServerSetMultiProductCosmeticLoadout(UObject* Context, FFrame& Stack)
{
    FCosmeticLoadout LoadoutTemp{};
    auto& Loadout = Stack.StepCompiledInRef(&LoadoutTemp);
    TArray<FCosmeticLoadoutActiveArchetype> ArchetypesTemp{};
    auto& Archetypes = Stack.StepCompiledInRef(&ArchetypesTemp);

    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto CosmeticLoadoutComponent = PlayerController->CosmeticLoadoutComponent;

    if (!CosmeticLoadoutComponent)
        return;

    static auto SetCosmeticLoadout = (void (*)(UFortControllerComponent_CosmeticLoadout*, const FCosmeticLoadout*))(ImageBase + 0x94729E8);
    static auto BroadcastActiveArchetypesChanged = (void (*)(void*))(ImageBase + 0x4A3DF04);
    static auto GetCosmeticLoadout = (FFortAthenaLoadout* (*)(AFortPlayerControllerAthena*, FFortAthenaLoadout*, bool, bool*))(ImageBase + 0xA00838C);
    static auto DestructAthenaLoadout = (void (*)(FFortAthenaLoadout*))(ImageBase + 0x2289914);
    static auto CachedLoadoutCharacterInfoName = FName(L"CachedLoadoutCharacterInfo");

    SetCosmeticLoadout(CosmeticLoadoutComponent, &Loadout);

    auto& ActiveArchetypes = CosmeticLoadoutComponent->ActiveArchetypes;

    if (ActiveArchetypes.Num() != Archetypes.Num() || (Archetypes.Num() != 0 && memcmp(ActiveArchetypes.Data, Archetypes.Data, Archetypes.Num() * sizeof(FCosmeticLoadoutActiveArchetype)) != 0))
    {
        ActiveArchetypes.Free();

        for (auto& Archetype : Archetypes)
            ActiveArchetypes.Add(Archetype);

        BroadcastActiveArchetypesChanged((uint8*)CosmeticLoadoutComponent + 0x350);
    }

    alignas(16) uint8 LoadoutData[0x400];
    auto AthenaLoadout = (FFortAthenaLoadout*)LoadoutData;
    bool bFromComponent = false;

    GetCosmeticLoadout(PlayerController, AthenaLoadout, false, &bFromComponent);

    if (bFromComponent)
    {
        auto CosmeticLoadoutInterface = (void**)(uint64(PlayerController) + 0x868);
        ((void (*)(void**, FFortAthenaLoadout*))(*(void***)CosmeticLoadoutInterface)[4])(CosmeticLoadoutInterface, AthenaLoadout);
    }

    DestructAthenaLoadout(AthenaLoadout);

    if (!bFromComponent)
        return;

    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    PlayerState->CachedLoadoutCharacterInfo.CachedLoadoutCharacterUngated = PlayerController->CosmeticLoadoutPC.Character;
    UNetPushModelHelpers::MarkPropertyDirty(PlayerState, CachedLoadoutCharacterInfoName);

    auto MyFortPawn = PlayerController->MyFortPawn;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;

    if (MyFortPawn)
        ApplyCosmeticLoadoutToPawn(PlayerController, MyFortPawn);

    auto Pickaxe = PlayerController->CosmeticLoadoutPC.Pickaxe;

    if (!WorldInventory || !Pickaxe || !Pickaxe->WeaponDefinition)
        return;

    std::vector<FGuid> OldPickaxes;
    bool bHasPickaxe = false;
    bool bWasEquipped = false;

    for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (!Entry.ItemDefinition || ((UFortItemDefinition*)Entry.ItemDefinition)->ItemType != EFortItemType::WeaponHarvest)
            continue;

        if (Entry.ItemDefinition == Pickaxe->WeaponDefinition)
        {
            bHasPickaxe = true;
            continue;
        }

        if (MyFortPawn && MyFortPawn->CurrentWeapon && MyFortPawn->CurrentWeapon->ItemEntryGuid == Entry.ItemGuid)
            bWasEquipped = true;

        OldPickaxes.push_back(Entry.ItemGuid);
    }

    if (OldPickaxes.empty())
        return;

    for (auto& OldPickaxe : OldPickaxes)
        Remove(WorldInventory, OldPickaxe);

    UFortWorldItem* NewPickaxe = nullptr;

    if (bHasPickaxe)
    {
        for (auto Instance : WorldInventory->Inventory.ItemInstances)
        {
            if (Instance->ItemEntry.ItemDefinition == Pickaxe->WeaponDefinition)
            {
                NewPickaxe = Instance;
                break;
            }
        }
    }
    else
        NewPickaxe = GiveItem(WorldInventory, Pickaxe->WeaponDefinition);

    if (bWasEquipped && NewPickaxe && MyFortPawn)
        MyFortPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)NewPickaxe->ItemEntry.ItemDefinition, NewPickaxe->ItemEntry.ItemGuid, NewPickaxe->ItemEntry.TrackerGuid, false);
}

static void ServerOnMaterialSelection(UObject* Context, FFrame& Stack)
{
    EFortResourceType NewResourceType;
    EFortResourceLevel NewResourceLevel;

    Stack.StepCompiledIn(&NewResourceType);
    Stack.StepCompiledIn(&NewResourceLevel);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;

    PlayerController->CurrentResourceType = NewResourceType;
    PlayerController->CurrentResourceLevel = NewResourceLevel;
}

static void ServerPlaySquadQuickChatMessage(UObject* Context, FFrame& Stack)
{
    FAthenaQuickChatActiveEntry ChatEntryTemp{};
    auto& ChatEntry = Stack.StepCompiledInRef(&ChatEntryTemp);
    FUniqueNetIdRepl SenderIDTemp{};
    auto& SenderID = Stack.StepCompiledInRef(&SenderIDTemp);

    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerAthena*)Context;
    auto Bank = ChatEntry.Bank.Get();

    if (!Bank || !Bank->ChatOptions.IsValidIndex(ChatEntry.Index))
        return;

    auto& ChatOption = Bank->ChatOptions[ChatEntry.Index];
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    PlayerState->TeamMemberState = ChatOption.TeamCommType;
    PlayerState->ReplicatedTeamMemberState = ChatOption.TeamCommType;

    PlayerController->ServerPlayEmoteItem(ChatOption.EmojiItemDefinition, 0);

    PlayerState->OnRep_ReplicatedTeamMemberState();
}

static void ServerVehicleModSeatRpc(UObject* Context, FFrame& Stack)
{
    FGameplayTag RequestType;
    UFortVehicleModComponent* ModInstance;
    int32 SeatIndex;

    Stack.StepCompiledIn(&RequestType);
    Stack.StepCompiledIn(&ModInstance);
    Stack.StepCompiledIn(&SeatIndex);
    Stack.IncrementCode();

    auto PlayerController = (AFortPlayerControllerGameplay*)Context;
    auto Vehicle = ModInstance ? ModInstance->GetOwner() : nullptr;

    if (!Vehicle || !PlayerController->Pawn)
        return;

    static auto GetPawnAtSeatFn = IFortVehicleInterface::StaticClass()->FindFunction("GetPawnAtSeat");

    Params::FortVehicleInterface_GetPawnAtSeat GetPawnAtSeatParams{};
    GetPawnAtSeatParams.SeatIndex = SeatIndex;
    Vehicle->ProcessEvent(GetPawnAtSeatFn, &GetPawnAtSeatParams);

    if (GetPawnAtSeatParams.ReturnValue != PlayerController->Pawn)
        return;

    static auto EquipTurret = FName(L"Tag.VehicleMod.ServerRpcType.EquipTurret");
    static auto UnequipTurret = FName(L"Tag.VehicleMod.ServerRpcType.UnequipTurret");

    if (RequestType.TagName == EquipTurret)
        ModInstance->AuthorityTryEquipModSeatWeapon(SeatIndex);
    else if (RequestType.TagName == UnequipTurret)
        ModInstance->AuthorityTryUnequipModSeatWeapon(SeatIndex);
}

static bool RemoveInventoryItem(void* Interface, FGuid& ItemGuid, int32 Count, bool bForceRemoval, bool bForcePersistWhenEmpty)
{
    auto PlayerController = (AFortPlayerControllerAthena*)(__int64(Interface) - 0x878);
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

    if (!Item)
        return false;

    ItemEntry->Count -= (std::max)(Count, 0);

    if ((Count < 0 || ItemEntry->Count <= 0 || bForceRemoval) && !(bForcePersistWhenEmpty && Count >= 0 && !bForceRemoval))
        Remove(WorldInventory, ItemGuid);
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(WorldInventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }

    return true;
}

static void (*EnterAircraftOG)(UFortControllerComponent_Aircraft*, AActor*);

static void EnterAircraft(UFortControllerComponent_Aircraft* AircraftComponent, AActor* Aircraft)
{
    auto PlayerController = AircraftComponent->GetOwner();

    if (!PlayerController || !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()))
        return EnterAircraftOG(AircraftComponent, Aircraft);

    if (auto WorldInventory = (AFortInventory*)((AFortPlayerControllerAthena*)PlayerController)->WorldInventory.ObjectPointer)
    {
        std::vector<FGuid> GuidsToRemove;

        for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
        {
            if (CanBeDropped((UFortItemDefinition*)Entry.ItemDefinition))
                GuidsToRemove.push_back(Entry.ItemGuid);
        }

        for (auto& Guid : GuidsToRemove)
            Remove(WorldInventory, Guid);
    }

    EnterAircraftOG(AircraftComponent, Aircraft);
}

static void ServerAttemptAircraftJump(UFortControllerComponent_Aircraft* AircraftComponent, const FRotator& ClientRotation)
{
    static auto IsAircraftJumpAllowed = (bool (*)(UFortControllerComponent_Aircraft*))(ImageBase + 0x946741C);
    static auto KickFromAircraft = (void (*)(UFortControllerComponent_Aircraft*))(ImageBase + 0x94678A4);

    auto PlayerController = (AFortPlayerControllerAthena*)AircraftComponent->GetOwner();

    if (PlayerController && PlayerController->IsA(AFortAthenaAIBotController::StaticClass()))
    {
        if (IsAircraftJumpAllowed(AircraftComponent))
            KickFromAircraft(AircraftComponent);

        return;
    }

    if (!PlayerController || !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()) || !PlayerController->IsInAircraft())
        return;

    PlayerController->StateName = FName(L"Inactive");

    if (PlayerController->Pawn)
        PlayerController->UnPossess();

    UWorld::GetWorld()->AuthorityGameMode->RestartPlayer(PlayerController);
    PlayerController->SetControlRotation(ClientRotation);

    if (!bLateGame)
        return;

    auto Pawn = (AFortPawn*)PlayerController->Pawn;
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(PlayerController);
    auto Aircraft = GamePhaseLogic && GamePhaseLogic->Aircrafts_GameState.Num() > 0 ? GamePhaseLogic->Aircrafts_GameState.Data[0].Get() : nullptr;

    if (!Pawn || !Aircraft)
        return;

    auto Angle = UKismetMathLibrary::RandomFloatInRange(0., 360.);
    auto Distance = UKismetMathLibrary::RandomFloatInRange(0., 1000.);

    Pawn->SetShield(100.f);
    Pawn->K2_SetActorLocation(Aircraft->K2_GetActorLocation() + FVector(UKismetMathLibrary::DegCos(Angle) * Distance, UKismetMathLibrary::DegSin(Angle) * Distance, 0.), false, nullptr, true);
}

static void RefreshCosmeticLoadoutFromProfile(UFortControllerComponent_CosmeticLoadout* Component)
{
}

static void (*ServerAttemptInteractOG)(UObject*, FFrame&, void*);

static void ServerAttemptInteract(UObject* Context, FFrame& Stack, void* Result)
{
    auto ReceivingActor = *(AActor**)Stack.Locals;
    auto PlayerController = (AFortPlayerControllerAthena*)((UActorComponent*)Context)->GetOwner();

    if (!PlayerController || !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()) || !ReceivingActor)
        return ServerAttemptInteractOG(Context, Stack, Result);

    auto Pawn = PlayerController->MyFortPawn;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;

    if (ReceivingActor->IsA(ABuildingItemCollectorActor::StaticClass()))
    {
        auto CollectorActor = (ABuildingItemCollectorActor*)ReceivingActor;

        if (CollectorActor->IsA(ABuildingItemWeaponUpgradeActor::StaticClass()))
            return ServerAttemptInteractOG(Context, Stack, Result);

        CollectorActor->ControllingPlayer = PlayerController;

        FCollectorUnitInfo* Collection = nullptr;

        for (auto& ItemCollection : CollectorActor->ItemCollections)
        {
            if (ItemCollection.InputItem == CollectorActor->ActiveInputItem)
            {
                Collection = &ItemCollection;
                break;
            }
        }

        UFortWorldItem* Item = nullptr;
        auto Cost = Collection ? Collection->InputCount.Evaluate((float)CollectorActor->StartingGoalLevel) : 0.f;

        if (Collection && Cost > 0)
        {
            for (auto Instance : WorldInventory->Inventory.ItemInstances)
            {
                if (Instance->ItemEntry.ItemDefinition == Collection->InputItem)
                {
                    Item = Instance;
                    break;
                }
            }
        }

        if (!Collection || (Cost > 0 && (!Item || Item->ItemEntry.Count < (int)Cost)))
        {
            CollectorActor->bCurrentInteractionSuccess = false;
            CollectorActor->ControllingPlayer = nullptr;
            CollectorActor->BlueprintOnInteract(Pawn, EInteractionBeingAttempted::FirstInteraction);
            return ServerAttemptInteractOG(Context, Stack, Result);
        }

        if (Cost > 0)
        {
            for (auto& Entry : WorldInventory->Inventory.ReplicatedEntries)
            {
                if (Entry.ItemDefinition != Collection->InputItem)
                    continue;

                Entry.Count -= (int)Cost;

                if (Entry.Count <= 0)
                    Remove(WorldInventory, Entry.ItemGuid);
                else
                {
                    Item->ItemEntry.Count = Entry.Count;
                    UpdateEntry(WorldInventory, Entry);
                    Item->ItemEntry.bIsDirty = true;
                }

                break;
            }
        }

        CollectorActor->ClientPausedActiveInputItem = CollectorActor->ActiveInputItem;
        CollectorActor->bCurrentInteractionSuccess = true;
        CollectorActor->BlueprintOnInteract(Pawn, EInteractionBeingAttempted::FirstInteraction);
    }
    else if (ReceivingActor->IsA(ABuildingProp_LockDevice::StaticClass()))
    {
        auto LockDevice = (ABuildingProp_LockDevice*)ReceivingActor;
        LockDevice->CurrentLockState = ELockState::UNLOCKED;
        LockDevice->OnRep_CurrentLockState();
    }

    ServerAttemptInteractOG(Context, Stack, Result);
}

static void ServerNotifyStartLongUse(UObject* Context, FFrame& Stack)
{
    AActor* ReceivingActor;

    Stack.StepCompiledIn(&ReceivingActor);
    Stack.IncrementCode();

    auto Component = (UFortControllerComponent_Interaction*)Context;
    auto PlayerController = (AFortPlayerControllerAthena*)Component->GetOwner();
    auto InteractingPawn = PlayerController ? PlayerController->MyFortPawn : nullptr;

    if (!ReceivingActor || !InteractingPawn)
        return;

    auto& LongInteractValidateInfo = Component->LongInteractValidateInfo;
    LongInteractValidateInfo.Target = ReceivingActor;
    *(float*)(uint64(&LongInteractValidateInfo) + 0x8) = (float)UGameplayStatics::GetTimeSeconds(ReceivingActor);

    auto MultiInteractComponent = (UFortActorComponent_MultiInteract*)ReceivingActor->GetComponentByClass(UFortActorComponent_MultiInteract::StaticClass());

    if (MultiInteractComponent && MultiInteractComponent->IsMultipleInteractionEnabled())
        MultiInteractComponent->AddInteractingActor(InteractingPawn);

    if (auto InteractInterface = ReceivingActor->GetInterface(IFortInteractInterface::StaticClass()))
        ((void (*)(void*, AActor*))(*(void***)InteractInterface)[0x11])(InteractInterface, InteractingPawn);
}

static void ServerNotifyEndLongUse(UObject* Context, FFrame& Stack)
{
    AActor* ReceivingActor;
    bool bUseCompleted;

    Stack.StepCompiledIn(&ReceivingActor);
    Stack.StepCompiledIn(&bUseCompleted);
    Stack.IncrementCode();

    auto Component = (UFortControllerComponent_Interaction*)Context;
    auto PlayerController = (AFortPlayerControllerAthena*)Component->GetOwner();
    auto InteractingPawn = PlayerController ? PlayerController->MyFortPawn : nullptr;

    auto& LongInteractValidateInfo = Component->LongInteractValidateInfo;

    if (LongInteractValidateInfo.Target == ReceivingActor)
        LongInteractValidateInfo.Target = nullptr;

    if (!ReceivingActor || !InteractingPawn)
        return;

    auto MultiInteractComponent = (UFortActorComponent_MultiInteract*)ReceivingActor->GetComponentByClass(UFortActorComponent_MultiInteract::StaticClass());

    if (MultiInteractComponent && MultiInteractComponent->IsMultipleInteractionEnabled())
        MultiInteractComponent->RemoveInteractingActor(InteractingPawn);

    if (auto InteractInterface = ReceivingActor->GetInterface(IFortInteractInterface::StaticClass()))
        ((void (*)(void*, AActor*, bool))(*(void***)InteractInterface)[0x10])(InteractInterface, InteractingPawn, bUseCompleted);
}

void AFortPlayerController__PostLoad()
{
    auto PlayerControllerClass = AFortPlayerControllerAthena::StaticClass();
    auto DefaultPlayerControllerZone = AFortPlayerControllerZone::GetDefaultObj();

    Hook(ImageBase + 0x15EB0AC, GetPlayerViewPoint, GetPlayerViewPointOG);
    Hook<AFortPlayerControllerAthena>(uint32(0x12D), DefaultPlayerControllerZone->VTable[0x12D]);
    Hook<AFortPlayerControllerAthena>(uint32(0x28C), DefaultPlayerControllerZone->VTable[0x28C]);
    ExecHook(PlayerControllerClass->FindFunction("ServerAcknowledgePossession"), ServerAcknowledgePossession);
    ExecHook(PlayerControllerClass->FindFunction("ServerExecuteInventoryItem"), ServerExecuteInventoryItem);
    Hook<AFortPlayerControllerAthena>(uint32(0x283), AFortPlayerController::GetDefaultObj()->VTable[0x283]);
    ExecHook(PlayerControllerClass->FindFunction("ServerCreateBuildingActor"), ServerCreateBuildingActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerBeginEditingBuildingActor"), ServerBeginEditingBuildingActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerEditBuildingActor"), ServerEditBuildingActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerEndEditingBuildingActor"), ServerEndEditingBuildingActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerRepairBuildingActor"), ServerRepairBuildingActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerAttemptInventoryDrop"), ServerAttemptInventoryDrop);
    ExecHook(PlayerControllerClass->FindFunction("ServerPlayEmoteItem"), ServerPlayEmoteItem);
    Hook(ImageBase + 0x901FDFC, OnPawnDied, OnPawnDiedOG);
    Hook(ImageBase + 0xA06E3C0, ClientOnPawnDied, ClientOnPawnDiedOG);
    Hook(ImageBase + 0x8F98730, OnPawnDeathNoRespawn, OnPawnDeathNoRespawnOG);
    Rel32Hook(ImageBase + 0x906A4F5 + 1, StartRespawnTimer);
    Hook<AFortPlayerControllerAthena>(uint32(0x48C), ServerSpectateActor);
    ExecHook(PlayerControllerClass->FindFunction("ServerDropAllItems"), ServerDropAllItems);
    Hook(ImageBase + 0xA45DC2C, K2_OnUnEquip, K2_OnUnEquipOG);
    ExecHook(PlayerControllerClass->FindFunction("SpawnToyInstance"), SpawnToyInstance);
    ExecHook(PlayerControllerClass->FindFunction("ServerCraftSchematic"), ServerCraftSchematic);
    ExecHook(PlayerControllerClass->FindFunction("ServerRequestSeatChange"), ServerRequestSeatChange, ServerRequestSeatChangeOG);
    ExecHook(PlayerControllerClass->FindFunction("ServerSetMultiProductCosmeticLoadout"), ServerSetMultiProductCosmeticLoadout);
    ExecHook(PlayerControllerClass->FindFunction("ServerOnMaterialSelection"), ServerOnMaterialSelection);
    ExecHook(PlayerControllerClass->FindFunction("ServerPlaySquadQuickChatMessage"), ServerPlaySquadQuickChatMessage);
    ExecHook(AFortPlayerControllerGameplay::StaticClass()->FindFunction("ServerVehicleModSeatRpc"), ServerVehicleModSeatRpc);
    Hook(ImageBase + 0xA02DACC, RemoveInventoryItem);

    Hook(ImageBase + 0x945F42C, EnterAircraft, EnterAircraftOG);
    Hook<UFortControllerComponent_Aircraft>(uint32(0xAA), ServerAttemptAircraftJump);

    Hook(ImageBase + 0x947710C, RefreshCosmeticLoadoutFromProfile);

    ExecHook(UFortControllerComponent_Interaction::StaticClass()->FindFunction("ServerAttemptInteract"), ServerAttemptInteract, ServerAttemptInteractOG);
    ExecHook(UFortControllerComponent_Interaction::StaticClass()->FindFunction("ServerNotifyStartLongUse"), ServerNotifyStartLongUse);
    ExecHook(UFortControllerComponent_Interaction::StaticClass()->FindFunction("ServerNotifyEndLongUse"), ServerNotifyEndLongUse);
}
