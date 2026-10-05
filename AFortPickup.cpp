#include "pch.h"
#include "Shared.h"
#include "AFortPickup.h"
#include "AFortInventory.h"
#include "AFortPlayerController.h"
#include "AFortAthenaAIBotController.h"
#include "UAthenaAIServiceLoot.h"

INIT_MODULE(AFortPickup);
INIT_POSTLOAD(AFortPickup);
INIT_TICKER(AFortPickup);

static void SetPickupItems(AFortPickupAthena* Pickup)
{
    static auto SetPickupItemsFn = (void (*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t))(ImageBase + 0x9AC2504);

    TArray<FFortItemEntry> a{};
    SetPickupItemsFn(Pickup, &Pickup->PrimaryPickupItemEntry, &a, (uint8_t)EFortPickupSourceTypeFlag::Container, false, (uint8_t)EFortPickupSpawnSource::Chest);
}

AFortPickupAthena* SpawnPickup(FVector Loc, FFortItemEntry& Entry, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource, AFortPawn* Pawn, int OverrideCount, bool Toss, bool RandomRotation,
                               bool bCombine, const UClass* OverrideClass, FVector FinalLoc)
{
    auto NewPickup = SpawnActor<AFortPickupAthena>(OverrideClass ? OverrideClass : AFortPickupAthena::StaticClass(), Loc, {});
    if (!NewPickup)
        return nullptr;

    NewPickup->bRandomRotation = RandomRotation;
    if (Entry.Level != -1)
        NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;
    CopyWeaponModsFrom(NewPickup->PrimaryPickupItemEntry, Entry);

    SetPickupItems(NewPickup);
    NewPickup->PawnWhoDroppedPickup = Pawn;

    auto FinalLocation = Loc;

    if (FinalLoc.X || FinalLoc.Y || FinalLoc.Z)
        FinalLocation = FinalLoc;
    NewPickup->TossPickup(FinalLocation, Pawn, -1, Toss, true, SourceTypeFlag, SpawnSource);

    NewPickup->bTossedFromContainer = SpawnSource == EFortPickupSpawnSource::Chest || SpawnSource == EFortPickupSpawnSource::AmmoBox;
    if (NewPickup->bTossedFromContainer)
        NewPickup->OnRep_TossedFromContainer();

    RegisterPickupLoot(NewPickup);

    return NewPickup;
}

AFortPickupAthena* SpawnPickup(FVector Loc, const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource, AFortPawn* Pawn, bool Toss,
                               bool bRandomRotation, const UClass* OverrideClass)
{
    auto ItemEntry = MakeItemEntry(ItemDefinition, Count, -1);
    if (LoadedAmmo != -1)
        ItemEntry->LoadedAmmo = LoadedAmmo;

    auto Pickup = SpawnPickup(Loc, *ItemEntry, SourceTypeFlag, SpawnSource, Pawn, -1, Toss, true, bRandomRotation, OverrideClass);
    free(ItemEntry);
    return Pickup;
}

AFortPickupAthena* SpawnPickup(ABuildingContainer* Container, FFortItemEntry& Entry, AFortPawn* Pawn, int OverrideCount)
{
    auto ContainerLoc = Container->K2_GetActorLocation();
    auto& SpawnLocation = Container->LootSpawnLocation_Athena;
    auto Loc = ContainerLoc + (Container->GetActorForwardVector() * SpawnLocation.X) + (Container->GetActorRightVector() * SpawnLocation.Y) + (Container->GetActorUpVector() * SpawnLocation.Z);
    auto NewPickup = SpawnActor<AFortPickupAthena>(Loc, {});

    if (!NewPickup)
        return nullptr;

    NewPickup->bRandomRotation = true;
    NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;
    CopyWeaponModsFrom(NewPickup->PrimaryPickupItemEntry, Entry);

    SetPickupItems(NewPickup);

    NewPickup->PawnWhoDroppedPickup = Pawn;

    UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 10, (int32)std::clamp((float)rand() * 0.0003357036f, 0.f, 10.f), Container->LootTossConeHalfAngle_Athena,
                                                *(FRotator*)&Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, Container->bForceHidePickupMinimapIndicator);

    NewPickup->bTossedFromContainer = true;
    NewPickup->OnRep_TossedFromContainer();

    RegisterPickupLoot(NewPickup);

    return NewPickup;
}

template <typename T>
static T* PickWeighted(TArray<T*>& Map, float (*RandFunc)(float), bool bCheckZero = true)
{
    float TotalWeight = 0.0f;

    for (auto& Element : Map)
        TotalWeight += Element->Weight;

    float RandomNumber = RandFunc(TotalWeight);

    for (auto& Element : Map)
    {
        float Weight = Element->Weight;

        if (bCheckZero && Weight == 0)
            continue;

        if (RandomNumber <= Weight)
            return Element;

        RandomNumber -= Weight;
    }

    return nullptr;
}

static void SetupLDSForPackage(TArray<FFortItemEntry*>& LootDrops, FName Package, int i, FName TierGroup, int WorldLevel)
{
    TArray<FFortLootPackageData*> LPGroups{};

    for (auto const& Val : LootPackageMap[Package.ComparisonIndex])
    {
        if (!Val)
            continue;

        if (i != -1 && Val->LootPackageCategory != i)
            continue;

        if (WorldLevel >= 0)
        {
            if (Val->MaxWorldLevel >= 0 && WorldLevel > Val->MaxWorldLevel)
                continue;

            if (Val->MinWorldLevel >= 0 && WorldLevel < Val->MinWorldLevel)
                continue;
        }

        LPGroups.Add(Val);
    }

    if (LPGroups.Num() == 0)
        return;

    auto LootPackage = PickWeighted(LPGroups, [](float Total) { return ((float)rand() / 32767.f) * Total; });
    if (!LootPackage)
        return;

    if (LootPackage->LootPackageCall.Num() > 1)
    {
        for (int i = 0; i < LootPackage->Count; i++)
            SetupLDSForPackage(LootDrops, UKismetStringLibrary::Conv_StringToName(LootPackage->LootPackageCall), -1, TierGroup, WorldLevel);

        return;
    }

    auto ItemDefinition = LootPackage->ItemDefinition.Get();

    if (!ItemDefinition)
        return;

    FFortItemEntry* AmmoEntry = nullptr;

    if (ItemDefinition->IsA(UFortWorldItemDefinition::StaticClass()))
    {
        auto AmmoDefinition = ((UFortWorldItemDefinition*)ItemDefinition)->GetAmmoWorldItemDefinition_BP();

        if (AmmoDefinition && AmmoDefinition->IsA(UFortAmmoItemDefinition::StaticClass()))
        {
            auto SpawnAmmoData = FindObject<UFortWeaponPickupSpawnAmmoData>(L"/Game/Athena/Balance/Pickups/FortWeaponPickupSpawnAmmoData.FortWeaponPickupSpawnAmmoData");

            FGameplayTagContainer AmmoTags{};

            if (auto Interface = AmmoDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass()))
            {
                auto GetOwnedGameplayTags = (void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2];
                GetOwnedGameplayTags(Interface, &AmmoTags);
            }

            float AmmoCount = 0.f;
            for (auto& AmmoCountData : SpawnAmmoData->WeaponPickupAmmoCountArray)
            {
                if (AmmoTags.HasTag(AmmoCountData.AmmoItemDefinitionTag))
                {
                    AmmoCount = AmmoCountData.SpawnCount.Evaluate((float)WorldLevel);
                    break;
                }
            }

            auto Multiplier = SpawnAmmoData->DefaultWeaponAmmoMultiplier.Evaluate((float)WorldLevel);

            FGameplayTagContainer WeaponTags{};

            if (auto Interface = ItemDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass()))
            {
                auto GetOwnedGameplayTags = (void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2];
                GetOwnedGameplayTags(Interface, &WeaponTags);
            }

            for (auto& MultiplierData : SpawnAmmoData->WeaponPickupAmmoMultiplierOverrideArray)
            {
                if (WeaponTags.HasTag(MultiplierData.Tag))
                {
                    Multiplier = MultiplierData.CountMultiplier.Evaluate((float)WorldLevel);
                    break;
                }
            }

            auto FinalCount = AmmoCount * Multiplier;

            if (FinalCount > 0.f)
                AmmoEntry = MakeItemEntry(AmmoDefinition, (int)FinalCount, 0);

            AmmoTags.GameplayTags.Free();
            AmmoTags.ParentTags.Free();
            WeaponTags.GameplayTags.Free();
            WeaponTags.ParentTags.Free();
        }
    }

    bool found = false;
    for (auto& LootDrop : LootDrops)
    {
        if (LootDrop->ItemDefinition == (UItemDefinitionBase*)ItemDefinition && LootDrop->Count < ItemDefinition->GetMaxStackSize(nullptr))
        {
            LootDrop->Count += LootPackage->Count;

            if (LootDrop->Count > ItemDefinition->GetMaxStackSize(nullptr))
            {
                auto OGCount = LootDrop->Count;
                LootDrop->Count = ItemDefinition->GetMaxStackSize(nullptr);

                LootDrops.Add(MakeItemEntry(ItemDefinition, OGCount - ItemDefinition->GetMaxStackSize(nullptr), 0));
            }

            found = true;
            break;
        }

        if (AmmoEntry && AmmoEntry->ItemDefinition && LootDrop->ItemDefinition == AmmoEntry->ItemDefinition)
        {
            auto AmmoDefinition = (UFortItemDefinition*)AmmoEntry->ItemDefinition;

            LootDrop->Count += AmmoEntry->Count;

            if (LootDrop->Count > AmmoDefinition->GetMaxStackSize(nullptr))
            {
                auto OGCount = LootDrop->Count;
                LootDrop->Count = AmmoDefinition->GetMaxStackSize(nullptr);

                LootDrops.Add(MakeItemEntry(AmmoDefinition, OGCount - AmmoDefinition->GetMaxStackSize(nullptr), 0));
            }

            free(AmmoEntry);
            AmmoEntry = nullptr;
        }
    }

    if (!found && LootPackage->Count > 0)
        LootDrops.Add(MakeItemEntry(ItemDefinition, LootPackage->Count, 0));

    if (AmmoEntry && LootPackage->Count > 0)
        LootDrops.Add(AmmoEntry);
    LPGroups.Free();
}

void ChooseLootForContainer(TArray<FFortItemEntry*>& LootDrops, FName TierGroup, int LootTier, int WorldLevel)
{
    TArray<FFortLootTierData*> TierDataGroups{};

    for (auto const& Val : TierDataMap[TierGroup.ComparisonIndex])
        if (LootTier == -1 ? true : LootTier == Val->LootTier)
            TierDataGroups.Add(Val);

    auto LootTierData = PickWeighted(TierDataGroups, [](float Total) { return ((float)rand() / 32767.f) * Total; });

    if (!LootTierData)
        return;

    if (LootTierData->NumLootPackageDrops <= 0)
        return;

    int DropCount;
    if (LootTierData->NumLootPackageDrops < 1.f)
        DropCount = 1;
    else
    {
        DropCount = (int)((LootTierData->NumLootPackageDrops * 2.f) - .5f) >> 1;

        float RemainderSomething = LootTierData->NumLootPackageDrops - (float)DropCount;

        if (RemainderSomething > 0.0000099999997f)
            DropCount += RemainderSomething >= ((float)rand() / 32767.f);
    }

    auto& Weights = LootTierData->LootPackageCategoryWeightArray;
    auto& Maxes = LootTierData->LootPackageCategoryMaxArray;
    auto NumCategories = (std::min)(LootTierData->LootPackageCategoryMinArray.Num(), Weights.Num());

    TArray<int32> CategoryDrops{};
    int32 NumDrops = 0;

    for (int32 i = 0; i < NumCategories; i++)
    {
        CategoryDrops.Add(LootTierData->LootPackageCategoryMinArray[i]);
        NumDrops += CategoryDrops[i];
    }

    auto CanRollCategory = [&](int32 i) { return Weights[i] > 0 && (Maxes[i] < 0 || CategoryDrops[i] < Maxes[i]); };

    for (; NumDrops < DropCount; NumDrops++)
    {
        int32 SumWeights = 0;

        for (int32 i = 0; i < NumCategories; i++)
            if (CanRollCategory(i))
                SumWeights += Weights[i];

        if (SumWeights <= 0)
            break;

        auto RandomWeight = (int32)(((float)rand() / 32768.f) * SumWeights);

        for (int32 i = 0; i < NumCategories; i++)
        {
            if (!CanRollCategory(i))
                continue;

            if (RandomWeight < Weights[i])
            {
                CategoryDrops[i]++;
                break;
            }

            RandomWeight -= Weights[i];
        }
    }

    for (int32 i = 0; i < NumCategories; i++)
        for (int32 j = 0; j < CategoryDrops[i]; j++)
            SetupLDSForPackage(LootDrops, LootTierData->LootPackage, i, TierGroup, WorldLevel);

    CategoryDrops.Free();
    TierDataGroups.Free();
}

void SpawnFloorLootForContainer(const UClass* ContainerType)
{
    TArray<ABuildingContainer*> Containers;
    GetAll<ABuildingContainer>(ContainerType, Containers);

    for (auto& BuildingContainer : Containers)
        BuildingContainer->K2_DestroyActor();

    Containers.Free();
}

static void SpawnPickup(UObject* Context, FFrame& Stack, AFortPickup** Ret)
{
    UFortWorldItemDefinition* ItemDefinition;
    int32 NumberToSpawn;
    AFortPawn* TriggeringPawn;
    FVector Position;
    FVector Direction;
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&NumberToSpawn);
    Stack.StepCompiledIn(&TriggeringPawn);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.IncrementCode();

    auto Stats = ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()) ? GetStats((UFortWeaponItemDefinition*)ItemDefinition) : nullptr;

    *Ret = SpawnPickup(Position, ItemDefinition, NumberToSpawn, Stats ? Stats->ClipSize : 0, EFortPickupSourceTypeFlag::Other, EFortPickupSpawnSource::SupplyDrop);
}

struct FPendingConsumableQuery
{
    TWeakObjectPtr<ABGAConsumableSpawner> Spawner;
    TWeakObjectPtr<UEnvQueryInstanceBlueprintWrapper> Query;
    std::vector<std::pair<UClass*, int32>> Consumables;
    int32 Total;
    double StartTime;
};

static std::vector<FPendingConsumableQuery> PendingQueries;

void WorldReady(ABGAConsumableSpawner* Spawner)
{
    if (!Spawner || Spawner->Role != ENetRole::ROLE_Authority)
        return;

    auto World = UWorld::GetWorld();

    if (!World || !World->AuthorityGameMode || !World->AuthorityGameMode->IsA(AFortGameModeAthena::StaticClass()) || !World->GameState)
        return;

    auto QueryTemplate = Spawner->QueryTemplate;

    if (!QueryTemplate)
        return;

    TArray<FFortItemEntry*> LootDrops{};
    ChooseLootForContainer(LootDrops, Spawner->SpawnLootTierGroup);

    std::vector<std::pair<UClass*, int32>> Consumables;
    int32 Total = 0;

    for (auto& LootDrop : LootDrops)
    {
        auto WrapperDefinition = LootDrop->ItemDefinition && LootDrop->ItemDefinition->IsA(UBGAConsumableWrapperItemDefinition::StaticClass()) ? (UBGAConsumableWrapperItemDefinition*)LootDrop->ItemDefinition : nullptr;
        auto ConsumableClass = WrapperDefinition ? WrapperDefinition->ConsumableClass.Get() : nullptr;

        if (ConsumableClass)
        {
            auto Min = (int32)WrapperDefinition->MinNumberConsumablesToSpawn.Evaluate();
            auto Max = (int32)WrapperDefinition->MaxNumberConsumablesToSpawn.Evaluate();
            int32 Count = 0;

            for (int32 i = 0; i < LootDrop->Count; i++)
                Count += Max > Min ? Min + rand() % (Max - Min + 1) : Min;

            if (Count > 0)
            {
                Consumables.push_back({ ConsumableClass, Count });
                Total += Count;
            }
        }

        free(LootDrop);
    }

    LootDrops.Free();

    if (Total <= 0)
        return;

    auto Query = UEnvQueryManager::RunEQSQuery(Spawner, QueryTemplate, Spawner, EEnvQueryRunMode::AllMatching, UEnvQueryInstanceBlueprintWrapper::StaticClass());

    if (!Query)
        return;

    for (auto& Param : Spawner->QueryConfig)
        Query->SetNamedParam(Param.ParamName, Param.Value);

    PendingQueries.push_back({ TWeakObjectPtr<ABGAConsumableSpawner>(Spawner), TWeakObjectPtr<UEnvQueryInstanceBlueprintWrapper>(Query), std::move(Consumables), Total, UGameplayStatics::GetTimeSeconds(World) });
}

static void K2_SpawnPickupInWorld(UObject* Context, FFrame& Stack, AFortPickupAthena** Ret)
{
    UObject* WorldContextObject;
    UFortItemDefinition* ItemDefinition;
    int32 NumberToSpawn;
    FVector Position;
    FVector Direction;
    int32 OverrideMaxStackCount;
    bool bToss;
    bool bRandomRotation;
    bool bBlockedFromAutoPickup;
    int32 PickupInstigatorHandle;
    EFortPickupSourceTypeFlag SourceType;
    EFortPickupSpawnSource Source;
    AFortPlayerController* OptionalOwnerPC;
    bool bPickupOnlyRelevantToOwner;

    Stack.StepCompiledIn(&WorldContextObject);
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&NumberToSpawn);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.StepCompiledIn(&OverrideMaxStackCount);
    Stack.StepCompiledIn(&bToss);
    Stack.StepCompiledIn(&bRandomRotation);
    Stack.StepCompiledIn(&bBlockedFromAutoPickup);
    Stack.StepCompiledIn(&PickupInstigatorHandle);
    Stack.StepCompiledIn(&SourceType);
    Stack.StepCompiledIn(&Source);
    Stack.StepCompiledIn(&OptionalOwnerPC);
    Stack.StepCompiledIn(&bPickupOnlyRelevantToOwner);
    Stack.IncrementCode();

    *Ret = SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1, SourceType, Source, OptionalOwnerPC ? OptionalOwnerPC->MyFortPawn : nullptr, bToss, bRandomRotation);
}

static void K2_SpawnPickupInWorldWithClassAndItemEntry(UObject* Context, FFrame& Stack, AFortPickupAthena** Ret)
{
    UObject* WorldContextObject;
    FFortItemEntry Entry{};
    TSubclassOf<AFortPickupAthena> PickupClass;
    FVector Position;
    FVector Direction;
    int32 OverrideMaxStackCount;
    bool bToss;
    bool bRandomRotation;
    bool bBlockedFromAutoPickup;
    EFortPickupSourceTypeFlag SourceType;
    EFortPickupSpawnSource Source;
    AFortPlayerController* OptionalOwnerPC;
    bool bPickupOnlyRelevantToOwner;

    Stack.StepCompiledIn(&WorldContextObject);
    Stack.StepCompiledIn(&Entry);
    Stack.StepCompiledIn(&PickupClass);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.StepCompiledIn(&OverrideMaxStackCount);
    Stack.StepCompiledIn(&bToss);
    Stack.StepCompiledIn(&bRandomRotation);
    Stack.StepCompiledIn(&bBlockedFromAutoPickup);
    Stack.StepCompiledIn(&SourceType);
    Stack.StepCompiledIn(&Source);
    Stack.StepCompiledIn(&OptionalOwnerPC);
    Stack.StepCompiledIn(&bPickupOnlyRelevantToOwner);
    Stack.IncrementCode();

    *Ret = SpawnPickup(Position, (UFortItemDefinition*)Entry.ItemDefinition, Entry.Count, Entry.LoadedAmmo, SourceType, Source, OptionalOwnerPC ? OptionalOwnerPC->MyFortPawn : nullptr, bToss, bRandomRotation);

    static auto DestroyItemEntry = (void (*)(FFortItemEntry*))(ImageBase + 0x16E77DC);
    DestroyItemEntry(&Entry);
}

static void SpawnItemVariantPickupInWorld(UObject* Context, FFrame& Stack, AFortPickupAthena** Ret)
{
    UObject* WorldContextObject;
    FSpawnItemVariantParams Params;

    Stack.StepCompiledIn(&WorldContextObject);
    Stack.StepCompiledIn(&Params);
    Stack.IncrementCode();

    *Ret = SpawnPickup(Params.Position, Params.WorldItemDefinition, Params.NumberToSpawn, -1, Params.SourceType, Params.Source, nullptr, Params.bToss, Params.bRandomRotation);
}

static void PickLootDrops(UObject* Context, FFrame& Stack, bool* Ret)
{
    UObject* WorldContextObject;
    FName TierGroupName;
    int32 WorldLevel;
    int32 ForcedLootTier;
    FGameplayTagContainer OptionalLootTags{};

    Stack.StepCompiledIn(&WorldContextObject);
    TArray<FFortItemEntry> OutLootToDropTemp{};
    auto& OutLootToDrop = Stack.StepCompiledInRef(&OutLootToDropTemp);
    Stack.StepCompiledIn(&TierGroupName);
    Stack.StepCompiledIn(&WorldLevel);
    Stack.StepCompiledIn(&ForcedLootTier);
    Stack.StepCompiledIn(&OptionalLootTags);
    Stack.IncrementCode();

    TArray<FFortItemEntry*> LootDrops{};
    ChooseLootForContainer(LootDrops, TierGroupName, ForcedLootTier, WorldLevel);

    for (auto LootDrop : LootDrops)
    {
        OutLootToDrop.Add(*LootDrop);
        free(LootDrop);
    }

    *Ret = LootDrops.Num() > 0;
}

static void (*GivePickupToOG)(AFortPickupAthena*, void*, bool);

static void GivePickupTo(AFortPickupAthena* Pickup, void* InventoryOwner, bool bDestroyAfterPickup)
{
    auto Pawn = (AFortPlayerPawnAthena*)Pickup->PickupLocationData.PickupTarget.Get();
    auto Controller = Pawn ? (AFortAthenaAIBotController*)Pawn->Controller : nullptr;

    if (Pickup->Role == ENetRole::ROLE_Authority && Controller && Controller->IsA(AFortAthenaAIBotController::StaticClass()) && InventoryOwner == (void*)(uint64(Controller) + 0x3F0) && Controller->Inventory.ObjectPointer)
    {
        InternalPickup(Controller, &Pickup->PrimaryPickupItemEntry);
        RefreshSlotItems(Controller);
    }

    GivePickupToOG(Pickup, InventoryOwner, bDestroyAfterPickup);
}

void AFortPickup__Init()
{
    Hook(ImageBase + 0x9ABE14C, GivePickupTo, GivePickupToOG);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_SpawnPickupInWorld"), K2_SpawnPickupInWorld);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_SpawnPickupInWorldWithClassAndItemEntry"), K2_SpawnPickupInWorldWithClassAndItemEntry);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("SpawnItemVariantPickupInWorld"), SpawnItemVariantPickupInWorld);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("PickLootDrops"), PickLootDrops);
}

void AFortPickup__PostLoad()
{
    ExecHook(AFortAthenaSupplyDrop::StaticClass()->FindFunction("SpawnPickup"), (void (*)(UObject*, FFrame&, AFortPickup**))SpawnPickup);
}

void AFortPickup__Tick()
{
    if (PendingQueries.empty())
        return;

    auto World = UWorld::GetWorld();

    if (!World)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(World);

    for (size_t i = 0; i < PendingQueries.size();)
    {
        auto& Pending = PendingQueries[i];
        auto Spawner = Pending.Spawner.Get();
        auto Query = Pending.Query.Get();

        if (Spawner && Query && !Query->ItemType && Time - Pending.StartTime < 30.)
        {
            i++;
            continue;
        }

        if (Spawner && Query && Query->ItemType)
        {
            auto Locations = Query->GetResultsAsLocations();
            int32 LocationIndex = 0;

            for (auto& [ConsumableClass, Count] : Pending.Consumables)
            {
                auto Consumable = (ABuildingGameplayActorConsumable*)ConsumableClass->ClassDefaultObject;
                auto bRandomRotation = Consumable && Consumable->IsA(ABuildingGameplayActorConsumable::StaticClass()) && Consumable->bSpawnerCalculateRandomRotation;

                for (int32 j = 0; j < Count && LocationIndex < Locations.Num(); j++)
                {
                    FRotator Rotation{};

                    if (bRandomRotation)
                        Rotation.Yaw = ((float)rand() / 32767.f) * 360.f;

                    SpawnActor(ConsumableClass, Locations[LocationIndex++], Rotation);
                }
            }

            Locations.Free();
        }

        PendingQueries.erase(PendingQueries.begin() + i);
    }
}
