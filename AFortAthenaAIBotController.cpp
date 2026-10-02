#include "pch.h"
#include "Shared.h"
#include "AFortInventory.h"
#include "AFortPickup.h"
#include "UConversationInstance.h"

INIT_MODULE(AFortAthenaAIBotController);
INIT_TICKER(AFortAthenaAIBotController);

static void (*InitializeInventoryOG)(void*, TArray<FItemAndCount>*);
static void (*ApplyCosmeticLoadoutOG)(AFortAthenaAIBotController*, void*);
static void (*DropLootOnDeathOG)(AFortAthenaAIBotController*, AFortPawn*, void*);
static void (*AutomaticReviveFromDBNOTickOG)(AFortAthenaAIBotController*);

static bool CreateInventory(void* InventoryOwnerInterface)
{
    auto Controller = (AFortAthenaAIBotController*)(uint64(InventoryOwnerInterface) - 0x3F0);
    auto Inventory = SpawnActor<AFortInventory>(FVector{}, FRotator{}, Controller);

    if (!Inventory)
        return false;

    Inventory->InventoryType = EFortInventoryType::World;

    Controller->Inventory.ObjectPointer = Inventory;
    Controller->Inventory.InterfacePointer = Inventory->GetInterface(IFortInventoryInterface::StaticClass());

    return Controller->Inventory.InterfacePointer != nullptr;
}

static void InitializeInventory(void* InventoryOwnerInterface, TArray<FItemAndCount>* Items)
{
    auto Controller = (AFortAthenaAIBotController*)(uint64(InventoryOwnerInterface) - 0x3F0);
    auto InventoryRuntimeParameters = Controller->CachedInventoryRuntimeParameters;

    if (!InventoryRuntimeParameters || InventoryRuntimeParameters->bSkipInventoryInitialization)
        return InitializeInventoryOG(InventoryOwnerInterface, Items);

    if (!Controller->Inventory.ObjectPointer)
        CreateInventory(InventoryOwnerInterface);

    auto Inventory = (AFortInventory*)Controller->Inventory.ObjectPointer;
    bool bRemoveAllItemsOnInitialization = InventoryRuntimeParameters->bRemoveAllItemsOnInitialization;

    if (Inventory && bRemoveAllItemsOnInitialization)
    {
        std::vector<FGuid> ItemGuids;

        for (int32 i = 0; i < Inventory->Inventory.ItemInstances.Num(); i++)
            if (auto Item = Inventory->Inventory.ItemInstances.Data[i])
                ItemGuids.push_back(Item->ItemEntry.ItemGuid);

        for (auto& ItemGuid : ItemGuids)
            Remove(Inventory, ItemGuid);
    }

    if (Inventory && Items && InventoryRuntimeParameters->bAddStartingItemsToInventory)
    {
        for (int32 i = 0; i < Items->Num(); i++)
        {
            auto& ItemAndCount = Items->Data[i];
            auto ItemDefinition = (UFortItemDefinition*)ItemAndCount.Item;

            if (!ItemDefinition || ItemAndCount.Count <= 0)
                continue;

            auto Stats = ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()) ? GetStats((UFortWeaponItemDefinition*)ItemDefinition) : nullptr;

            GiveItem(Inventory, ItemDefinition, ItemAndCount.Count, Stats ? Stats->ClipSize : 0);
        }
    }

    InventoryRuntimeParameters->bRemoveAllItemsOnInitialization = false;
    InitializeInventoryOG(InventoryOwnerInterface, Items);
    InventoryRuntimeParameters->bRemoveAllItemsOnInitialization = bRemoveAllItemsOnInitialization;
}

static void DropLootOnDeath(AFortAthenaAIBotController* Controller, AFortPawn* Pawn, void* Context)
{
    DropLootOnDeathOG(Controller, Pawn, Context);

    auto InventoryRuntimeParameters = Controller->CachedInventoryRuntimeParameters;

    if (!InventoryRuntimeParameters || !Pawn)
        return;

    auto LoopStateComponent = (UFortAthenaNPCLoopStateComponent*)Pawn->GetComponentByClass(UFortAthenaNPCLoopStateComponent::StaticClass());

    if (LoopStateComponent && LoopStateComponent->bSpawnOutsideTheLoop)
        return;

    auto& LootInfo = InventoryRuntimeParameters->LootInfo;
    FFortAthenaAILootInfoDataTableRow* LootInfoRow = nullptr;

    if (LootInfo.DataTable)
    {
        for (auto& Row : LootInfo.DataTable->RowMap)
        {
            if (Row.Key() == LootInfo.RowName)
            {
                LootInfoRow = (FFortAthenaAILootInfoDataTableRow*)Row.Value();
                break;
            }
        }
    }

    if (!LootInfoRow)
        return;

    auto Location = Pawn->K2_GetActorLocation();
    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    auto PlaylistContextTags = GameState ? GameState->GetAthenaPlaylistContextTags() : FGameplayTagContainer{};
    FGameplayTagContainer OwnedGameplayTags{};

    if (auto GameplayTagAssetInterface = Pawn->GetInterface(IGameplayTagAssetInterface::StaticClass()))
        ((void (*)(void*, FGameplayTagContainer*))(*(void***)GameplayTagAssetInterface)[0x2])(GameplayTagAssetInterface, &OwnedGameplayTags);

    auto Inventory = (AFortInventory*)Controller->Inventory.ObjectPointer;
    auto& LootDroppingBehaviors = LootInfoRow->LootDroppingBehaviors;
    std::vector<AFortPickupAthena*> Pickups;

    for (int32 i = 0; i < LootDroppingBehaviors.Num(); i++)
    {
        auto& LootDroppingBehavior = LootDroppingBehaviors.Data[i];

        if (!UBlueprintGameplayTagLibrary::IsTagQueryEmpty(LootDroppingBehavior.OptionalTagQuery) &&
            !UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(OwnedGameplayTags, LootDroppingBehavior.OptionalTagQuery))
            continue;

        if (!UBlueprintGameplayTagLibrary::IsTagQueryEmpty(LootDroppingBehavior.OptionalPlaylistTagQuery) &&
            !UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(PlaylistContextTags, LootDroppingBehavior.OptionalPlaylistTagQuery))
            continue;

        if (LootDroppingBehavior.bShouldDropInventoryOnDeath && Inventory)
        {
            for (int32 j = 0; j < Inventory->Inventory.ReplicatedEntries.Num(); j++)
            {
                auto& ItemEntry = Inventory->Inventory.ReplicatedEntries.Data[j];

                if (ItemEntry.ItemDefinition && CanBeDropped((UFortItemDefinition*)ItemEntry.ItemDefinition))
                    Pickups.push_back(SpawnPickup(Location, ItemEntry, EFortPickupSourceTypeFlag::AI, EFortPickupSpawnSource::NPCElimination, Pawn));
            }
        }

        auto& LootTiers = LootDroppingBehavior.LootTiers;

        if (!LootDroppingBehavior.bShouldDropLootOnDeath || LootTiers.Num() == 0)
            continue;

        int32 FirstTier = 0;
        int32 LastTier = LootTiers.Num();

        if (LootDroppingBehavior.bShouldBotPickRandomLootTier)
        {
            FirstTier = rand() % LootTiers.Num();
            LastTier = FirstTier + 1;
        }

        for (int32 Tier = FirstTier; Tier < LastTier; Tier++)
        {
            TArray<FFortItemEntry*> LootDrops{};

            ChooseLootForContainer(LootDrops, LootTiers.Data[Tier]);

            for (auto& LootDrop : LootDrops)
            {
                Pickups.push_back(SpawnPickup(Location, *LootDrop, EFortPickupSourceTypeFlag::AI, EFortPickupSpawnSource::NPCElimination, Pawn));
                free(LootDrop);
            }

            LootDrops.Free();
        }
    }

    OwnedGameplayTags.GameplayTags.Free();
    OwnedGameplayTags.ParentTags.Free();
    PlaylistContextTags.GameplayTags.Free();
    PlaylistContextTags.ParentTags.Free();

    for (auto& Pickup : Pickups)
        if (Pickup)
            Controller->OnPickupCreated.Process(Controller, Pickup);
}

static void AutomaticReviveFromDBNOTick(AFortAthenaAIBotController* Controller)
{
    auto& AutomaticReviveTimerHandle = *(uint64*)(uint64(Controller) + 0x1338);
    bool bWaitingForRevive = AutomaticReviveTimerHandle != 0;

    AutomaticReviveFromDBNOTickOG(Controller);

    auto Pawn = Controller->PlayerBotPawn;

    if (bWaitingForRevive && !AutomaticReviveTimerHandle && Pawn && Pawn->IsDBNO())
        Pawn->ReviveFromDBNO(Controller);
}

static void ApplyCosmeticLoadout(AFortAthenaAIBotController* Controller, void* CosmeticLoadout)
{
    static auto ApplyCharacterCustomization = (void (*)(APlayerState*, AFortPawn*, bool))(ImageBase + 0xA0A1664);

    ApplyCosmeticLoadoutOG(Controller, CosmeticLoadout);

    if (Controller->PlayerState && Controller->PlayerBotPawn)
        ApplyCharacterCustomization(Controller->PlayerState, nullptr, false);
}

static void (*InitializeServiceOG)(UAthenaAIServicePlayerBots*);

static void InitializeService(UAthenaAIServicePlayerBots* Service)
{
    if (Service->Class == UAthenaAIServicePlayerBots::StaticClass())
    {
        const std::pair<FSoftObjectPtr*, const wchar_t*> SoftObjectDefaults[] = {
            { &Service->MMRSpawnTable, L"/Game/Athena/AI/Phoebe/PhoebeSpawningTables/PhoebeMMRSpawning.PhoebeMMRSpawning" },
            { &Service->EQSMapSampling, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_BaseNavigation.EQS_Phoebe_BaseNavigation" },
            { &Service->BotPOIEQS, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_SystemicPOI.EQS_Phoebe_SystemicPOI" },
            { &Service->FindLocationsAroundPOIQuery, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_FindLocationInPOI.EQS_Phoebe_FindLocationInPOI" },
            { &Service->BotNameDataAsset, L"/Game/Athena/AI/Phoebe/Names/PhoebeNameDataAsset.PhoebeNameDataAsset" },
        };

        const std::pair<UObject**, const wchar_t*> ObjectDefaults[] = {
            { (UObject**)&Service->BotItemDataTable, L"/Game/Athena/AI/Phoebe/ManagerData/PhoebeItemData.PhoebeItemData" },
            { (UObject**)&Service->WeaponModRegistry, L"/Game/DataRegistries/AI/Phoebe_WeaponModRegistry.Phoebe_WeaponModRegistry" },
            { (UObject**)&Service->VisibilitySettingsRegistry, L"/Game/DataRegistries/AI/Phoebe_VisibilityRegistry.Phoebe_VisibilityRegistry" },
            { (UObject**)&Service->BotPOIExcludedZonesDataRegistry, L"/Game/DataRegistries/AI/Phoebe_DynamicPOI_ExcludedZones.Phoebe_DynamicPOI_ExcludedZones" },
            { (UObject**)&Service->DefaultBotAISpawnerData, L"/Game/Athena/AI/Phoebe/BP_AISpawnerData_Phoebe.BP_AISpawnerData_Phoebe_C" },
        };

        auto PhoebeData = L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_Data.Phoebe_Data";
        auto PhoebeDataByMaxSquadSize = L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_DataByMaxSquadSize.Phoebe_DataByMaxSquadSize";
        auto PhoebeDataBySafeZoneIndex = L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_DataBySafeZoneIndex.Phoebe_DataBySafeZoneIndex";

        const std::tuple<FScalableFloat*, const wchar_t*, const wchar_t*> CurveDefaults[] = {
            { &Service->DynamicBotPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.Enabled" },
            { &Service->JumpOffBusWithDynamicPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.JumpOffBusWithDynamicPOIEnabled" },
            { &Service->NavigationWithDynamicPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.NavigationWithDynamicPOIEnabled" },
            { &Service->DenyJumpBusBotPOIByProximityEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.DenyJumpBusBotPOIByProximity.Enabled" },
            { &Service->BotPOINextPOIDurationSecondByWeight, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIDurationSecondByWeight" },
            { &Service->BotPOINextPOIMinDuration, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIMinDuration" },
            { &Service->BotPOINextPOIMaxDuration, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIMaxDuration" },
            { &Service->BotPOIScoringWeightFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringWeightFactor" },
            { &Service->BotPOIScoringDistanceFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringDistanceFactor" },
            { &Service->BotPOIScoringOccupancyFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringOccupancyFactor" },
            { &Service->BotPOIScoringVisitedFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringVisitedFactor" },
            { &Service->FailToReachBotPOICountMax, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.FailToReachBotPOICountMax" },
            { &Service->BotPOIJumpMainPOICountMax, PhoebeDataByMaxSquadSize, L"Phoebe.BotPOI.JumpBus.MainPOICount" },
            { &Service->BotPOISquadJumpOnMainPOICountMax, PhoebeDataByMaxSquadSize, L"Phoebe.BotPOI.JumpBus.SquadJumpOnMainCount" },
            { &Service->BotPOIMinimumWeightForSquadMembers, PhoebeDataByMaxSquadSize, L"Phoebe.BotPOI.MinWeightToUsePOI" },
            { &Service->DenyJumpBusBotPOIDistance, PhoebeDataByMaxSquadSize, L"Phoebe.BotPOI.DenyJumpBusBotPOIByProximity.Distance" },
            { &Service->ChanceToUseNextSafeZone, PhoebeDataBySafeZoneIndex, L"Phoebe.ServicePlayerBots.DynamicBotPOI.ChanceToUseNextSafeZone" },
        };

        for (auto& [SoftObject, Path] : SoftObjectDefaults)
        {
            auto Object = FindObject<UObject>(Path);

            if (!Object || SoftObject->WeakPtr.Get() || SoftObject->ObjectID.AssetPath.PackageName.ComparisonIndex != 0)
                continue;

            std::wstring ObjectPath = Path;
            auto Separator = ObjectPath.find(L'.');

            SoftObject->ObjectID.AssetPath.PackageName = FName(ObjectPath.substr(0, Separator).c_str());
            SoftObject->ObjectID.AssetPath.AssetName = FName(ObjectPath.substr(Separator + 1).c_str());
            SoftObject->WeakPtr = FWeakObjectPtr(Object);
        }

        for (auto& [Field, Path] : ObjectDefaults)
        {
            auto Object = FindObject<UObject>(Path);

            if (Object && !*Field)
                *Field = Object;
        }

        for (auto& [ScalableFloat, CurveTablePath, Row] : CurveDefaults)
        {
            if (ScalableFloat->Curve.CurveTable)
                continue;

            auto CurveTable = FindObject<UCurveTable>(CurveTablePath);
            auto RowName = FName(Row);
            bool bHasRow = false;

            if (CurveTable)
                for (auto& CurveRow : GetRowMap(CurveTable))
                    if (CurveRow.Key() == RowName)
                        bHasRow = true;

            if (!bHasRow)
                continue;

            ScalableFloat->Value = 1.f;
            *(int32*)((uint8*)ScalableFloat + 0x4) = -1;
            ScalableFloat->Curve.CurveTable = CurveTable;
            ScalableFloat->Curve.RowName = RowName;
            *(void**)((uint8*)ScalableFloat + 0x20) = nullptr;
        }

        if (Service->GamePhaseToStartSpawning == EAthenaGamePhase::None)
            Service->GamePhaseToStartSpawning = EAthenaGamePhase::Aircraft;
    }

    InitializeServiceOG(Service);
}

bool BotFinishedTargetSpline(AActor* Pickup, AActor* Controller, bool (*Original)(void*))
{
    auto Inventory = (AFortInventory*)((AFortAthenaAIBotController*)Controller)->Inventory.ObjectPointer;
    auto& PickupEntry = ((AFortPickupAthena*)Pickup)->PrimaryPickupItemEntry;
    auto CountItem = [&]()
    {
        int32 Count = 0;

        if (Inventory && PickupEntry.ItemDefinition)
            for (int32 i = 0; i < Inventory->Inventory.ReplicatedEntries.Num(); i++)
                if (Inventory->Inventory.ReplicatedEntries.Data[i].ItemDefinition == PickupEntry.ItemDefinition)
                    Count += Inventory->Inventory.ReplicatedEntries.Data[i].Count;

        return Count;
    };

    auto Before = CountItem();
    auto Result = Original(Pickup);
    auto After = CountItem();

    if (Inventory && PickupEntry.ItemDefinition && PickupEntry.Count > 0 && After <= Before)
        GiveItem(Inventory, PickupEntry);

    return Result;
}

void CreateSubsystems(UAthenaAISystem* AISystem, UObject* GameMode)
{
    if (!AISystem->PerceptionManager)
        AISystem->PerceptionManager = (UAthenaAIPerceptionManager*)UGameplayStatics::SpawnObject(UAthenaAIPerceptionManager::StaticClass(), AISystem);

    if (!AISystem->AIDropper)
        AISystem->AIDropper = (UAthenaAIDropper*)UGameplayStatics::SpawnObject(UAthenaAIDropper::StaticClass(), AISystem);

    if (!AISystem->AIVehicleAvoidanceManager)
        AISystem->AIVehicleAvoidanceManager = (UAthenaAIVehicleAvoidanceManager*)UGameplayStatics::SpawnObject(UAthenaAIVehicleAvoidanceManager::StaticClass(), AISystem);

    if (AISystem->AIPopulationTracker)
        return;

    auto PopulationTracker = (UAthenaAIPopulationTracker*)UGameplayStatics::SpawnObject(UAthenaAIPopulationTracker::StaticClass(), AISystem);

    if (!PopulationTracker)
        return;

    PopulationTracker->CachedGameMode = (AFortGameModeAthena*)GameMode;

    if (AISystem->AISpawner)
        AISystem->AISpawner->OnPawnSpawnedEvent.Bind(PopulationTracker, FName(L"OnAISpawned"));

    AISystem->AIPopulationTracker = PopulationTracker;
}

enum class EBotNavigationStage : uint8
{
    WaitingForRelease,
    WaitingForRegistration,
    Building,
    Done
};

static EBotNavigationStage BotNavigationStage = EBotNavigationStage::WaitingForRelease;
static UNavigationSystemBase* BotNavigationSystem = nullptr;
static int32 NavDataCountBeforeSpawn = 0;
static uint64 StageTime = 0;
static bool bBoundsReannounced = false;

static UClass* GetDigestedSkillSetClass(UClass* SkillSetClass)
{
    auto SkillSet = SkillSetClass ? SkillSetClass->ClassDefaultObject : nullptr;
    UClass* DigestedSkillSetClass = nullptr;

    if (SkillSet)
        ((UClass** (*)(UObject*, UClass**))SkillSet->VTable[0x59])(SkillSet, &DigestedSkillSetClass);

    return DigestedSkillSetClass;
}

static void GetRuntimeParametersClasses(void* RuntimeParametersProvider, TArray<UClass*>* OutClasses)
{
    auto Component = (UFortAthenaAISpawnerDataComponent_SkillsetBase*)(uint64(RuntimeParametersProvider) - 0x28);

    if (!OutClasses || !Component->IsA(UFortAthenaAISpawnerDataComponent_SkillsetBase::StaticClass()))
        return;

    auto SkillSets = Component->GetSkillSets();

    for (int32 i = 0; i < SkillSets.Num(); i++)
        if (auto DigestedSkillSetClass = GetDigestedSkillSetClass(SkillSets.Data[i]))
            OutClasses->Add(DigestedSkillSetClass);

    SkillSets.Free();
}

static int32 FillRuntimeParameters(void* RuntimeParametersProvider, UObject* RuntimeParameters, int32 SkillLevel, int32 Result)
{
    auto Component = (UFortAthenaAISpawnerDataComponent_SkillsetBase*)(uint64(RuntimeParametersProvider) - 0x28);

    if (!RuntimeParameters || !Component->IsA(UFortAthenaAISpawnerDataComponent_SkillsetBase::StaticClass()))
        return Result;

    auto SkillSets = Component->GetSkillSets();

    for (int32 i = 0; i < SkillSets.Num(); i++)
    {
        auto DigestedSkillSetClass = GetDigestedSkillSetClass(SkillSets.Data[i]);

        if (!DigestedSkillSetClass || !RuntimeParameters->IsA(DigestedSkillSetClass))
            continue;

        ((void (*)(UObject*, int32, UObject*))RuntimeParameters->VTable[0x59])(RuntimeParameters, SkillLevel, ((UClass*)SkillSets.Data[i])->ClassDefaultObject);
        break;
    }

    SkillSets.Free();

    return Result;
}

static UFortAthenaAIBotCosmeticLibraryData* LastLibrary = nullptr;
static UFortAthenaAISpawnerDataComponent_CosmeticLibrary* LastLibraryComponent = nullptr;

static float RandomFraction()
{
    return float(rand()) / float(RAND_MAX + 1);
}

static UFortAthenaAIBotCosmeticLibraryData* PickLibrary(UFortAthenaAISpawnerDataComponent_CosmeticLibrary* Component)
{
    auto& CosmeticLibraries = Component->CosmeticLibraries;
    std::vector<std::pair<int32, float>> Weights;
    float TotalWeight = 0.f;

    for (int32 i = 0; i < CosmeticLibraries.Num(); i++)
    {
        auto Weight = Component->DefaultCosmeticLibraryWeight.Evaluate(float(i));

        if (Weight <= 0.f)
            continue;

        Weights.push_back({ i, Weight });
        TotalWeight += Weight;
    }

    if (Weights.empty())
        return nullptr;

    auto Roll = RandomFraction() * TotalWeight;
    auto Chosen = Weights.back().first;

    for (auto& [Index, Weight] : Weights)
    {
        if (Roll < Weight)
        {
            Chosen = Index;
            break;
        }

        Roll -= Weight;
    }

    return CosmeticLibraries.Data[Chosen].Get();
}

static UObject* LoadPrimaryAsset(const FPrimaryAssetId& PrimaryAssetId)
{
    if (auto Object = UKismetSystemLibrary::GetObjectFromPrimaryAssetId(PrimaryAssetId))
        return Object;

    return UKismetSystemLibrary::LoadAsset_Blocking(UKismetSystemLibrary::GetSoftObjectReferenceFromPrimaryAssetId(PrimaryAssetId));
}

static FFortBotCosmeticItemDataTableRow* PickRow(UDataTable* DataTable, std::vector<FFortBotCosmeticItemDataTableRow*>& Excluded)
{
    std::vector<FFortBotCosmeticItemDataTableRow*> Rows;
    float TotalWeight = 0.f;

    for (auto& RowPair : DataTable->RowMap)
    {
        auto Row = (FFortBotCosmeticItemDataTableRow*)RowPair.Value();

        if (!Row || Row->Weight <= 0.f || std::find(Excluded.begin(), Excluded.end(), Row) != Excluded.end())
            continue;

        Rows.push_back(Row);
        TotalWeight += Row->Weight;
    }

    if (Rows.empty())
        return nullptr;

    auto Roll = RandomFraction() * TotalWeight;

    for (auto Row : Rows)
    {
        if (Roll < Row->Weight)
            return Row;

        Roll -= Row->Weight;
    }

    return Rows.back();
}

static void GetLoadout(UFortAthenaAISpawnerDataComponent_CosmeticLibrary* Component, FFortAthenaLoadout* Loadout)
{
    auto Library = PickLibrary(Component);

    LastLibrary = Library;
    LastLibraryComponent = Component;

    if (!Library)
        return;

    const std::tuple<TSoftObjectPtr<UDataTable>*, TSoftObjectPtr<UDataTable>*, UObject**> Slots[] = {
        { &Library->CharactersDataTable, &Library->FallbackCharactersDataTable, (UObject**)&Loadout->Character },
        { &Library->BackpacksDataTable, nullptr, (UObject**)&Loadout->Backpack },
        { &Library->PickaxesDataTable, nullptr, (UObject**)&Loadout->Pickaxe },
        { &Library->GlidersDataTable, nullptr, (UObject**)&Loadout->Glider },
        { &Library->SkyDiveContrailsDataTable, nullptr, (UObject**)&Loadout->SkyDiveContrail },
    };

    for (auto& [Table, FallbackTable, LoadoutItem] : Slots)
    {
        auto DataTable = Table->Get();

        if (!DataTable && FallbackTable)
            DataTable = FallbackTable->Get();

        if (!DataTable)
            continue;

        std::vector<FFortBotCosmeticItemDataTableRow*> Excluded;
        auto Row = PickRow(DataTable, Excluded);
        auto Item = Row ? LoadPrimaryAsset(Row->PrimaryAssetId) : nullptr;

        if (Item)
            *LoadoutItem = Item;
    }
}

static void GetDances(UFortAthenaAISpawnerDataComponent_CosmeticLibrary* Component, TArray<UAthenaDanceItemDefinition*>* Dances, AFortAthenaAIBotController* BotController)
{
    auto Library = LastLibraryComponent == Component && LastLibrary ? LastLibrary : PickLibrary(Component);
    auto DataTable = Library ? Library->EmotesDataTable.Get() : nullptr;

    LastLibrary = nullptr;
    LastLibraryComponent = nullptr;

    if (!DataTable)
        return;

    auto EmotesMaxCount = int32(Component->EmotesMaxCount.Evaluate());
    std::vector<FFortBotCosmeticItemDataTableRow*> Picked;

    while (int32(Picked.size()) < EmotesMaxCount)
    {
        auto Row = PickRow(DataTable, Picked);

        if (!Row)
            break;

        Picked.push_back(Row);

        if (auto Dance = LoadPrimaryAsset(Row->PrimaryAssetId))
            Dances->Add((UAthenaDanceItemDefinition*)Dance);
    }
}

static std::unordered_map<UClass*, int32> DescriptorTags;

static uint64 (*ApplyToPawnOG)(UFortAthenaAISpawnerDataComponent*, APawn*);

static uint64 ApplyToPawn(UFortAthenaAISpawnerDataComponent* Component, APawn* Pawn)
{
    auto Result = ApplyToPawnOG(Component, Pawn);

    if (!Component || !Pawn)
        return Result;

    auto ConversationComponent = Pawn->GetComponentByClass(UFortNonPlayerConversationParticipantComponent::StaticClass());

    if (!ConversationComponent)
        return Result;

    FGameplayTag Tag{};
    auto Found = DescriptorTags.find(Component->Class);

    if (Found != DescriptorTags.end())
        Tag.TagName.ComparisonIndex = Found->second;
    else
    {
        for (int32 i = 0; i < UObject::GObjects->Num(); i++)
        {
            auto SpawnerData = (UFortAthenaAISpawnerData*)UObject::GObjects->GetByIndex(i);

            if (!SpawnerData || !SpawnerData->IsDefaultObject() || !SpawnerData->IsA(UFortAthenaAISpawnerData::StaticClass()) || SpawnerData->ConversationComponent != Component->Class)
                continue;

            if (SpawnerData->DescriptorTag.GameplayTags.Num() > 0)
            {
                Tag = SpawnerData->DescriptorTag.GameplayTags.Data[0];
                break;
            }
        }

        if (Tag.TagName.ComparisonIndex != 0)
            DescriptorTags[Component->Class] = Tag.TagName.ComparisonIndex;
    }

    SetNPCUniqueIDTag(ConversationComponent, Tag);

    return Result;
}

static void (*TickComponentOG)(UFortNearbyActorsPerceptionComponent*, float, uint8, void*);

static void TickComponent(UFortNearbyActorsPerceptionComponent* Component, float DeltaTime, uint8 TickType, void* ThisTickFunction)
{
    auto& CachedAIDirector = *(FWeakObjectPtr*)(uint64(Component) + 0xB8);

    if (!CachedAIDirector.Get())
    {
        auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

        if (GameMode && GameMode->AIDirector)
            CachedAIDirector = FWeakObjectPtr(GameMode->AIDirector);
    }

    TickComponentOG(Component, DeltaTime, TickType, ThisTickFunction);
}

void AFortAthenaAIBotController__Init()
{
    Hook(ImageBase + 0xA549788, ApplyCosmeticLoadout, ApplyCosmeticLoadoutOG);
    Hook(ImageBase + 0xA5512C8, DropLootOnDeath, DropLootOnDeathOG);
    Hook(ImageBase + 0xA5753F4, AutomaticReviveFromDBNOTick, AutomaticReviveFromDBNOTickOG);

    auto InventoryOwnerInterfaceVft = *(void***)(uint64(AFortAthenaAIBotController::GetDefaultObj()) + 0x3F0);

    InitializeInventoryOG = decltype(InitializeInventoryOG)(InventoryOwnerInterfaceVft[0x4]);
    Patch<void*>(uint64(&InventoryOwnerInterfaceVft[0x4]), (void*)InitializeInventory);
    Patch<void*>(uint64(&InventoryOwnerInterfaceVft[0x3]), (void*)CreateInventory);

    Hook<UAthenaAIServicePlayerBots>(uint32(0x59), InitializeService, InitializeServiceOG);
    Patch<uint8_t>(ImageBase + 0xA6BB9A4, 0xC3);

    auto BaseInterfaceVft = *(void***)(uint64(UFortAthenaAISpawnerDataComponent::GetDefaultObj()) + 0x28);

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (!Object || !Object->IsDefaultObject() || !Object->IsA(UFortAthenaAISpawnerDataComponent_SkillsetBase::StaticClass()))
            continue;

        auto InterfaceVft = *(void***)(uint64(Object) + 0x28);

        if (InterfaceVft == BaseInterfaceVft)
            continue;

        if (InterfaceVft[0x2] == BaseInterfaceVft[0x2])
            Patch<void*>(uint64(&InterfaceVft[0x2]), (void*)GetRuntimeParametersClasses);

        if (InterfaceVft[0x3] == BaseInterfaceVft[0x3])
            Patch<void*>(uint64(&InterfaceVft[0x3]), (void*)FillRuntimeParameters);
    }

    Hook<UFortAthenaAISpawnerDataComponent_CosmeticLibrary>(uint32(0x62), GetLoadout);
    Hook<UFortAthenaAISpawnerDataComponent_CosmeticLibrary>(uint32(0x5F), GetDances);
    Hook<UFortAthenaAISpawnerDataComponent_AIBotConversation>(uint32(0x5B), ApplyToPawn, ApplyToPawnOG);

    auto TickFunction = UFortNearbyActorsPerceptionComponent::GetDefaultObj()->VTable[0x7B];

    TickComponentOG = decltype(TickComponentOG)(TickFunction);

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (Object && Object->IsDefaultObject() && Object->IsA(UFortNearbyActorsPerceptionComponent::StaticClass()) && Object->VTable[0x7B] == TickFunction)
            VirtualSwap(Object->VTable, 0x7B, TickComponent);
    }
}

void AFortAthenaAIBotController__Tick()
{
    auto World = UWorld::GetWorld();
    auto NavigationSystem = World ? (UNavigationSystemV1*)World->NavigationSystem : nullptr;

    if (!NavigationSystem)
        return;

    if (NavigationSystem != BotNavigationSystem)
    {
        BotNavigationSystem = NavigationSystem;
        BotNavigationStage = EBotNavigationStage::WaitingForRelease;
        bBoundsReannounced = false;
    }

    if (BotNavigationStage == EBotNavigationStage::Done)
        return;

    auto AISystem = (UFortAISystem*)World->AISystem;

    if (!AISystem || !AISystem->IsA(UFortAISystem::StaticClass()) || !AISystem->AIServiceManager)
        return;

    auto& AIServices = AISystem->AIServiceManager->AIServices;
    bool bPlayerBotsRegistered = false;

    for (int32 i = 0; i < AIServices.Num(); i++)
        if (AIServices.Data[i] && AIServices.Data[i]->Class == UAthenaAIServicePlayerBots::StaticClass())
            bPlayerBotsRegistered = true;

    if (!bPlayerBotsRegistered)
        return;

    auto Now = GetTickCount64();
    auto& NavDataSet = NavigationSystem->NavDataSet;

    if (BotNavigationStage == EBotNavigationStage::WaitingForRelease)
    {
        if (NavDataSet.Num() > 0)
        {
            BotNavigationStage = EBotNavigationStage::Done;
            return;
        }

        if (UAthenaNavSystem::IsInitialNavigationLockActive(World) || (UNavigationSystemV1::IsNavigationBeingBuiltOrLocked(World) && !UNavigationSystemV1::IsNavigationBeingBuilt(World)))
            return;

        NavDataCountBeforeSpawn = NavDataSet.Num();

        ((void (*)(UNavigationSystemV1*))NavigationSystem->VTable[0x92])(NavigationSystem);

        BotNavigationStage = EBotNavigationStage::WaitingForRegistration;
        StageTime = Now;
        return;
    }

    if (BotNavigationStage == EBotNavigationStage::WaitingForRegistration)
    {
        if (NavDataSet.Num() > NavDataCountBeforeSpawn)
        {
            BotNavigationStage = EBotNavigationStage::Building;
            StageTime = Now;
        }
        else if (Now - StageTime >= 15000)
            BotNavigationStage = EBotNavigationStage::Done;

        return;
    }

    if (UNavigationSystemV1::IsNavigationBeingBuilt(World))
    {
        BotNavigationStage = EBotNavigationStage::Done;
        return;
    }

    if (!bBoundsReannounced && Now - StageTime >= 5000)
    {
        TArray<ANavMeshBoundsVolume*> Volumes;
        GetAll<ANavMeshBoundsVolume>(Volumes);

        for (int32 i = 0; i < Volumes.Num(); i++)
            if (Volumes.Data[i] && (*(uint32*)&Volumes.Data[i]->SupportedAgents & *(uint32*)&NavigationSystem->SupportedAgentsMask))
                NavigationSystem->OnNavigationBoundsUpdated(Volumes.Data[i]);

        Volumes.Free();

        bBoundsReannounced = true;
        StageTime = Now;
    }
    else if (bBoundsReannounced && Now - StageTime >= 10000)
        BotNavigationStage = EBotNavigationStage::Done;
}
