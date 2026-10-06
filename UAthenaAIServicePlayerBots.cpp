#include "pch.h"
#include "Shared.h"
#include "UAthenaAIServicePlayerBots.h"
#include "AFortGameMode.h"
#include "AFortAthenaAIBotController.h"
#include "UAthenaNavSystem.h"
#include "UFortAthenaAISpawnerDataComponent_CosmeticLibrary.h"

INIT_MODULE(UAthenaAIServicePlayerBots);

static void (*OnPlaylistLoadedOG)(UAthenaAIServicePlayerBots*);
static void (*StartSpawningOG)(UAthenaAIServicePlayerBots*, void*);
static void (*OnAllBotsSpawnedOG)(UAthenaAIServicePlayerBots*);
static void (*UnpauseWarmupFailSafeOG)(UAthenaAIServicePlayerBots*);
static void (*PlacePlayersInAircraftOG)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*);
static void (*AssignBotTeamOG)(UAthenaAIServicePlayerBots*, AFortAthenaAIBotController*, AFortTeamInfo*, uint8);

static const wchar_t* BotNames[] = {
    L"Kyzen", L"Vaxtrix", L"FrostByte07", L"NovaRush", L"lilzapp", L"Drexo", L"Mxrcus", L"Zynthra", L"Kaioken42", L"Ravvy",
    L"Solvent", L"QuietRiot88", L"Jynx0", L"bleakfox", L"Tenshi_vr", L"Hollowpoint", L"Saberline", L"Crxwn", L"Pyrelight", L"MossyBoi",
    L"Vexlin", L"ZeroGravs", L"Quillon", L"Brekk", L"SnipeCity", L"Ozzilo", L"TacoWizard", L"Lumen_FN", L"Arkyn", L"Velocitty",
    L"Dazzlr", L"Grimsby", L"HxltZ", L"Coldbrew21", L"SkyeStorm", L"Rekkon", L"Ploxie", L"Thundr", L"Mavrik", L"Juno_ow",
    L"Fennec", L"Zarqo", L"Twixxy", L"Obsidyn", L"WaffleKing", L"Strafe_God", L"Nyxie", L"Kairo", L"Bramble", L"Hexed",
    L"Pixlwave", L"Torrent9", L"Sleepy_Sam", L"Cyphr", L"Rogue_Ash", L"Vantage", L"Clutchless", L"Lyrxa", L"Ember_Kid", L"Quazar",
    L"Dyno", L"Wrenly", L"Gorlax", L"SpectrE", L"Tilted_Tim", L"Brisky", L"Notchy", L"Polar_Bae", L"Zeneth", L"Ruckus",
    L"Kalisto", L"Fluxed", L"Mangoz", L"Driftwood", L"OmegaPup", L"Sorrel", L"Jaxxen", L"Neonvein", L"Bokchoy", L"Sylph",
    L"Trench", L"Nimbus77", L"Yolkie", L"Valkyr", L"Cheddarz", L"Ronin_Rex", L"Glitchy", L"Ashfall", L"Kepler", L"Moxxie",
    L"Draven_x", L"Pebbles", L"Sunspot", L"Wolfie", L"Rattler", L"Kinetik", L"Plum", L"Zorvin", L"Haze", L"Coastal",
    L"Bandit_b", L"Lucent", L"Stryde", L"Quokka", L"Tundra", L"Viper_7", L"Mellow", L"Riptide", L"Frizz", L"Onyx",
    L"Breeze", L"Scorch", L"Tuxedo_T", L"Halcyon", L"Nibbler", L"Sprocket", L"Vandal", L"Echoes", L"Calypso", L"Grizz",
    L"Pumice", L"SlickRik9", L"Talon", L"Zest", L"Marrow", L"Kudzu", L"Flare", L"Brickz", L"Juniper", L"Raptr",
    L"Wisp", L"Cobalt", L"Pistachio", L"Saxon", L"Glimmer", L"Tremor", L"Dusk", L"Yeti", L"Corvid", L"Spudz",
    L"Lynxx", L"Ozone", L"Banjo", L"Mirage", L"Thistle", L"Krakn", L"Sundae", L"Valor", L"Pixie", L"Gravel",
    L"Rook", L"Fable", L"Stingray", L"Muddy", L"Zephyr", L"Clover", L"Hydra", L"Tofu", L"Blitz", L"Nomad"
};

static void SetCurve(FScalableFloat& ScalableFloat, UCurveTable* CurveTable, const wchar_t* RowName)
{
    ScalableFloat.Value = 1.f;
    *(int32*)(uint64(&ScalableFloat) + 0x4) = -1;
    ScalableFloat.Curve.CurveTable = CurveTable;
    ScalableFloat.Curve.RowName = FName(RowName);
    *(void**)(uint64(&ScalableFloat) + 0x20) = nullptr;
}

static void SetSoftObject(FSoftObjectPtr& SoftObject, const std::wstring& Path)
{
    auto Separator = Path.find(L'.');

    SoftObject.ObjectID.AssetPath.PackageName = FName(Path.substr(0, Separator).c_str());
    SoftObject.ObjectID.AssetPath.AssetName = FName(Path.substr(Separator + 1).c_str());
    SoftObject.WeakPtr = FWeakObjectPtr(FindObject<UObject>(Path.c_str()));
}

static bool IsUsingAISettings(UAthenaAIServicePlayerBots* Service, const wchar_t* AISettingsPath)
{
    auto GameMode = Service->CachedGameMode;

    return GameMode && GameMode->AISettings && GameMode->AISettings == StaticFindObject(AISettingsPath, UAthenaAISettings::StaticClass());
}

static int32 GetPlaylistMaxPlayerCount(UAthenaAIServicePlayerBots* Service)
{
    return ((int32 (*)(UAthenaAIServicePlayerBots*))Service->VTable[0x64])(Service);
}

static void OnPlaylistLoaded(UAthenaAIServicePlayerBots* Service)
{
    static auto IsBotEnabled = (bool (*)(UWorld*, bool))(ImageBase + 0xA6A8AEC);

    auto ManagerData = FindObject<UCurveTable>(L"/Game/Athena/AI/Phoebe/ManagerData/Phoebe_ManagerData.Phoebe_ManagerData");
    auto PhoebeData = FindObject<UCurveTable>(L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_Data.Phoebe_Data");
    auto DataByMaxSquadSize = FindObject<UCurveTable>(L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_DataByMaxSquadSize.Phoebe_DataByMaxSquadSize");
    auto DataBySafeZoneIndex = FindObject<UCurveTable>(L"/Game/Athena/AI/Phoebe/DataTables/Phoebe_DataBySafeZoneIndex.Phoebe_DataBySafeZoneIndex");
    auto bClyde = IsUsingAISettings(Service, L"/Clyde/Playlist/PlayerBotsFlow_AISettings_Clyde.PlayerBotsFlow_AISettings_Clyde");
    auto bBlastBerry = IsUsingAISettings(Service, L"/BlastBerry/AI/Bots/PlayerBotsFlow_AISettings_BlastBerry.PlayerBotsFlow_AISettings_BlastBerry");
    auto NextPOIData = bBlastBerry ? FindObject<UCurveTable>(L"/BlastBerry/AI/Bots/Data/Phoebe_Data_BlastBerry.Phoebe_Data_BlastBerry") : PhoebeData;

    Service->GamePhaseToStartSpawning = EAthenaGamePhase::Warmup;

    if (!bNavCacheAttached)
        Service->bWaitForNavmeshToBeLoaded = false;

    Service->DefaultBotAISpawnerData = FindObject<UClass>(bBlastBerry ? L"/BlastBerry/AI/Bots/SpawnData/BP_AISD_Phoebe_BlastBerry.BP_AISD_Phoebe_BlastBerry_C" : L"/Game/Athena/AI/Phoebe/BP_AISpawnerData_Phoebe.BP_AISpawnerData_Phoebe_C");
    Service->BotItemDataTable = FindObject<UDataTable>(bClyde ? L"/Clyde/Datatables/Phoebe/Clyde_CompositePhoebeItemData.Clyde_CompositePhoebeItemData" : L"/Game/Athena/AI/Phoebe/ManagerData/PhoebeItemData.PhoebeItemData");
    Service->VisibilitySettingsRegistry = FindObject<UDataRegistry>(L"/Game/DataRegistries/AI/Phoebe_VisibilityRegistry.Phoebe_VisibilityRegistry");
    Service->BotPOIExcludedZonesDataRegistry = FindObject<UDataRegistry>(L"/Game/DataRegistries/AI/Phoebe_DynamicPOI_ExcludedZones.Phoebe_DynamicPOI_ExcludedZones");
    Service->WeaponModRegistry = FindObject<UDataRegistry>(L"/Game/DataRegistries/AI/Phoebe_WeaponModRegistry.Phoebe_WeaponModRegistry");

    SetSoftObject(Service->MMRSpawnTable, bBlastBerry ? L"/BlastBerry/AI/Bots/Data/PhoebeMMRSpawning_BlastBerry.PhoebeMMRSpawning_BlastBerry" : L"/Game/Athena/AI/Phoebe/PhoebeSpawningTables/PhoebeMMRSpawning.PhoebeMMRSpawning");
    SetSoftObject(Service->EQSMapSampling, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_BaseNavigation.EQS_Phoebe_BaseNavigation");
    SetSoftObject(Service->BotPOIEQS, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_SystemicPOI.EQS_Phoebe_SystemicPOI");
    SetSoftObject(Service->FindLocationsAroundPOIQuery, L"/Game/Athena/AI/Phoebe/EQS/EQS_Phoebe_FindLocationInPOI.EQS_Phoebe_FindLocationInPOI");

    SetCurve(Service->UseCustomSupportedItemList, ManagerData, L"phoebe.UseCustomSupportedItemList");
    SetCurve(Service->UseRegionalNameList, ManagerData, L"phoebe.UseRegionalNameList");
    SetCurve(Service->UseAllBattleBusPOIsSquadRatio, ManagerData, L"Phoebe.UseAllBattleBusPOIsSquadRatio");
    SetCurve(Service->ThankBusDriverProbability, ManagerData, L"Phoebe.ThankBusDriverProbability");
    SetCurve(Service->ThankBusDriverMinTime, ManagerData, L"Phoebe.ThankBusDriverMinTime");
    SetCurve(Service->ThankBusDriverMaxTime, ManagerData, L"Phoebe.ThankBusDriverMaxTime");
    SetCurve(Service->MaxNumberOfBotsToSpawnAroundPlayer, ManagerData, L"Phoebe.MaxBotsSpawnAroundPlayer");
    SetCurve(Service->BotsUniqueIDUseValidAccountID, ManagerData, L"Phoebe.BotsUniqueIDUseValidAccountID");
    SetCurve(Service->ZiplineOctreeRadius, ManagerData, L"Phoebe.AIService.ZiplineOctreeRadius");
    SetCurve(Service->UseZiplines, ManagerData, L"Phoebe.AIService.UseZiplines");
    SetCurve(Service->MaxAnonymousBotCount, ManagerData, L"Phoebe.AIService.MaxAnonymousBotCount");
    SetCurve(Service->ShowSeasonLevelProbability, ManagerData, L"Phoebe.AIService.ShowSeasonLevelProbability");
    SetCurve(Service->SpectateOthersOnDeath, ManagerData, L"Phoebe.AIService.SpectateOthersOnDeath");
    SetCurve(Service->TriggerEndGameBehaviorMinTime, ManagerData, L"Phoebe.AIService.TriggerEndGameBehaviorMinTime");
    SetCurve(Service->TriggerEndGameBehaviorMaxTime, ManagerData, L"Phoebe.AIService.TriggerEndGameBehaviorMaxTime");

    SetCurve(Service->DynamicBotPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.Enabled");
    SetCurve(Service->JumpOffBusWithDynamicPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.JumpOffBusWithDynamicPOIEnabled");
    SetCurve(Service->NavigationWithDynamicPOIEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.NavigationWithDynamicPOIEnabled");
    SetCurve(Service->DenyJumpBusBotPOIByProximityEnabled, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.DenyJumpBusBotPOIByProximity.Enabled");
    SetCurve(Service->BotPOINextPOIDurationSecondByWeight, NextPOIData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIDurationSecondByWeight");
    SetCurve(Service->BotPOINextPOIMinDuration, NextPOIData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIMinDuration");
    SetCurve(Service->BotPOINextPOIMaxDuration, NextPOIData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOINextPOIMaxDuration");
    SetCurve(Service->BotPOIScoringWeightFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringWeightFactor");
    SetCurve(Service->BotPOIScoringDistanceFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringDistanceFactor");
    SetCurve(Service->BotPOIScoringOccupancyFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringOccupancyFactor");
    SetCurve(Service->BotPOIScoringVisitedFactor, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.BotPOIScoringVisitedFactor");
    SetCurve(Service->FailToReachBotPOICountMax, PhoebeData, L"Phoebe.ServicePlayerBots.DynamicBotPOI.FailToReachBotPOICountMax");

    SetCurve(Service->BotPOIJumpMainPOICountMax, DataByMaxSquadSize, L"Phoebe.BotPOI.JumpBus.MainPOICount");
    SetCurve(Service->BotPOISquadJumpOnMainPOICountMax, DataByMaxSquadSize, L"Phoebe.BotPOI.JumpBus.SquadJumpOnMainCount");
    SetCurve(Service->BotPOIMinimumWeightForSquadMembers, DataByMaxSquadSize, L"Phoebe.BotPOI.MinWeightToUsePOI");
    SetCurve(Service->DenyJumpBusBotPOIDistance, DataByMaxSquadSize, L"Phoebe.BotPOI.DenyJumpBusBotPOIByProximity.Distance");

    SetCurve(Service->ChanceToUseNextSafeZone, DataBySafeZoneIndex, L"Phoebe.ServicePlayerBots.DynamicBotPOI.ChanceToUseNextSafeZone");

    OnPlaylistLoadedOG(Service);

    if ((*(uint8*)(uint64(Service) + 0x70) & 1) && IsBotEnabled(UWorld::GetWorld(), false))
        PreloadBotCosmetics(Service->DefaultBotAISpawnerData, GetPlaylistMaxPlayerCount(Service));
}

static void ResetSpawningInfos(TArray<FMMRSpawningInfo>& SpawningInfos, int32 Num)
{
    static auto DestructItems = (void (*)(FMMRSpawningInfo*, int32))(ImageBase + 0xA695C40);
    static auto ResizeForCopy = (void (*)(TArray<FMMRSpawningInfo>*, int32, int32))(ImageBase + 0x5A96A6C);

    DestructItems(SpawningInfos.Data, SpawningInfos.NumElements);
    SpawningInfos.NumElements = 0;
    ResizeForCopy(&SpawningInfos, Num, SpawningInfos.MaxElements);
    SpawningInfos.NumElements = Num;
    memset(SpawningInfos.Data, 0, Num * sizeof(FMMRSpawningInfo));
}

static void CalculateSpawnAmountAndSkill(UAthenaAIServicePlayerBots* Service)
{
    auto Base = uint64(Service);
    auto& Flags = *(uint8*)(Base + 0xC58);

    if (Flags & 0x8)
        return;

    auto Info = uint64(&Service->CachedMMRSpawningInfo);
    auto& SpawningInfos = Service->CachedMMRSpawningInfo.SpawningInfos;
    auto TotalWeight = *(int32*)(Info + 0x30);
    auto& TotalAmountToSpawn = *(int32*)(Info + 0x34);

    if (*(int32*)(Info + 0x8) <= -99999 || *(int32*)(Info + 0x8) >= *(int32*)(Info + 0xC) || SpawningInfos.Num() <= 0 || TotalWeight <= 0)
    {
        Flags |= 0x8;
        return;
    }

    auto& AmountToSpawn = *(int32*)(Base + 0xD84);

    if (AmountToSpawn == -1)
    {
        AmountToSpawn = *(int32*)(Base + 0xD88);

        if (AmountToSpawn == -1)
        {
            AmountToSpawn = 0;

            if (Service->CachedGameMode && Service->CachedGameState)
            {
                auto MaxPlayers = GetPlaylistMaxPlayerCount(Service);
                auto MaxSpawnAmount = *(int32*)(ImageBase + 0x12A9F22C);

                AmountToSpawn = (int32)*(float*)(ImageBase + 0x12A9F234);

                if (AmountToSpawn <= 0)
                    AmountToSpawn = MaxPlayers > *(int32*)(Base + 0xD80) ? MaxPlayers - *(int32*)(Base + 0xD80) : 0;

                if (MaxSpawnAmount > 0 && AmountToSpawn > MaxSpawnAmount)
                    AmountToSpawn = MaxSpawnAmount;
            }
        }
    }

    TotalAmountToSpawn = 0;

    for (int32 i = 0; i < SpawningInfos.Num(); i++)
    {
        auto Entry = uint64(&SpawningInfos.Data[i]);
        auto& NumBotsToSpawn = *(int32*)(Entry + 0x8);

        NumBotsToSpawn = (int32)((float)AmountToSpawn * (*(float*)(Entry + 0x4) / (float)TotalWeight));
        TotalAmountToSpawn += NumBotsToSpawn;
    }

    for (int32 i = 0; i < SpawningInfos.Num() && TotalAmountToSpawn < AmountToSpawn; i++)
    {
        (*(int32*)(uint64(&SpawningInfos.Data[i]) + 0x8))++;
        TotalAmountToSpawn++;
    }

    if (TotalAmountToSpawn != AmountToSpawn)
        TotalAmountToSpawn = 0;

    Flags |= 0x8;
}

static void UseFailSafeMMRInfo(UAthenaAIServicePlayerBots* Service, float TargetELO)
{
    auto Info = uint64(&Service->CachedMMRSpawningInfo);
    auto& SpawningInfos = Service->CachedMMRSpawningInfo.SpawningInfos;

    *(int32*)(Info + 0x8) = -1000;
    *(int32*)(Info + 0xC) = 4000;
    *(int32*)(Info + 0x30) = 100;

    ResetSpawningInfos(SpawningInfos, 1);

    auto Entry = uint64(&SpawningInfos.Data[0]);

    *(float*)(Entry + 0x4) = 100.f;
    *(float*)Entry = TargetELO > -1.f ? TargetELO : 1100.f;
    *(UClass**)(Entry + 0x10) = Service->DefaultBotAISpawnerData;

    CalculateSpawnAmountAndSkill(Service);
}

static void InitializeMMRInfos(UAthenaAIServicePlayerBots* Service)
{
    auto MMR = *(bool*)(ImageBase + 0x12F166DE) ? *(int32*)(ImageBase + 0x12A9A374) : -99999;
    auto MMRSpawnTable = Service->MMRSpawnTable.Get();

    if (MMRSpawnTable)
    {
        for (auto& Pair : MMRSpawnTable->RowMap)
        {
            auto Row = (FMMRSpawningBracketBotDataTableRow*)Pair.Value();

            if (!Row || MMR < Row->MMRBracketLow || MMR > Row->MMRBracketHigh || Row->PhaseSpawningItems.Num() == 0)
                continue;

            auto Info = uint64(&Service->CachedMMRSpawningInfo);
            auto& SpawningInfos = Service->CachedMMRSpawningInfo.SpawningInfos;
            auto& SpawningItems = Row->PhaseSpawningItems.Data[0].SpawningItems;
            float TotalWeight = 0.f;

            *(FMMRSpawningBracketBotDataTableRow**)(uint64(Service) + 0xE00) = Row;
            *(int32*)(Info + 0x8) = Row->MMRBracketLow;
            *(int32*)(Info + 0xC) = Row->MMRBracketHigh;

            ResetSpawningInfos(SpawningInfos, SpawningItems.Num());

            for (int32 i = 0; i < SpawningItems.Num(); i++)
            {
                auto Entry = uint64(&SpawningInfos.Data[i]);
                auto& Item = SpawningItems.Data[i];

                *(float*)Entry = Item.Skill;
                *(float*)(Entry + 0x4) = Item.Weight;
                *(UClass**)(Entry + 0x10) = Item.AISpawnerData;
                TotalWeight += Item.Weight;
            }

            *(int32*)(Info + 0x30) = (int32)TotalWeight;

            return CalculateSpawnAmountAndSkill(Service);
        }
    }

    UseFailSafeMMRInfo(Service, (float)MMR);
}

static void StartSpawning(UAthenaAIServicePlayerBots* Service, void* FlowNode)
{
    InitializeMMRInfos(Service);
    StartSpawningOG(Service, FlowNode);

    if (*(uint64*)(uint64(Service) + 0xE40))
        PauseWarmup(EGameModePauseWarmupMaskId::PlayerBotsSpawning);
}

static void UnpauseWarmUp(UAthenaAIServicePlayerBots* Service)
{
    UKismetSystemLibrary::K2_ClearAndInvalidateTimerHandle(Service, *(FTimerHandle*)(uint64(Service) + 0xE40));
    UnPauseWarmup(EGameModePauseWarmupMaskId::PlayerBotsSpawning);
}

static void OnAllBotsSpawned(UAthenaAIServicePlayerBots* Service)
{
    OnAllBotsSpawnedOG(Service);
    UnpauseWarmUp(Service);
}

static void UnpauseWarmupFailSafe(UAthenaAIServicePlayerBots* Service)
{
    UnpauseWarmupFailSafeOG(Service);

    if (!*(uint64*)(uint64(Service) + 0xE40))
        UnpauseWarmUp(Service);
}

static void FetchPlayerBotNameAndID(FString* OutBotName, FString* OutBotAccountID, AFortAthenaAIBotController* Controller)
{
    static auto Empty = (void (*)(FString*))(ImageBase + 0x4A3CDEC);
    static auto AssignRange = (void (*)(FString*, const wchar_t*, int32))(ImageBase + 0x18104B4);
    static auto Emplace = (void* (*)(void*, int32*, const FString*, bool*))(ImageBase + 0x24897E4);

    auto Service = Controller->CachedAIServicePlayerBots;
    auto Count = int32(std::size(BotNames));
    auto Start = rand() % Count;

    Empty(OutBotName);
    Empty(OutBotAccountID);

    for (int32 i = 0; i < Count; i++)
    {
        auto Name = BotNames[(Start + i) % Count];
        auto Length = int32(wcslen(Name));
        FString Candidate((wchar_t*)Name, Length + 1, Length + 1);
        int32 ElementId;
        bool bAlreadyInSet = false;

        Emplace(&Service->ReservedPlayerNames, &ElementId, &Candidate, &bAlreadyInSet);

        if (bAlreadyInSet)
            continue;

        AssignRange(OutBotName, Name, Length);
        return;
    }
}

static void FinishPendingRespawns(UAthenaAIServicePlayerBots* Service)
{
    static auto RespawnPlayerAfterDeath = (void (*)(AFortAthenaAIBotController*))(ImageBase + 0xA56D804);

    std::vector<AFortAthenaAIBotController*> RespawningBots;

    for (int32 i = 0; i < Service->PlayerBots.Num(); i++)
    {
        auto Controller = Service->PlayerBots.Data[i].BotController;

        if (Controller && *(uint64*)(uint64(Controller) + 0xC60))
            RespawningBots.push_back(Controller);
    }

    for (auto Controller : RespawningBots)
        RespawnPlayerAfterDeath(Controller);
}

static void UnspawnBotFromWarmup(UAthenaAIServicePlayerBots* Service)
{
    static auto EquipWeapon = (bool (*)(AFortPawn*, AFortWeapon*))(ImageBase + 0x9F20BEC);
    static auto SetIsInAnyStorm = (void (*)(AFortPlayerPawn*, bool))(ImageBase + 0x9F7D728);
    static auto ProcessMulticastDelegate = (void (*)(void*, void*))(ImageBase + 0x1FA44EC);
    static auto UnPossess = (void (*)(AController*))(ImageBase + 0x1D96B00);
    static auto Destroy = (bool (*)(AActor*, bool, bool))(ImageBase + 0x23CC010);
    static auto ResizeGrow = (void (*)(TArray<FFortServerBotInfo>*, int32))(ImageBase + 0x5B06384);

    if (!Service->CachedAIPopulationTracker)
        return;

    for (int32 i = 0; i < Service->PlayerBots.Num(); i++)
    {
        if (auto Controller = Service->PlayerBots.Data[i].BotController)
        {
            if (auto StartupInventory = Controller->StartupInventory)
            {
                auto InventoryOwner = (void*)(uint64(Controller) + 0x3F0);

                ((void (*)(void*, TArray<FItemAndCount>*))(*(void***)InventoryOwner)[0x4])(InventoryOwner, &StartupInventory->Items);
            }

            if (auto Pawn = Controller->PlayerBotPawn)
            {
                EquipWeapon(Pawn, nullptr);
                SetIsInAnyStorm(Pawn, false);
                ((void (*)(AFortPlayerPawnAthena*, bool))Pawn->VTable[0x271])(Pawn, true);
                ProcessMulticastDelegate(&Pawn->OnEnteredAircraft, nullptr);
                UnPossess(Controller);
                Destroy(Pawn, false, true);
            }
        }

        auto& PlayerBotsRestartLeft = Service->PlayerBotsRestartLeft;
        auto Num = PlayerBotsRestartLeft.NumElements;

        PlayerBotsRestartLeft.NumElements = Num + 1;

        if (PlayerBotsRestartLeft.NumElements > PlayerBotsRestartLeft.MaxElements)
            ResizeGrow(&PlayerBotsRestartLeft, Num);

        PlayerBotsRestartLeft.Data[Num] = Service->PlayerBots.Data[i];
    }
}

static void PlacePlayerBotsInAircraft(UAthenaAIServicePlayerBots* Service, UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    static auto IsBotEnabled = (bool (*)(UWorld*, bool))(ImageBase + 0xA6A8AEC);
    static auto GetAircraftIndex = (int32 (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, APlayerState*))(ImageBase + 0x1BB1F9C);
    static auto GetAircraft = (AFortAthenaAircraft* (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, int32))(ImageBase + 0x1BB1E8C);
    static auto ComputePOISquadRepartition = (void (*)(UAthenaAIServicePlayerBots*))(ImageBase + 0xA69F304);

    if (!IsBotEnabled(UWorld::GetWorld(), false) || !Service->CachedGameState || !Service->CachedAIPopulationTracker)
        return;

    for (int32 i = 0; i < Service->PlayerBots.Num(); i++)
    {
        auto Controller = Service->PlayerBots.Data[i].BotController;

        if (!Controller)
            continue;

        auto AircraftIndex = GetAircraftIndex(GamePhaseLogic, Controller->PlayerState);

        if (AircraftIndex >= 0 && AircraftIndex < GamePhaseLogic->Aircrafts_GameState.Num())
            EnterAircraft(Controller, GetAircraft(GamePhaseLogic, AircraftIndex));
    }

    ComputePOISquadRepartition(Service);
}

void ForceAllExitAircraft(UAthenaAIServicePlayerBots* Service)
{
    if (!Service->CachedAIPopulationTracker)
        return;

    for (int32 i = 0; i < Service->PlayerBots.Num(); i++)
    {
        auto Controller = Service->PlayerBots.Data[i].BotController;

        if (Controller && !Controller->PlayerBotPawn)
            ExitAircraft(Controller);
    }
}

static void PlacePlayersInAircraft(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    static auto GetService = (UAthenaAIServicePlayerBots* (*)(UWorld*))(ImageBase + 0x8D45068);

    PlacePlayersInAircraftOG(GamePhaseLogic);

    if (auto Service = GetService(UWorld::GetWorld()))
    {
        FinishPendingRespawns(Service);
        UnspawnBotFromWarmup(Service);
        PlacePlayerBotsInAircraft(Service, GamePhaseLogic);
    }
}

static void AssignBotTeam(UAthenaAIServicePlayerBots* Service, AFortAthenaAIBotController* Controller, AFortTeamInfo* TeamInfo, uint8 SquadId)
{
    static auto FindOrAddGroup = (void* (*)(void*, uint32))(ImageBase + 0x24ABAFC);
    static auto AddToGroup = (void (*)(void*, APlayerState*))(ImageBase + 0x24ABB50);
    static auto BroadcastGroupMemberAdded = (void (*)(void*, uint8, uint8, APlayerState*))(ImageBase + 0x329E8F8);

    AssignBotTeamOG(Service, Controller, TeamInfo, SquadId);

    auto PlayerState = (AFortPlayerStateAthena*)Controller->PlayerState;
    auto GameState = UWorld::GetWorld()->GameState;

    if (!Controller->bIsAnAthenaGameParticipant || !PlayerState || !GameState)
        return;

    AddToGroup(FindOrAddGroup((void*)(uint64(GameState) + 0x1A28), PlayerState->TeamIndex), PlayerState);
    AddToGroup(FindOrAddGroup((void*)(uint64(GameState) + 0x1A60), PlayerState->SquadID), PlayerState);
    BroadcastGroupMemberAdded((void*)(uint64(GameState) + 0x1C58), PlayerState->SquadID, PlayerState->TeamIndex, PlayerState);
}

void UAthenaAIServicePlayerBots__Init()
{
    static constexpr uint8_t FetchCall[] = { 0x48, 0x8D, 0x54, 0x24, 0x40, 0x4C, 0x8B, 0xC6, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90 };

    Hook<UAthenaAIServicePlayerBots>(uint32(0x59), OnPlaylistLoaded, OnPlaylistLoadedOG);
    Hook(ImageBase + 0xA6B9420, StartSpawning, StartSpawningOG);
    Hook(ImageBase + 0xA6AC1A8, OnAllBotsSpawned, OnAllBotsSpawnedOG);
    Hook(ImageBase + 0xA6BAC00, UnpauseWarmupFailSafe, UnpauseWarmupFailSafeOG);
    Hook(ImageBase + 0x2B54F10, PlacePlayersInAircraft, PlacePlayersInAircraftOG);
    Hook(ImageBase + 0xA69A8F8, AssignBotTeam, AssignBotTeamOG);

    for (int32 i = 0; i < int32(sizeof(FetchCall)); i++)
        Patch<uint8_t>(ImageBase + 0xA549BBE + i, FetchCall[i]);

    Rel32Hook(ImageBase + 0xA549BC6 + 1, FetchPlayerBotNameAndID);
}
