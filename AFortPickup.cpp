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

    TArray<FFortItemEntry> MultiItemPickupEntries{};
    SetPickupItemsFn(Pickup, &Pickup->PrimaryPickupItemEntry, &MultiItemPickupEntries, (uint8_t)EFortPickupSourceTypeFlag::Container, false, (uint8_t)EFortPickupSpawnSource::Chest);
}

static void InitializePickup(AFortPickupAthena* Pickup, const FFortItemEntry& Entry, AFortPawn* Pawn, int OverrideCount)
{
    auto& PickupEntry = Pickup->PrimaryPickupItemEntry;

    PickupEntry.ItemDefinition = Entry.ItemDefinition;
    PickupEntry.LoadedAmmo = Entry.LoadedAmmo;
    PickupEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    PickupEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;
    CopyWeaponModsFrom(PickupEntry, Entry);

    SetPickupItems(Pickup);
    Pickup->PawnWhoDroppedPickup = Pawn;
}

AFortPickupAthena* SpawnPickup(FVector Loc, FFortItemEntry& Entry, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource, AFortPawn* Pawn, int OverrideCount, bool bToss, bool bRandomRotation,
                               bool bCombine, const UClass* OverrideClass, FVector FinalLoc)
{
    auto NewPickup = SpawnActor<AFortPickupAthena>(OverrideClass ? OverrideClass : AFortPickupAthena::StaticClass(), Loc, {});

    if (!NewPickup)
        return nullptr;

    NewPickup->bRandomRotation = bRandomRotation;

    if (Entry.Level != -1)
        NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;

    InitializePickup(NewPickup, Entry, Pawn, OverrideCount);

    auto FinalLocation = FinalLoc.X || FinalLoc.Y || FinalLoc.Z ? FinalLoc : Loc;
    NewPickup->TossPickup(FinalLocation, Pawn, -1, bToss, true, SourceTypeFlag, SpawnSource);

    NewPickup->bTossedFromContainer = SpawnSource == EFortPickupSpawnSource::Chest || SpawnSource == EFortPickupSpawnSource::AmmoBox;

    if (NewPickup->bTossedFromContainer)
        NewPickup->OnRep_TossedFromContainer();

    RegisterPickupLoot(NewPickup);

    return NewPickup;
}

AFortPickupAthena* SpawnPickup(FVector Loc, const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource, AFortPawn* Pawn, bool bToss,
                               bool bRandomRotation, const UClass* OverrideClass)
{
    auto ItemEntry = MakeItemEntry(ItemDefinition, Count, -1);

    if (LoadedAmmo != -1)
        ItemEntry->LoadedAmmo = LoadedAmmo;

    auto Pickup = SpawnPickup(Loc, *ItemEntry, SourceTypeFlag, SpawnSource, Pawn, -1, bToss, true, bRandomRotation, OverrideClass);
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
    InitializePickup(NewPickup, Entry, Pawn, OverrideCount);

    UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 10, (int32)std::clamp((float)rand() * 0.0003357036f, 0.f, 10.f), Container->LootTossConeHalfAngle_Athena,
                                                *(FRotator*)&Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, Container->bForceHidePickupMinimapIndicator);

    NewPickup->bTossedFromContainer = true;
    NewPickup->OnRep_TossedFromContainer();

    RegisterPickupLoot(NewPickup);

    return NewPickup;
}

template <typename T>
static T* PickWeighted(TArray<T*>& Elements)
{
    float TotalWeight = 0.0f;

    for (auto& Element : Elements)
        TotalWeight += Element->Weight;

    float RandomNumber = ((float)rand() / 32767.f) * TotalWeight;

    for (auto& Element : Elements)
    {
        float Weight = Element->Weight;

        if (Weight == 0)
            continue;

        if (RandomNumber <= Weight)
            return Element;

        RandomNumber -= Weight;
    }

    return nullptr;
}

static FFortItemEntry* MakeWeaponAmmoEntry(UFortItemDefinition* ItemDefinition, int WorldLevel)
{
    if (!ItemDefinition->IsA(UFortWorldItemDefinition::StaticClass()))
        return nullptr;

    auto AmmoDefinition = ((UFortWorldItemDefinition*)ItemDefinition)->GetAmmoWorldItemDefinition_BP();

    if (!AmmoDefinition || !AmmoDefinition->IsA(UFortAmmoItemDefinition::StaticClass()))
        return nullptr;

    auto SpawnAmmoData = FindObject<UFortWeaponPickupSpawnAmmoData>(L"/Game/Athena/Balance/Pickups/FortWeaponPickupSpawnAmmoData.FortWeaponPickupSpawnAmmoData");

    FGameplayTagContainer AmmoTags{};
    GetOwnedGameplayTags(AmmoDefinition, AmmoTags);

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
    GetOwnedGameplayTags(ItemDefinition, WeaponTags);

    for (auto& MultiplierData : SpawnAmmoData->WeaponPickupAmmoMultiplierOverrideArray)
    {
        if (WeaponTags.HasTag(MultiplierData.Tag))
        {
            Multiplier = MultiplierData.CountMultiplier.Evaluate((float)WorldLevel);
            break;
        }
    }

    auto FinalCount = AmmoCount * Multiplier;
    auto AmmoEntry = FinalCount > 0.f ? MakeItemEntry(AmmoDefinition, (int)FinalCount, 0) : nullptr;

    FreeGameplayTags(AmmoTags);
    FreeGameplayTags(WeaponTags);

    return AmmoEntry;
}

static void AddToLootDrop(TArray<FFortItemEntry*>& LootDrops, FFortItemEntry* LootDrop, const UFortItemDefinition* ItemDefinition, int32 Count)
{
    LootDrop->Count += Count;

    if (LootDrop->Count > ItemDefinition->GetMaxStackSize(nullptr))
    {
        auto TotalCount = LootDrop->Count;
        LootDrop->Count = ItemDefinition->GetMaxStackSize(nullptr);

        LootDrops.Add(MakeItemEntry(ItemDefinition, TotalCount - ItemDefinition->GetMaxStackSize(nullptr), 0));
    }
}

static void SetupLDSForPackage(TArray<FFortItemEntry*>& LootDrops, FName Package, int LootPackageCategory, int WorldLevel)
{
    TArray<FFortLootPackageData*> Candidates{};

    for (auto const& Candidate : LootPackageMap[Package.ComparisonIndex])
    {
        if (!Candidate)
            continue;

        if (LootPackageCategory != -1 && Candidate->LootPackageCategory != LootPackageCategory)
            continue;

        if (WorldLevel >= 0)
        {
            if (Candidate->MaxWorldLevel >= 0 && WorldLevel > Candidate->MaxWorldLevel)
                continue;

            if (Candidate->MinWorldLevel >= 0 && WorldLevel < Candidate->MinWorldLevel)
                continue;
        }

        Candidates.Add(Candidate);
    }

    if (Candidates.Num() == 0)
        return;

    auto LootPackage = PickWeighted(Candidates);

    if (!LootPackage)
        return;

    if (LootPackage->LootPackageCall.Num() > 1)
    {
        for (int i = 0; i < LootPackage->Count; i++)
            SetupLDSForPackage(LootDrops, UKismetStringLibrary::Conv_StringToName(LootPackage->LootPackageCall), -1, WorldLevel);

        return;
    }

    auto ItemDefinition = LootPackage->ItemDefinition.Get();

    if (!ItemDefinition)
        return;

    auto AmmoEntry = MakeWeaponAmmoEntry(ItemDefinition, WorldLevel);
    bool bFound = false;

    for (auto& LootDrop : LootDrops)
    {
        if (LootDrop->ItemDefinition == (UItemDefinitionBase*)ItemDefinition && LootDrop->Count < ItemDefinition->GetMaxStackSize(nullptr))
        {
            AddToLootDrop(LootDrops, LootDrop, ItemDefinition, LootPackage->Count);
            bFound = true;
            break;
        }

        if (AmmoEntry && AmmoEntry->ItemDefinition && LootDrop->ItemDefinition == AmmoEntry->ItemDefinition)
        {
            AddToLootDrop(LootDrops, LootDrop, (UFortItemDefinition*)AmmoEntry->ItemDefinition, AmmoEntry->Count);
            free(AmmoEntry);
            AmmoEntry = nullptr;
        }
    }

    if (!bFound && LootPackage->Count > 0)
        LootDrops.Add(MakeItemEntry(ItemDefinition, LootPackage->Count, 0));

    if (AmmoEntry && LootPackage->Count > 0)
        LootDrops.Add(AmmoEntry);

    Candidates.Free();
}

void ChooseLootForContainer(TArray<FFortItemEntry*>& LootDrops, FName TierGroup, int LootTier, int WorldLevel)
{
    TArray<FFortLootTierData*> TierDataGroups{};

    for (auto const& TierData : TierDataMap[TierGroup.ComparisonIndex])
        if (LootTier == -1 || LootTier == TierData->LootTier)
            TierDataGroups.Add(TierData);

    auto LootTierData = PickWeighted(TierDataGroups);

    if (!LootTierData || LootTierData->NumLootPackageDrops <= 0)
        return;

    int DropCount;

    if (LootTierData->NumLootPackageDrops < 1.f)
        DropCount = 1;
    else
    {
        DropCount = (int)((LootTierData->NumLootPackageDrops * 2.f) - .5f) >> 1;

        float Remainder = LootTierData->NumLootPackageDrops - (float)DropCount;

        if (Remainder > 0.0000099999997f)
            DropCount += Remainder >= ((float)rand() / 32767.f);
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
            SetupLDSForPackage(LootDrops, LootTierData->LootPackage, i, WorldLevel);

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

static FFortSupplyDropGamePhaseData* GetSupplyDropGamePhaseData(AFortAthenaSupplyDrop* SupplyDrop, UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    for (auto& SpawnData : GamePhaseLogic->SupplyDropSpawnDataList)
    {
        if (!SpawnData.SupplyDropInfo || SpawnData.SupplyDropInfo->SupplyDropClass != SupplyDrop->Class)
            continue;

        for (auto& GamePhaseData : SpawnData.SupplyDropInfo->GamePhaseData)
        {
            if (GamePhaseData.GamePhase == GamePhaseLogic->GamePhase)
                return &GamePhaseData;
        }
    }

    return nullptr;
}

static void GetSupplyDropSpeedData(UObject* Context, FFrame& Stack, void* Ret)
{
    float MinSpeedTemp = 0.f;
    auto& MinSpeed = Stack.StepCompiledInRef(&MinSpeedTemp);
    float MaxSpeedTemp = 0.f;
    auto& MaxSpeed = Stack.StepCompiledInRef(&MaxSpeedTemp);
    Stack.IncrementCode();

    auto SupplyDrop = (AFortAthenaSupplyDrop*)Context;
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(SupplyDrop);
    auto GamePhaseData = GamePhaseLogic ? GetSupplyDropGamePhaseData(SupplyDrop, GamePhaseLogic) : nullptr;

    MinSpeed = GamePhaseData ? (float)(int32)GamePhaseData->SupplyDropMinSpeed.Evaluate() : 280.f;
    MaxSpeed = GamePhaseData ? (float)(int32)GamePhaseData->SupplyDropMaxSpeed.Evaluate() : 500.f;
}

static void (*GetSupplyDropSpawnDataOG)(AFortAthenaSupplyDrop*, float&, float&);

static void GetSupplyDropSpawnData(AFortAthenaSupplyDrop* SupplyDrop, float& MinSpawnHeight, float& MaxSpawnHeight)
{
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(SupplyDrop);

    if (!GamePhaseLogic)
        return GetSupplyDropSpawnDataOG(SupplyDrop, MinSpawnHeight, MaxSpawnHeight);

    auto GamePhaseData = GetSupplyDropGamePhaseData(SupplyDrop, GamePhaseLogic);

    MinSpawnHeight = GamePhaseData ? (float)(int32)GamePhaseData->SupplyDropMinSpawnHeight.Evaluate() : 28000.f;
    MaxSpawnHeight = GamePhaseData ? (float)(int32)GamePhaseData->SupplyDropMaxSpawnHeight.Evaluate() : 32000.f;
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
    Hook(ImageBase + 0x3D4E748, GetSupplyDropSpawnData, GetSupplyDropSpawnDataOG);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_SpawnPickupInWorld"), K2_SpawnPickupInWorld);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("K2_SpawnPickupInWorldWithClassAndItemEntry"), K2_SpawnPickupInWorldWithClassAndItemEntry);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("SpawnItemVariantPickupInWorld"), SpawnItemVariantPickupInWorld);
    ExecHook(UFortKismetLibrary::StaticClass()->FindFunction("PickLootDrops"), PickLootDrops);
}

void AFortPickup__PostLoad()
{
    ExecHook(AFortAthenaSupplyDrop::StaticClass()->FindFunction("SpawnPickup"), (void (*)(UObject*, FFrame&, AFortPickup**))SpawnPickup);
    ExecHook(AFortAthenaSupplyDrop::StaticClass()->FindFunction("GetSupplyDropSpeedData"), GetSupplyDropSpeedData);
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
