#include "pch.h"
#include "Shared.h"
#include "Config.h"
#include "AFortGameMode.h"
#include "AFortInventory.h"
#include "AFortPickup.h"
#include "ABuildingActor.h"
#include "AFortAthenaVehicle.h"
#include "UFortQuestManager.h"
#include "AFortPlayerController.h"
#include "AFortAthenaAIBotController.h"
#include "UFortControllerComponent_TournamentStatsTracker.h"

INIT_MODULE(AFortGameMode);
INIT_POSTLOAD(AFortGameMode);
INIT_TICKER(AFortGameMode);

static TArray<UFortAbilitySet*> AbilitySets;
static bool bSkipWarmup = false;
static bool bSkipAircraft = false;

static TArray<FVector> SafeZoneLocations;
static TArray<FName> LevelsToStream;

static int GetLateGamePhase(AFortGameStateAthena* GameState)
{
    auto& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
    auto Count = (int)SafeZoneDefinition.Count.Evaluate();
    auto FinalPhase = Count - 1;

    for (int i = 1; i < Count; i++)
    {
        if (SafeZoneDefinition.Radius.Evaluate((float)i) <= 0.f)
        {
            FinalPhase = i;
            break;
        }
    }

    return FinalPhase - 9 + LateGameZone;
}

struct FSharedRef
{
    void* Object;
    void* SharedReferenceCount;
};

struct FOnlineSessionSettings
{
    uint8 Pad_0[0x8];
    int32 NumPublicConnections;
    int32 NumPrivateConnections;
    bool bShouldAdvertise;
    bool bAllowJoinInProgress;
    bool bIsLANMatch;
    uint8 Pad_13[0xC5];
};

static void RegisterServer(AGameSession* GameSession)
{
    static auto GetSessionInterface = (FSharedRef* (*)(FSharedRef*, UWorld*, FName))(ImageBase + 0x71D8BA0);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto InitHostBeacon = (void (*)(AGameSession*, FOnlineSessionSettings*))(ImageBase + 0x9CD9E24);
    static auto StartMatchmakingBackfillV2 = (bool (*)(AGameSession*))(ImageBase + 0x9D2ECD8);
    static auto GetOnlineSubsystem = (uint8* (*)(void*))(ImageBase + 0x25336F0);
    static auto FindServicePermissions = (uint8* (*)(uint8*, void*))(ImageBase + 0x3C253EC);
    static auto MakeServicePermissions = (void* (*)(void*, void*, void*, void*))(ImageBase + 0x2BF1684);
    static auto AddServicePermissions = (int32 (*)(void*, void*))(ImageBase + 0x2BF1790);
    static auto DestroyServicePermissions = (void (*)(void*))(ImageBase + 0x1805494);

    FSharedRef SessionInterface{};
    GetSessionInterface(&SessionInterface, UWorld::GetWorld(), FName());

    if (SessionInterface.Object)
    {
        auto CreateSession = (bool (*)(void*, int32, FName, const FOnlineSessionSettings&))(*(void***)SessionInterface.Object)[0x9];
        auto GetSessionSettings = (FOnlineSessionSettings* (*)(void*, FName))(*(void***)SessionInterface.Object)[0x70];
        FOnlineSessionSettings SessionSettings{};

        SessionSettings.NumPublicConnections = GameSession->MaxPlayers;
        SessionSettings.bAllowJoinInProgress = true;
        SessionSettings.bIsLANMatch = true;

        CreateSession(SessionInterface.Object, 0, GameSession->SessionName, SessionSettings);

        auto CreatedSettings = GetSessionSettings(SessionInterface.Object, GameSession->SessionName);

        if (CreatedSettings && GameSession->IsA(AFortGameSessionDedicatedAthena::StaticClass()))
        {
            auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
            auto UpdateSessionSettings = (void (*)(AGameSession*, FOnlineSessionSettings*, int32, FString*, FString*))(*(void***)GameSession)[0x11D];

            UpdateSessionSettings(GameSession, CreatedSettings, GameMode->CurrentPlaylistId, &GameMode->CurrentZoneInstanceId, &GameMode->CurrentWUID);
            InitHostBeacon(GameSession, CreatedSettings);

            auto OnlineSubsystem = GetOnlineSubsystem((void*)(ImageBase + 0x12D183B8));
            auto McpService = OnlineSubsystem ? *(uint8**)(OnlineSubsystem + 0x2E8) : nullptr;

            if (McpService && !FindServicePermissions(McpService + 0x10, (void*)(ImageBase + 0x12D18A90)))
            {
                if (auto ClientPermissions = FindServicePermissions(McpService + 0x10, (void*)(ImageBase + 0x12D18A80)))
                {
                    uint8 ServerPermissions[0x30];
                    MakeServicePermissions(ServerPermissions, (void*)(ImageBase + 0x12D18A90), ClientPermissions + 0x10, ClientPermissions + 0x20);
                    AddServicePermissions(McpService + 0x10 + 0xB8, ServerPermissions);
                    DestroyServicePermissions(ServerPermissions);
                }
            }

            if (((AFortGameSession*)GameSession)->ReservationBeaconHost)
            {
                *(bool*)(__int64(GameSession) + 0x8A8) = true;
                *(bool*)(__int64(GameSession) + 0x8A9) = true;
                StartMatchmakingBackfillV2(GameSession);
            }
        }
    }

    TSharedRef_Destructor(&SessionInterface);
}

static void SetGamePhase(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, EAthenaGamePhase GamePhase)
{
    static auto SetGamePhaseInternal = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, EAthenaGamePhase))(ImageBase + 0xA4C8F3C);

    SetGamePhaseInternal(GamePhaseLogic, GamePhase);
}

static void SetGamePhaseStep(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, EAthenaGamePhaseStep GamePhaseStep)
{
    auto OldGamePhaseStep = GamePhaseLogic->GamePhaseStep;

    GamePhaseLogic->GamePhaseStep = GamePhaseStep;
    GamePhaseLogic->HandleGamePhaseStepChanged(GamePhaseStep);

    if (OldGamePhaseStep != GamePhaseStep)
    {
        static auto BroadcastGamePhaseStepUpdatedEvent = (void (*)(const UClass*, UObject*, EAthenaGamePhaseStep*, UObject*))(ImageBase + 0x3639070);

        BroadcastGamePhaseStepUpdatedEvent(AFortGameState::StaticClass(), GamePhaseLogic->GetOwner(), &GamePhaseStep, GamePhaseLogic);
    }
}

static void StartAircraftPhase(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    if (GamePhaseLogic->GamePhase >= EAthenaGamePhase::Aircraft)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
    auto Playlist = (UFortPlaylistAthena*)((AFortGameStateAthena*)GameMode->GameState)->CurrentPlaylistInfo.BasePlaylist;

    SetConsoleTitleA("Remix 32.00 | Match started");

    TournamentMatchStarted(GameMode);

    if (Playlist && Playlist->bAllowJoinInProgress)
        *(bool*)(__int64(GameMode) + 0x1018) = false;

    static auto GetSessionInterface = (FSharedRef* (*)(FSharedRef*, UWorld*, FName))(ImageBase + 0x71D8BA0);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);

    FSharedRef SessionInterface{};
    GetSessionInterface(&SessionInterface, UWorld::GetWorld(), FName());

    if (SessionInterface.Object)
    {
        auto GetSessionSettings = (FOnlineSessionSettings* (*)(void*, FName))(*(void***)SessionInterface.Object)[0x70];
        auto UpdateSession = (bool (*)(void*, FName, FOnlineSessionSettings&, bool))(*(void***)SessionInterface.Object)[0x13];

        if (auto SessionSettings = GetSessionSettings(SessionInterface.Object, GameMode->GameSession->SessionName))
        {
            SessionSettings->bAllowJoinInProgress = Playlist && Playlist->bAllowJoinInProgress;
            UpdateSession(SessionInterface.Object, GameMode->GameSession->SessionName, *SessionSettings, false);
        }
    }

    TSharedRef_Destructor(&SessionInterface);

    if (bSkipAircraft)
    {
        SetGamePhase(GamePhaseLogic, EAthenaGamePhase::SafeZones);
        SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormForming);

        return;
    }

    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;

    if (GameState->MapInfo->FlightInfos.Num() > 0)
    {
        auto& FlightInfo = GameState->MapInfo->FlightInfos[0];

        if (bLateGame)
        {
            GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;

            auto BusPhase = GetLateGamePhase(GameState) - 1;
            auto& Center = SafeZoneLocations.Data[BusPhase];
            auto Radius = GameState->MapInfo->SafeZoneDefinition.Radius.Evaluate((float)BusPhase);

            if (bLateGameMovingBus && Radius > 0.f)
            {
                auto Speed = FlightInfo.FlightSpeed > 0.f && Radius / 10.f > FlightInfo.FlightSpeed ? FlightInfo.FlightSpeed : Radius / 10.f;
                auto Yaw = FlightInfo.FlightStartRotation.Yaw;

                (FVector&)FlightInfo.FlightStartLocation = FVector(Center.X - UKismetMathLibrary::DegCos(Yaw) * Speed * 5.f, Center.Y - UKismetMathLibrary::DegSin(Yaw) * Speed * 5.f, 17500.);
                FlightInfo.FlightSpeed = Speed;
                FlightInfo.TimeTillFlightEnd = 10.f;
                FlightInfo.TimeTillDropEnd = 10.f;
            }
            else
            {
                (FVector&)FlightInfo.FlightStartLocation = FVector(Center.X, Center.Y, 17500.);
                FlightInfo.FlightSpeed = 0.f;
                FlightInfo.TimeTillFlightEnd = 7.f;
                FlightInfo.TimeTillDropEnd = 7.f;
            }

            FlightInfo.TimeTillDropStart = 0.f;
        }

        if (!GameState->MapInfo->AircraftClass.Get())
            return;
        auto Aircraft = AFortAthenaAircraft::SpawnAircraft(UWorld::GetWorld(), GameState->MapInfo->AircraftClass, FlightInfo, nullptr);

        if (!Aircraft)
            return;

        Aircraft->FlightElapsedTime = 0;
        Aircraft->DropStartTime = (float)Time + FlightInfo.TimeTillDropStart;
        Aircraft->DropEndTime = (float)Time + FlightInfo.TimeTillDropEnd;
        Aircraft->FlightStartTime = (float)Time;
        Aircraft->FlightEndTime = (float)Time + FlightInfo.TimeTillFlightEnd;
        Aircraft->ReplicatedFlightTimestamp = (float)Time;
        GamePhaseLogic->bAircraftIsLocked = true;

        for (auto& Player : GameMode->AlivePlayers)
        {
            auto Pawn = (AFortPlayerPawnAthena*)Player->Pawn;

            if (Pawn)
            {
                if (Pawn->Role == ENetRole::ROLE_Authority)
                {
                    if (Pawn->bIsInAnyStorm)
                    {
                        Pawn->bIsInAnyStorm = false;
                        Pawn->OnRep_IsInAnyStorm();
                    }
                }
                Pawn->bIsInsideSafeZone = true;
                Pawn->OnRep_IsInsideSafeZone();
                Pawn->OnEnteredAircraft.Process();
            }

            Player->ClientActivateSlot(EFortQuickBars(0), 0, 0.f, true, true);
            if (Pawn)
                Pawn->K2_DestroyActor();
            static auto Reset = (void (*)(AFortPlayerControllerAthena*))(ImageBase + 0x6F854DC);
            Reset(Player);
            Player->ClientGotoState(FName(L"Spectating"));
        }

        GamePhaseLogic->Aircrafts_GameMode.Add(TWeakObjectPtr<AFortAthenaAircraft>(Aircraft));
        GamePhaseLogic->Aircrafts_GameState.Add(TWeakObjectPtr<AFortAthenaAircraft>(Aircraft));
    }

    SetGamePhase(GamePhaseLogic, EAthenaGamePhase::Aircraft);
    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::BusLocked);
}

static void StartNewSafeZonePhase(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, int NewSafeZonePhase)
{
    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    auto SafeZoneIndicator = GamePhaseLogic->SafeZoneIndicator;
    auto& Array = SafeZoneIndicator->SafeZonePhases;

    if (Array.IsValidIndex(NewSafeZonePhase))
    {
        if (Array.IsValidIndex(NewSafeZonePhase - 1))
        {
            auto& PreviousPhaseInfo = Array[NewSafeZonePhase - 1];

            (FVector&)SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
            SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
        }

        auto& PhaseInfo = Array[NewSafeZonePhase];

        (FVector&)SafeZoneIndicator->NextCenter = PhaseInfo.Center;
        SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
        SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

        if (Array.IsValidIndex(NewSafeZonePhase + 1))
        {
            auto& NextPhaseInfo = Array[NewSafeZonePhase + 1];

            if (SafeZoneIndicator->FutureReplicator)
            {
                (FVector&)SafeZoneIndicator->FutureReplicator->NextNextCenter = NextPhaseInfo.Center;
                SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhaseInfo.Radius;
            }

            (FVector&)SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
            SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
            SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
        }

        SafeZoneIndicator->SafeZoneStartShrinkTime = bLateGame && bLateGameLongZone ? 676767.f : TimeSeconds + PhaseInfo.WaitTime;
        SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseInfo.ShrinkTime;

        SafeZoneIndicator->CurrentDamageInfo = PhaseInfo.DamageInfo;
        SafeZoneIndicator->OnRep_CurrentDamageInfo();

        auto OldPhase = SafeZoneIndicator->CurrentPhase;
        SafeZoneIndicator->CurrentPhase = NewSafeZonePhase;
        SafeZoneIndicator->OnRep_CurrentPhase();

        SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

        auto& SafeZoneState = *(uint8_t*)(__int64(SafeZoneIndicator) + 0x60C);
        SafeZoneState = 2;
        bool bInitial = OldPhase <= 0;

        SafeZoneIndicator->OnSafeZoneStateChange(EFortSafeZoneState::Holding, bInitial);
        SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, EFortSafeZoneState::Holding);

        static auto UpdateGamePhaseStep = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, float))(ImageBase + 0x1BB24D0);

        UpdateGamePhaseStep(GamePhaseLogic, 0.f);

        auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
        if (!bInitial)
            for (auto& PlayerController : GameMode->AlivePlayers)
                SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::StormPhase, 1, true);
    }
}

static void InitializeSafeZoneLocations(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    auto Playlist = (UFortPlaylistAthena*)GameState->CurrentPlaylistInfo.BasePlaylist;

    auto SafeZoneBlacklist = Playlist->SafeZoneLocationBlacklist.Get();

    if (!SafeZoneBlacklist)
        SafeZoneBlacklist = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaSafeZoneBlacklist.AthenaSafeZoneBlacklist");

    TArray<FVector4> BlacklistLocations;

    for (auto& Pair : GetRowMap(SafeZoneBlacklist))
    {
        auto Row = (FSimpleCurve*)Pair.Value();

        if (!Row)
            continue;

        FVector4 Loc{};

        for (auto& Key : Row->Keys)
        {
            if (Key.time == 0.f)
                Loc.X = Key.Value;
            else if (Key.time == 1.f)
                Loc.Y = Key.Value;
            else if (Key.time == 2.f)
                Loc.Z = Key.Value;
            else if (Key.time == 3.f)
                Loc.W = Key.Value;
        }

        if (Loc.X == 0 && Loc.Y == 0 && Loc.Z == 0 && Loc.W == 0)
            continue;

        BlacklistLocations.Add(Loc);
    }

    auto ZeroVector = FVector(0, 0, 0);
    auto SafeZoneCount = (float)Playlist->LastSafeZoneIndex;

    if (SafeZoneCount == -1)
        SafeZoneCount = GameState->MapInfo->SafeZoneDefinition.Count.Evaluate(0.f);
    else
        SafeZoneCount++;

    auto Center = GameState->MapInfo->GetMapCenter();

    SafeZoneLocations.Clear();
    SafeZoneLocations.Reserve((int)SafeZoneCount);

    for (int i = (int)(SafeZoneCount - 1); i >= 0; i--)
    {
        auto Params = GameState->MapInfo->ConstructSafeZoneLocationParams(i, Center, i == SafeZoneCount - 1 ? ZeroVector : SafeZoneLocations.Data[i + 1], i == SafeZoneCount - 1, 0);

        auto Location = GameState->MapInfo->PickSafeZoneLocation(Params, BlacklistLocations);

        SafeZoneLocations.Data[i] = Location;
    }

    auto Count = (int32)SafeZoneCount;

    if (Count <= 0)
        return;

    static auto SetSafeZoneLocations = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, TArray<FVector>*))(ImageBase + 0xA4C93D4);

    TArray<FVector> Locations;

    for (int32 i = 0; i < Count; i++)
        Locations.Add(SafeZoneLocations.Data[i]);

    SetSafeZoneLocations(GamePhaseLogic, &Locations);
    Locations.Free();
}

static FProperty* FindProperty(const UObject* Object, const char* Name)
{
    for (const UStruct* Struct = Object->Class; Struct; Struct = Struct->SuperStruct)
    {
        auto Link = uint64(Struct->ChildProperties);

        for (auto Field = Link ? (FField*)(_rotr64(Link, 0x38) - 0xD969BB85ull) : nullptr; Field; Field = Field->GetNext())
        {
            if (Field->Name.ToString() == Name)
                return (FProperty*)Field;
        }
    }

    return nullptr;
}

template <typename T>
static T& GetProperty(const UObject* Object, const FProperty* Property)
{
    return *(T*)(__int64(Object) + (Property->Offset ^ 0x019039A6));
}

static bool HasTag(const TArray<FGameplayTag>& Tags, const FGameplayTag& Tag)
{
    for (auto& Other : Tags)
    {
        if (Other.TagName == Tag.TagName)
            return true;
    }

    return false;
}

static bool SelectCalendarPlaylist()
{
    static bool bDone = false;
    static ULONGLONG WaitStart = 0;

    if (bDone || bReload)
        return true;

    auto ManagerClass = UObject::FindClassFast("BP_Clyde_DataLayerManager_C");
    auto Manager = ManagerClass ? GetFirstInstance(ManagerClass) : nullptr;
    auto CurrentPlaylist = FindObject<UFortPlaylistAthena>(Playlist.c_str());

    if (!Manager || !CurrentPlaylist)
        return bDone = true;

    auto EventsProperty = FindProperty(Manager, "ClydeWeekCalendarEventsToWatch");
    auto WeekTagsProperty = FindProperty(Manager, "Apollo Retro Seasonly Gameplay Tags");
    auto UsePlaylistsProperty = (FBoolProperty*)FindProperty(Manager, "bShouldUseClydePlaylists");

    if (!EventsProperty || !WeekTagsProperty)
        return bDone = true;

    if (UsePlaylistsProperty && (GetProperty<uint8>(Manager, UsePlaylistsProperty) & UsePlaylistsProperty->FieldMask))
        return bDone = true;

    auto& Events = GetProperty<TArray<FString>>(Manager, EventsProperty);
    auto& WeekTags = GetProperty<TArray<FGameplayTag>>(Manager, WeekTagsProperty);
    auto& PlaylistTags = CurrentPlaylist->GameplayTagContainer.GameplayTags;

    for (auto& Tag : PlaylistTags)
    {
        if (HasTag(WeekTags, Tag))
            return bDone = true;
    }

    auto World = UWorld::GetWorld();
    auto GameInstance = World ? (UFortGameInstance*)World->OwningGameInstance : nullptr;
    auto EventManager = GameInstance ? GameInstance->SeasonalEventManager : nullptr;

    if (!EventManager)
        return bDone = true;

    if (*(uint8*)(__int64(EventManager) + 0x180))
    {
        if (!WaitStart)
            WaitStart = GetTickCount64();

        if (GetTickCount64() - WaitStart < 60000)
            return false;

        return bDone = true;
    }

    int32 Week = -1;

    for (int32 i = 0; i < Events.Num(); i++)
    {
        bool bIsEventActive = false;
        FTimespan TimeSinceBegin{};
        FTimespan TimeUntilEnd{};
        float TimespanRatio = 0.f;

        UFortKismetLibrary::GetCalendarEventInformation(World, Events[i], &bIsEventActive, &TimeSinceBegin, &TimeUntilEnd, &TimespanRatio);

        if (bIsEventActive)
            Week = i;
    }

    if (Week < 0 || Week >= WeekTags.Num())
        return bDone = true;

    auto& WeekTag = WeekTags[Week];

    const UObject* ManagerPackage = Manager->Class;

    while (ManagerPackage && ManagerPackage->Outer)
        ManagerPackage = ManagerPackage->Outer;

    auto PackageName = ManagerPackage ? ManagerPackage->Name.GetRawString() : std::string();
    auto RootEnd = PackageName.size() > 1 ? PackageName.find('/', 1) : std::string::npos;

    if (RootEnd == std::string::npos)
        return bDone = true;

    std::wstring Root(PackageName.begin(), PackageName.begin() + RootEnd + 1);
    auto Level = CurrentPlaylist->PreloadPersistentLevel.ObjectID.AssetPath.PackageName.GetRawString();
    std::wstring MatchPath;
    int32 Matches = 0;
    int32 BestSharedTags = -1;

    TArray<FPrimaryAssetId> AssetIds;
    UKismetSystemLibrary::GetPrimaryAssetIdList(FPrimaryAssetType{ FName(L"FortPlaylistAthena") }, &AssetIds);

    for (auto& AssetId : AssetIds)
    {
        auto PathString = UKismetSystemLibrary::Conv_SoftObjectReferenceToString(UKismetSystemLibrary::GetSoftObjectReferenceFromPrimaryAssetId(AssetId));

        if (PathString.Num() <= 1)
        {
            PathString.Free();
            continue;
        }

        std::wstring Path(PathString.Data);
        PathString.Free();

        if (Path.compare(0, Root.size(), Root) != 0)
            continue;

        auto Candidate = FindObject<UFortPlaylistAthena>(Path.c_str());

        if (!Candidate || Candidate == CurrentPlaylist || Candidate->MaxSquadSize != CurrentPlaylist->MaxSquadSize || Candidate->PreloadPersistentLevel.ObjectID.AssetPath.PackageName.GetRawString() != Level)
            continue;

        bool bHasWeekTag = false;
        bool bSameMode = true;
        int32 SharedTags = 0;

        for (auto& Tag : Candidate->GameplayTagContainer.GameplayTags)
        {
            if (Tag.TagName == WeekTag.TagName)
                bHasWeekTag = true;
            else if (HasTag(PlaylistTags, Tag))
                SharedTags++;
            else
                bSameMode = false;
        }

        if (!bHasWeekTag || !bSameMode || SharedTags < BestSharedTags)
            continue;

        if (SharedTags > BestSharedTags)
        {
            BestSharedTags = SharedTags;
            Matches = 0;
        }

        Matches++;
        MatchPath = Path;
    }

    AssetIds.Free();

    if (Matches == 1)
        Playlist = MatchPath;

    return bDone = true;
}

static std::wstring GetPluginName(const FName& LevelToStream)
{
    auto Level = LevelToStream.GetRawString();
    auto PluginName = Level.substr(1, Level.find('/', 1) - 1);

    return std::wstring(PluginName.begin(), PluginName.end());
}

static bool IsGameFeaturePluginActive(const std::wstring& PluginName)
{
    static auto GetGameFeatureDataForActivePlugin = (UGameFeatureData* (*)(UGameFeaturePluginStateMachine*))(ImageBase + 0x29EB49C);
    auto PluginDescriptor = L"/" + PluginName + L".uplugin";

    for (auto& Pair : ((UGameFeaturesSubsystem*)USubsystemBlueprintLibrary::GetEngineSubsystem(UGameFeaturesSubsystem::StaticClass()))->GameFeaturePluginStateMachines)
    {
        if (Pair.Key().ToWString().ends_with(PluginDescriptor))
            return Pair.Value() && GetGameFeatureDataForActivePlugin(Pair.Value());
    }

    return false;
}

static std::unordered_map<int, float> WeightMap;
static float Sum = 0;
static float Weight;
static float TotalWeight;

static void (*ReadyToStartMatchOG)(UObject*, FFrame&, bool*);
static void ReadyToStartMatch(UObject* Context, FFrame& Stack, bool* Ret)
{
    Stack.IncrementCode();

    if (Context->IsA(AFortGameModeFrontEnd::StaticClass()))
    {
        *Ret = callOGWithRet(((AGameMode*)Context), Stack.CurrentNativeFunction, ReadyToStartMatch);
        return;
    }

    auto GameMode = (AFortGameModeAthena*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (!MissedSelfSpawns.empty() && GameState->bPlaylistDataIsLoaded)
        ReplayMissedSelfSpawns();

    if (GameMode->WarmupRequiredPlayerCount != 1)
    {
        if (!SelectCalendarPlaylist())
        {
            *Ret = false;
            return;
        }

        auto World = UWorld::GetWorld();
        auto Engine = UEngine::GetEngine();
        auto NetDriverName = FName(L"GameNetDriver");

        GameMode->bEnableReplicationGraph = true;

        static auto GetWorldContextFromWorld = (void* (*)(UEngine*, UWorld*))(ImageBase + 0x175C254);
        static auto CreateNetDriver_Local = (UNetDriver* (*)(UEngine*, void*, FName, FName))(ImageBase + 0x1536974);
        static auto SetWorld = (void (*)(UNetDriver*, UWorld*))(ImageBase + 0x2494394);
        static auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))(ImageBase + 0x71BBFDC);

        auto WorldContext = GetWorldContextFromWorld(Engine, World);
        auto NetDriver = World->NetDriver = CreateNetDriver_Local(Engine, WorldContext, NetDriverName, FName());

        NetDriver->NetServerMaxTickRate = 30;
        NetDriver->NetDriverName = NetDriverName;
        NetDriver->World = World;
        *(bool*)(__int64(NetDriver) + 0x811) = true;

        for (auto& LevelCollection : World->LevelCollections)
            LevelCollection.NetDriver = NetDriver;

        FURL URL{};
        URL.Port = Port;

        SetWorld(NetDriver, World);
        FString Error;
        if (InitListen(NetDriver, World, &URL, false, Error))
            SetWorld(NetDriver, World);

        GameMode->WarmupRequiredPlayerCount = 1;

        auto Playlist = FindObject<UFortPlaylistAthena>(::Playlist.c_str());

        if (!Playlist)
            Playlist = FindObject<UFortPlaylistAthena>(L"/BRPlaylists/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

        if (Playlist)
        {
            Playlist->GarbageCollectionFrequency = 9999999999999999.f;
            Playlist->bEnableDynamicBotBackfill = false;
            GameMode->PlaylistHotfixOriginalGCFrequency = 9999999999999999.f;
            GameMode->bDisableGCOnServerDuringMatch = true;
            GameMode->bPlaylistHotfixChangedGCDisabling = true;

            bForceRespawns = bForceRespawns && Playlist->RespawnType == EAthenaRespawnType::None;

            if (bForceRespawns)
            {
                Playlist->RespawnType = bStormRespawns ? EAthenaRespawnType::InfiniteRespawn : EAthenaRespawnType::InfiniteRespawnExceptStorm;
                Playlist->bRespawnInAir = true;
                Playlist->RespawnHeight.Value = 20000.f;
                Playlist->RespawnHeight.Curve.CurveTable = nullptr;
                Playlist->RespawnHeight.Curve.RowName = FName();
                Playlist->RespawnTime.Value = 3.f;
                Playlist->RespawnTime.Curve.CurveTable = nullptr;
                Playlist->RespawnTime.Curve.RowName = FName();
                Playlist->bAllowJoinInProgress = true;
            }

            GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
            GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
            GameState->CurrentPlaylistInfo.MarkArrayDirty();
            GameState->OnRep_CurrentPlaylistInfo();

            GameMode->CurrentPlaylistId = Playlist->PlaylistId;
            GameState->CurrentPlaylistId = Playlist->PlaylistId;
            GameMode->CurrentPlaylistName = Playlist->PlaylistName;
            GameMode->GameSession->MaxPlayers = Playlist->MaxPlayers;

            auto StreamingPlaylist = GameState->CurrentPlaylistInfo.OverridePlaylist ? GameState->CurrentPlaylistInfo.OverridePlaylist : GameState->CurrentPlaylistInfo.BasePlaylist;
            static auto StartStreamingAdditionalPlaylistLevel = (void (*)(AFortGameStateAthena*, FName, bool))(ImageBase + 0x984CF30);

            for (auto& AdditionalLevel : StreamingPlaylist->AdditionalLevels)
                StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, false);

            for (auto& AdditionalLevel : StreamingPlaylist->AdditionalLevelsServerOnly)
                StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, true);

            if (StreamingPlaylist->SharedAssetGroup)
                for (auto& SharedAsset : StreamingPlaylist->SharedAssetGroup->SharedAssetsToLoad)
                    for (auto& AdditionalLevel : SharedAsset->SharedAdditionalLevels)
                        StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, false);

            if (auto UEFNProjects = (UFortGameStateComponent_UEFNProjects*)GameState->GetComponentByClass(UFortGameStateComponent_UEFNProjects::StaticClass()))
            {
                static auto GetLevelsToStream = (bool (*)(UFortGameStateComponent_UEFNProjects*, TArray<FName>*))(ImageBase + 0x90D8CA0);
                static auto ChangeGameFeatureTargetState = (void (*)(UGameFeaturesSubsystem*, const FString&, EGameFeatureTargetState, const void*))(ImageBase + 0x189B27C);

                GetLevelsToStream(UEFNProjects, &LevelsToStream);

                for (auto& LevelToStream : LevelsToStream)
                {
                    auto PluginName = GetPluginName(LevelToStream);
                    auto PluginURL = FString((L"file:../../../FortniteGame/Plugins/GameFeatures/" + PluginName + L"/" + PluginName + L".uplugin").c_str());
                    uint8 CompleteDelegate[0x10]{};

                    ChangeGameFeatureTargetState((UGameFeaturesSubsystem*)USubsystemBlueprintLibrary::GetEngineSubsystem(UGameFeaturesSubsystem::StaticClass()), PluginURL, EGameFeatureTargetState::Active, CompleteDelegate);
                    PluginURL.Free();
                }
            }
        }
        else
        {
            GameState->CurrentPlaylistId = GameMode->CurrentPlaylistId = 0;
            GameMode->GameSession->MaxPlayers = 100;
        }

        RegisterServer(GameMode->GameSession);

        if (auto MeshNetworkSubsystem = (UMeshNetworkSubsystem*)USubsystemBlueprintLibrary::GetGameInstanceSubsystem(World, UMeshNetworkSubsystem::StaticClass()))
            MeshNetworkSubsystem->NodeType = EMeshNetworkNodeType::Edge;

        if (!GameMode->AIDirector)
        {
            GameMode->AIDirector = SpawnActor<AFortAIDirector>(AAthenaAIDirector::StaticClass(), FVector{}, {}, GameMode);
            if (GameMode->AIDirector)
                GameMode->AIDirector->Activate();
        }

        if (Playlist)
        {
            static auto SetAISettings = (void (*)(AFortGameModeAthena*, UAthenaAISettings*))(ImageBase + 0x8F991FC);
            static auto OnGameModeInitialized = (void (*)(UAthenaAIServiceManager*, AFortGameModeAthena*))(ImageBase + 0xA6ADD4C);

            if (auto AISettings = Playlist->AISettings.Get())
            {
                auto& AIServices = AISettings->AIServices;
                bool bHasPlayerBotsService = false;

                for (int32 i = 0; i < AIServices.Num(); i++)
                {
                    auto ServiceClass = (UClass*)AIServices[i];

                    if (ServiceClass && ServiceClass->ClassDefaultObject->IsA(UAthenaAIServicePlayerBots::StaticClass()))
                        bHasPlayerBotsService = true;
                }

                for (int32 i = 0; i < AIServices.Num() && !bHasPlayerBotsService; i++)
                {
                    if (!(UClass*)AIServices[i])
                    {
                        AIServices[i] = UAthenaAIServicePlayerBots::StaticClass();
                        bHasPlayerBotsService = true;
                    }
                }

                auto AISystem = (UFortAISystem*)World->AISystem;

                if (AISystem && AISystem->IsA(UAthenaAISystem::StaticClass()))
                    CreateSubsystems((UAthenaAISystem*)AISystem, GameMode);

                SetAISettings(GameMode, AISettings);

                if (AISystem && AISystem->IsA(UFortAISystem::StaticClass()) && AISystem->AIServiceManager)
                    OnGameModeInitialized(AISystem->AIServiceManager, GameMode);
            }
        }

        if (!GameMode->AIGoalManager)
            GameMode->AIGoalManager = SpawnActor<AFortAIGoalManager>(AFortAIGoalManager::StaticClass(), FVector{}, {}, GameMode);

        if (!GameMode->SpawningPolicyManager)
        {
            GameMode->SpawningPolicyManager = SpawnActor<AFortAthenaSpawningPolicyManager>(AFortAthenaSpawningPolicyManager::StaticClass(), FVector{});
            GameMode->SpawningPolicyManager->GameStateAthena = GameState;
            GameMode->SpawningPolicyManager->GameModeAthena = GameMode;
        }

        if (auto PoiManager = GameState->PoiManager; PoiManager && !PoiManager->bHasInitialized)
        {
            static auto InitializePoiManager = (void (*)(AFortPoiManager*))(ImageBase + 0x908918C);
            InitializePoiManager(PoiManager);
        }

        if (World->NetDriver && GameState->HasBegunPlay())
        {
            static auto UpdateCullDistanceServer = (void (*)(ABuildingActor*))(ImageBase + 0x92D226C);

            TArray<ABuildingActor*> BuildingActors;
            GetAll<ABuildingActor>(BuildingActors);

            for (auto& BuildingActor : BuildingActors)
            {
                if (BuildingActor && !BuildingActor->bAlwaysUseNetCullDistanceSquaredForRelevancy)
                    UpdateCullDistanceServer(BuildingActor);
            }

            BuildingActors.Free();
        }

        *Ret = false;
        return;
    }

    if (!GameMode->bWorldIsReady)
    {
        TArray<APlayerStart*> Starts;
        GetAll<APlayerStart>(Starts);
        auto StartsNum = Starts.Num();
        Starts.Free();

        if (StartsNum == 0 || !bInit)
        {
            *Ret = false;
            return;
        }

        TArray<AFortAthenaMapInfo*> AllMapInfos;
        GetAll<AFortAthenaMapInfo>(AllMapInfos);
        auto MapInfosNum = AllMapInfos.Num();
        AllMapInfos.Free();

        if (!GameState->MapInfo && (MapInfosNum > 0 || LevelsToStream.Num() > 0))
        {
            static auto StartStreamingAdditionalPlaylistLevel = (void (*)(AFortGameStateAthena*, FName, bool))(ImageBase + 0x984CF30);
            static auto InitializeMapInfo = (AFortAthenaMapInfo* (*)(AFortGameModeAthena*, AFortGameStateAthena*))(ImageBase + 0x8F9DDA0);

            for (auto& LevelToStream : LevelsToStream)
            {
                if (!IsGameFeaturePluginActive(GetPluginName(LevelToStream)))
                {
                    *Ret = false;
                    return;
                }

                StartStreamingAdditionalPlaylistLevel(GameState, LevelToStream, false);
            }

            InitializeMapInfo(GameMode, GameState);

            if (!GameState->MapInfo)
            {
                *Ret = false;
                return;
            }
        }

        if (GameState->MapInfo)
        {
            if (auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameState))
            {
                static auto InitializeFlightPath = (void (*)(AFortAthenaMapInfo*, AFortGameStateAthena*, UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, bool, double, float, float))(ImageBase + 0x8F0EF30);

                InitializeSafeZoneLocations(GamePhaseLogic);
                InitializeFlightPath(GameState->MapInfo, GameState, GamePhaseLogic, false, 0.f, 0.f, 360.f);
            }
        }

        auto Playlist = GameState->CurrentPlaylistInfo.BasePlaylist;

        if (Playlist)
        {
            bSkipWarmup = Playlist->bSkipWarmup;
            bSkipAircraft = Playlist->bSkipAircraft;
        }

        bLateGame = bLateGame && Playlist && (Playlist->RespawnType == EAthenaRespawnType::None || bForceRespawns) && !bSkipAircraft && GameState->MapInfo && LateGameZone >= 1 && GetLateGamePhase(GameState) >= 1 &&
                    GetLateGamePhase(GameState) < (int)GameState->MapInfo->SafeZoneDefinition.Count.Evaluate();

        auto AbilitySet = FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer");
        AbilitySets.Add(AbilitySet);

        auto TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprintGame/Gameplay/AS_TacticalSprint.AS_TacticalSprint");

        if (!TacticalSprintAbility)
            TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprint/Gameplay/AS_TacticalSprint.AS_TacticalSprint");
        AbilitySets.Add(TacticalSprintAbility);

        auto AscenderAbility = FindObject<UFortAbilitySet>(L"/Ascender/Gameplay/Ascender/AS_Ascender.AS_Ascender");
        AbilitySets.Add(AscenderAbility);

        auto DoorBashAbility = FindObject<UFortAbilitySet>(L"/DoorBashContent/Gameplay/AS_DoorBash.AS_DoorBash");
        AbilitySets.Add(DoorBashAbility);

        auto HillScrambleAbility = FindObject<UFortAbilitySet>(L"/HillScramble/Gameplay/AS_HillScramble.AS_HillScramble");
        AbilitySets.Add(HillScrambleAbility);

        auto SlideImpulseAbility = FindObject<UFortAbilitySet>(L"/SlideImpulse/Gameplay/AS_SlideImpulse.AS_SlideImpulse");
        AbilitySets.Add(SlideImpulseAbility);

        if (Playlist)
            for (auto& ModifierPtr : Playlist->ModifierList)
            {
                auto Modifier = ModifierPtr.Get();

                if (!Modifier)
                    continue;

                for (auto& DeliveryInfo : Modifier->PersistentAbilitySets)
                {
                    if (!DeliveryInfo.DeliveryRequirements.bApplyToPlayerPawns)
                        continue;

                    for (auto& AbilitySetPtr : DeliveryInfo.AbilitySets)
                        AbilitySets.Add(AbilitySetPtr.Get());
                }
            }

        for (auto& Set : AbilitySets)
            if (Set)
                Set->AddToRoot();

        GameState->DefaultParachuteDeployTraceForGroundDistance = 10000;

        if (auto GrindRailGameData = FindObject<UCurveTable>(L"/GrindRail/DataTables/GrindRailGameData.GrindRailGameData"))
        {
            static FName UseGrindingMME = FName(L"Default.GrindRails.UseGrindingMME");

            for (auto& Pair : GetRowMap(GrindRailGameData))
            {
                if (Pair.Key() != UseGrindingMME)
                    continue;

                auto Row = (FSimpleCurve*)Pair.Value();

                if (!Row)
                    continue;

                for (auto& Key : Row->Keys)
                    Key.Value = 0.f;
            }
        }

        if (GameState->MapInfo)
        {
            auto& VendingMachineRarityCount = GameState->MapInfo->VendingMachineRarityCount;

            for (int i = 0; i < 6; i++)
            {
                float RarityWeight;
                UDataTableFunctionLibrary::EvaluateCurveTableRow(VendingMachineRarityCount.Curve.CurveTable, VendingMachineRarityCount.Curve.RowName, (float)i, nullptr, &RarityWeight, FString());

                WeightMap[i] = RarityWeight;
                Sum += RarityWeight;
            }

            UDataTableFunctionLibrary::EvaluateCurveTableRow(VendingMachineRarityCount.Curve.CurveTable, VendingMachineRarityCount.Curve.RowName, 0.f, nullptr, &Weight, FString());

            TotalWeight = 0.0f;

            for (auto& [Rarity, RarityWeight] : WeightMap)
                TotalWeight += RarityWeight;
        }

        GameMode->DefaultPawnClass = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

        for (auto& Pair : GameState->AllPlayerBuildableClassesIndexLookup)
            BuildingClassMap[Pair.Value()] = (UClass*)Pair.Key();

        bReady = true;
        SetConsoleTitleA("Remix 32.00 | Joinable");
        GameMode->bWorldIsReady = true;
    }

    int ReadyPlayers = 0;
    TArray<AFortPlayerControllerAthena*> PlayerList;
    GetAll<AFortPlayerControllerAthena>(PlayerList);

    for (auto& PlayerController : PlayerList)
    {
        if (!PlayerController->PlayerState->bIsSpectator && PlayerController->bReadyToStartMatch)
            ReadyPlayers++;
    }

    PlayerList.Free();

    auto VolumeManager = GameState->VolumeManager;

    bool bAllLevelsFinishedStreaming = true;
    auto& AdditionalPlaylistLevels = *(TArray<FPlaylistStreamedLevelData>*)(__int64(GameState) + 0x430);

    for (auto& AdditionalPlaylistLevel : AdditionalPlaylistLevels)
    {
        auto StreamingLevel = *(ULevelStreaming**)&AdditionalPlaylistLevel;

        if (!AdditionalPlaylistLevel.bIsFinishedStreaming || !StreamingLevel || !StreamingLevel->LoadedLevel->bIsVisible)
        {
            bAllLevelsFinishedStreaming = false;
            break;
        }
    }

    static auto WaitingToStart = FName(L"WaitingToStart");
    *Ret = GameMode->bWorldIsReady && GameState->bPlaylistDataIsLoaded && GameMode->MatchState == WaitingToStart && bAllLevelsFinishedStreaming && (!VolumeManager || !GameState->bInSpawningStartup) &&
           ReadyPlayers >= GameMode->WarmupRequiredPlayerCount;
}

static void SpawnDefaultPawnFor(UObject* Context, FFrame& Stack, APawn** Ret)
{
    AController* NewPlayer;
    AActor* StartSpot;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.StepCompiledIn(&StartSpot);
    Stack.IncrementCode();
    auto GameMode = (AFortGameModeAthena*)Context;

    if (!NewPlayer || !StartSpot)
        return;

    auto Pawn = (APawn*)SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding);

    while (!Pawn)
    {
        auto PlayerStart = GameMode->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (APawn*)SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), PlayerStart->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding);
    }

    *Ret = Pawn;

    if (!NewPlayer->IsA(AFortPlayerControllerAthena::StaticClass()))
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)NewPlayer;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Num = WorldInventory ? WorldInventory->Inventory.ReplicatedEntries.Num() : 0;

    if (Num == 0)
    {
        auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

        RegisterWithQuestManager(PlayerController);

        PlayerState->SeasonLevelUIDisplay = PlayerController->XPComponent->CurrentUILevel;
        PlayerState->OnRep_SeasonLevelUIDisplay();
    }
}

enum class EHttpRequestStatus : int32
{
    NotStarted,
    Processing,
    Failed,
    Failed_ConnectionError,
    Succeeded
};

struct FStringView
{
    const wchar_t* DataPtr;
    int32 Size;
};

struct FVoiceChatTokenRequest
{
    TWeakObjectPtr<AFortPlayerControllerZone> PlayerController;
    int32 RequestID;
    FString ChannelName;
    FSharedRef HttpRequest;
};

static std::vector<FVoiceChatTokenRequest> VoiceChatTokenRequests;

static const std::wstring& GetCurrentGameSessionId()
{
    static const auto GameSessionId = []
    {
        std::random_device Device;
        return std::format(L"{:08x}{:08x}{:08x}{:08x}", Device(), Device(), Device(), Device());
    }();

    return GameSessionId;
}

static std::wstring GenerateVoiceChatChannel(AFortGameModeAthena* GameMode, AFortPlayerControllerZone* PlayerController)
{
    static auto EvaluateVoiceChannelMutators = (bool (*)(AFortGameModeAthena*, FString*, AFortPlayerControllerZone*))(ImageBase + 0x8F8F2E0);
    static auto GetCurrentPlaylistData = (UFortPlaylistAthena* (*)(AFortGameStateAthena*))(ImageBase + 0x4A3E090);
    static auto AsBool = (bool (*)(const FScalableFloat*, float))(ImageBase + 0x1B741C4);
    static auto& EnableVoiceChannelMutatorUsage = *(int32*)(ImageBase + 0x12A9A1BC);

    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    if (!GameState || !GameState->IsA(AFortGameStateAthena::StaticClass()) || !PlayerState || !PlayerState->IsA(AFortPlayerStateAthena::StaticClass()))
        return L"";

    auto& GameSessionId = GetCurrentGameSessionId();
    FString MutatorChannel;
    auto bMutatorChannel = EnableVoiceChannelMutatorUsage && EvaluateVoiceChannelMutators(GameMode, &MutatorChannel, PlayerController);
    auto MutatorChannelName = MutatorChannel.Num() > 1 ? MutatorChannel.ToWString() : L"";

    MutatorChannel.Free();

    if (bMutatorChannel)
        return MutatorChannelName.empty() ? L"" : GameSessionId + L"." + MutatorChannelName;

    auto Playlist = GetCurrentPlaylistData(GameState);

    if ((Playlist && Playlist->bForceDisableTeamOrSquadGameChannel) || PlayerState->bOnlySpectator)
        return L"";

    auto CustomGameChannel = Playlist && AsBool(&Playlist->ShouldUseCustomGameChannel, 0.f) ? Playlist->CustomGameChannel : ECustomGameVoiceChannel::Squad;

    if (CustomGameChannel == ECustomGameVoiceChannel::WholeServer)
        return GameSessionId + L".WholeServer";

    if (CustomGameChannel == ECustomGameVoiceChannel::FullTeam)
        return GameSessionId + L"." + std::to_wstring(PlayerState->TeamIndex);

    int32 NumberOfPlayersInSquad = 0;

    for (auto& Member : GameState->GameMemberInfoArray.Members)
    {
        if (Member.SquadID == PlayerState->SquadID)
            NumberOfPlayersInSquad++;
    }

    return NumberOfPlayersInSquad > 1 ? GameSessionId + L"." + std::to_wstring(PlayerState->SquadID) : L"";
}

static void SendVoiceChatChannelJoin(AFortGameModeZone* GameMode, AFortPlayerControllerZone* PlayerController)
{
    static auto VoiceChatChannelName = FName(L"VoiceChatChannel");

    if (!PlayerController)
        return;

    auto Channel = GenerateVoiceChatChannel((AFortGameModeAthena*)GameMode, PlayerController);

    if (Channel == PlayerController->VoiceChatChannel.ToWString())
        return;

    PlayerController->VoiceChatChannel.Free();
    PlayerController->VoiceChatChannel = Channel.empty() ? FString() : FString(Channel.c_str());
    UNetPushModelHelpers::MarkPropertyDirty(PlayerController, VoiceChatChannelName);
}

static void SendVoiceChatToken(AFortGameModeZone*, AFortPlayerControllerZone* PlayerController, int32 RequestID, bool, const FString& ChannelName)
{
    static auto IOnlineSubsystem_Get = (void* (*)(const FName&))(ImageBase + 0x25336F0);
    static auto GetMcpGameService = (FSharedRef* (*)(void*, FSharedRef*))(ImageBase + 0x30223F4);
    static auto GetIdentityInterface = (FSharedRef* (*)(void*, FSharedRef*))(ImageBase + 0x2BD8F1C);
    static auto ProcessRequest = (void (*)(void*, void*, const void*, const FSharedRef*))(ImageBase + 0x749A688);
    static auto ToString = (FString* (*)(const FUniqueNetIdRepl*, FString*))(ImageBase + 0x3023E44);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto& McpSubsystemName = *(FName*)(ImageBase + 0x12D183B8);

    if (!PlayerController)
        return;

    auto PlayerState = PlayerController->PlayerState;
    auto& VoiceChatChannel = PlayerController->VoiceChatChannel;
    auto Subsystem = PlayerState && VoiceChatChannel.Num() > 1 ? IOnlineSubsystem_Get(McpSubsystemName) : nullptr;
    FSharedRef GameService{};
    FSharedRef Identity{};
    FSharedRef UserId{};

    if (Subsystem)
    {
        GetMcpGameService(Subsystem, &GameService);
        GetIdentityInterface(Subsystem, &Identity);
    }

    if (Identity.Object)
    {
        auto GetUniquePlayerId = (FSharedRef* (*)(void*, FSharedRef*, int32))(*(void***)Identity.Object)[0x22];

        GetUniquePlayerId(Identity.Object, &UserId, 0);
    }

    if (GameService.Object && UserId.Object)
    {
        auto CreateRequest = (FSharedRef* (*)(void*, FSharedRef*))(*(void***)GameService.Object)[0x1B];
        auto GetBaseUrl = (FString* (*)(void*, FString*))(*(void***)GameService.Object)[0xB];
        FSharedRef HttpRequest{};
        FString BaseUrl;
        FString AccountId;

        CreateRequest(GameService.Object, &HttpRequest);
        GetBaseUrl(GameService.Object, &BaseUrl);
        ToString(&PlayerState->UniqueID, &AccountId);

        auto HttpRequestVTable = *(void***)HttpRequest.Object;
        auto SetVerb = (void (*)(void*, const FString&))HttpRequestVTable[0xC];
        auto SetURL = (void (*)(void*, const FString&))HttpRequestVTable[0xD];
        auto SetContentAsString = (void (*)(void*, const FString&))HttpRequestVTable[0x12];
        auto SetHeader = (void (*)(void*, const FString&, const FString&))HttpRequestVTable[0x16];
        auto URL = FString((BaseUrl.ToWString() + L"/api/game/v2/voice/" + AccountId.ToWString() + L"/dedicated_server/createJoinToken").c_str());
        auto Content = FString((L"{\"channelName\":\"" + VoiceChatChannel.ToWString() + L"\"}").c_str());
        auto HeaderName = FString(L"Content-Type");
        auto HeaderValue = FString(L"application/json");
        auto Verb = FString(L"POST");

        SetURL(HttpRequest.Object, URL);
        SetContentAsString(HttpRequest.Object, Content);
        SetHeader(HttpRequest.Object, HeaderName, HeaderValue);
        SetVerb(HttpRequest.Object, Verb);
        ProcessRequest(Subsystem, GameService.Object, UserId.Object, &HttpRequest);

        VoiceChatTokenRequests.push_back({ PlayerController, RequestID, FString(VoiceChatChannel.CStr()), HttpRequest });

        Verb.Free();
        HeaderValue.Free();
        HeaderName.Free();
        Content.Free();
        URL.Free();
        AccountId.Free();
        BaseUrl.Free();
    }
    else
    {
        auto Error = FString(L"disabled");

        PlayerController->ClientVoiceChatSendJoinToken(RequestID, false, Error, FString(), ChannelName);
        Error.Free();
    }

    TSharedRef_Destructor(&UserId);
    TSharedRef_Destructor(&Identity);
    TSharedRef_Destructor(&GameService);
}

static void OnHttpRequestJoinTokenComplete(FVoiceChatTokenRequest& Request, bool bSucceeded)
{
    static auto TJsonReaderFactory_Create = (FSharedRef* (*)(FSharedRef*, const FString*))(ImageBase + 0x2A11D54);
    static auto FJsonSerializer_Deserialize = (bool (*)(void*, FSharedRef*, int32))(ImageBase + 0x1D916A4);
    static auto TryGetStringField = (bool (*)(void*, const FStringView*, FString*))(ImageBase + 0x1D8F394);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    alignas(16) static const FStringView TokenField{ L"token", 5 };
    alignas(16) static const FStringView ErrorCodeField{ L"errorCode", 9 };

    FString Token;
    FString Error;
    FSharedRef Response{};

    if (bSucceeded)
    {
        auto GetResponse = (FSharedRef* (*)(void*, FSharedRef*))(*(void***)Request.HttpRequest.Object)[0x24];

        GetResponse(Request.HttpRequest.Object, &Response);
    }

    if (Response.Object)
    {
        auto GetContentAsString = (FString* (*)(void*, FString*))(*(void***)Response.Object)[0xC];
        FString Content;
        FSharedRef JsonReader{};
        FSharedRef JsonObject{};

        GetContentAsString(Response.Object, &Content);
        TJsonReaderFactory_Create(&JsonReader, &Content);

        if (FJsonSerializer_Deserialize(JsonReader.Object, &JsonObject, 0) && JsonObject.Object && !TryGetStringField(JsonObject.Object, &TokenField, &Token))
            TryGetStringField(JsonObject.Object, &ErrorCodeField, &Error);

        TSharedRef_Destructor(&JsonObject);
        TSharedRef_Destructor(&JsonReader);
        Content.Free();
    }

    auto bSuccess = Token.Num() > 1;

    if (!bSuccess && Error.Num() <= 1)
    {
        Error.Free();
        Error = FString(L"disabled");
    }

    if (auto PlayerController = Request.PlayerController.Get())
        PlayerController->ClientVoiceChatSendJoinToken(Request.RequestID, bSuccess, Error, Token, Request.ChannelName);

    Token.Free();
    Error.Free();
    Request.ChannelName.Free();
    TSharedRef_Destructor(&Response);
    TSharedRef_Destructor(&Request.HttpRequest);
}

static void (*HandleStartingNewPlayerOG)(UObject*, FFrame&, void*);
static void HandleStartingNewPlayer(UObject* Context, FFrame& Stack, void* Ret)
{
    APlayerController* NewPlayer;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.IncrementCode();
    auto GameMode = (AFortGameModeAthena*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    auto PlayerController = (AFortPlayerControllerAthena*)NewPlayer;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    PlayerState->SquadID = PlayerState->TeamIndex - 2;
    PlayerState->OnRep_SquadId();

    FGameMemberInfo Member{};

    Member.MostRecentArrayReplicationKey = -1;
    Member.ReplicationID = -1;
    Member.ReplicationKey = -1;
    Member.TeamIndex = PlayerState->TeamIndex;
    Member.SquadID = PlayerState->SquadID;
    Member.MemberUniqueId = PlayerState->UniqueID;

    auto& NewMember = GameState->GameMemberInfoArray.Members.Add(Member);
    GameState->GameMemberInfoArray.MarkItemDirty(NewMember);

    static auto NotifyGameMemberAdded = (void (*)(AFortGameStateAthena*, uint8, uint8, FUniqueNetIdRepl*))(ImageBase + 0x24AB834);
    NotifyGameMemberAdded(GameState, Member.SquadID, Member.TeamIndex, &Member.MemberUniqueId);

    if (!PlayerController->WorldInventory.ObjectPointer)
    {
        auto WorldInventory = SpawnActor<AFortInventory>(PlayerController->WorldInventoryClass.Get(), FVector{}, FRotator{}, PlayerController);

        PlayerController->WorldInventory.ObjectPointer = WorldInventory;
        PlayerController->WorldInventory.InterfacePointer = WorldInventory->GetInterface(IFortInventoryInterface::StaticClass());
        WorldInventory->InventoryType = EFortInventoryType::World;
    }

    PlayerState->WorldPlayerId = 0;

    callOG(GameMode, Stack.CurrentNativeFunction, HandleStartingNewPlayer, NewPlayer);

    for (auto& OtherPlayerState : GameState->PlayerArray)
    {
        auto SquadPlayerState = (AFortPlayerStateAthena*)OtherPlayerState;

        if (SquadPlayerState && SquadPlayerState->IsA(AFortPlayerStateAthena::StaticClass()) &&SquadPlayerState->SquadID == PlayerState->SquadID && SquadPlayerState->Owner && SquadPlayerState->Owner->IsA(AFortPlayerControllerZone::StaticClass()))
            SendVoiceChatChannelJoin(GameMode, (AFortPlayerControllerZone*)SquadPlayerState->Owner);
    }
}

static UClass** GetGameSessionClass(AFortGameModeAthena* GameMode, UClass** Result)
{
    *Result = AFortGameSessionDedicatedAthena::StaticClass();
    return Result;
}

static void (*ResolvePlayerEligibilityOG)(void*, void*, void*);
static void ResolvePlayerEligibility(void* Resolver, void* PlayerId, void* OnResolved)
{
    static auto CompleteEligibility = (void (*)(void*, void*, bool))(ImageBase + 0x520DA78);

    if (!Resolver)
        return CompleteEligibility(OnResolved, PlayerId, true);

    ResolvePlayerEligibilityOG(Resolver, PlayerId, OnResolved);
}

static void (*HandleMatchHasStartedOG)(AFortGameModeAthena*);
static void HandleMatchHasStarted(AFortGameModeAthena* GameMode)
{
    HandleMatchHasStartedOG(GameMode);
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameMode);

    if (!bSkipWarmup)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

        GamePhaseLogic->WarmupCountdownStartTime = Time;
        GamePhaseLogic->WarmupCountdownEndTime = Time + GamePhaseLogic->WarmupCountdownDuration;

        SetGamePhase(GamePhaseLogic, EAthenaGamePhase::Warmup);
        SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::Warmup);
    }
    else
        StartAircraftPhase(GamePhaseLogic);
}

static void (*FinishWorldInitializationOG)(AFortGameModeAthena*, AActor*);
static void FinishWorldInitialization(AFortGameModeAthena* GameMode, AActor* WorldManager)
{
    FinishWorldInitializationOG(GameMode, WorldManager);

    auto AddToTierData = [&](UDataTable* Table, TArray<FFortLootTierData*>& TempArr)
    {
        if (!Table)
            return;

        Table->AddToRoot();

        for (auto& Pair : Table->RowMap)
            TempArr.Add((FFortLootTierData*)Pair.Value());
    };

    auto AddToPackages = [&](UDataTable* Table, std::unordered_map<int32, FFortLootPackageData*>& TempArr)
    {
        if (!Table)
            return;

        Table->AddToRoot();

        for (auto& Pair : Table->RowMap)
            TempArr[Pair.Key().ComparisonIndex] = (FFortLootPackageData*)Pair.Value();
    };

    auto Playlist = FindObject<UFortPlaylistAthena>(::Playlist.c_str());

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/BRPlaylists/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    TArray<FFortLootTierData*> LootTierDataTempArr;
    auto LootTierData = Playlist ? Playlist->LootTierData.Get() : nullptr;
    if (!LootTierData)
        LootTierData = FindObject<UDataTable>(L"/Game/Items/Datatables/AthenaLootTierData_Client.AthenaLootTierData_Client");
    if (LootTierData)
        AddToTierData(LootTierData, LootTierDataTempArr);

    for (auto& Val : LootTierDataTempArr)
        TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);

    std::unordered_map<int32, FFortLootPackageData*> LootPackageTempArr;
    auto LootPackages = Playlist ? Playlist->LootPackages.Get() : nullptr;
    if (!LootPackages)
        LootPackages = FindObject<UDataTable>(L"/Game/Items/Datatables/AthenaLootPackages_Client.AthenaLootPackages_Client");
    if (LootPackages)
        AddToPackages(LootPackages, LootPackageTempArr);

    for (auto& [_, Val] : LootPackageTempArr)
        LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);

    bool bUseGameModeGameFeatureLoot = GameMode->GameFeatureLootTierData.Num() > 0;

    if (bUseGameModeGameFeatureLoot)
    {
        for (auto& FeatureLootTierData : GameMode->GameFeatureLootTierData)
        {
            TArray<FFortLootTierData*> LTDTempData;

            AddToTierData(FeatureLootTierData.Get(), LTDTempData);

            for (auto& Val : LTDTempData)
                TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);
        }

        for (auto& FeatureLootPackageData : GameMode->GameFeatureLootPackageData)
        {
            std::unordered_map<int32, FFortLootPackageData*> LPTempData;

            AddToPackages(FeatureLootPackageData.Get(), LPTempData);

            for (auto& [_, Val] : LPTempData)
                LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);
        }
    }

    auto GameFeaturesSubsystem = !bUseGameModeGameFeatureLoot ? (UGameFeaturesSubsystem*)USubsystemBlueprintLibrary::GetEngineSubsystem(UGameFeaturesSubsystem::StaticClass()) : nullptr;

    if (GameFeaturesSubsystem)
    {
        static auto GetGameFeatureDataForActivePlugin = (UGameFeatureData* (*)(UGameFeaturePluginStateMachine*))(ImageBase + 0x29EB49C);

        for (auto& Pair : GameFeaturesSubsystem->GameFeaturePluginStateMachines)
        {
            auto StateMachine = Pair.Value();
            auto GameFeatureData = StateMachine ? (UFortGameFeatureData*)GetGameFeatureDataForActivePlugin(StateMachine) : nullptr;

            if (!GameFeatureData || !GameFeatureData->IsA(UFortGameFeatureData::StaticClass()))
                continue;

            auto LootTableData = &GameFeatureData->DefaultLootTableData;

            if (Playlist)
            {
                for (auto& OverridePair : GameFeatureData->PlaylistOverrideLootTableData)
                {
                    if (Playlist->GameplayTagContainer.HasTag(OverridePair.Key()))
                    {
                        LootTableData = &OverridePair.Value();
                        break;
                    }
                }
            }

            auto FeatureLootTierData = LootTableData->LootTierData.Get();
            auto FeatureLootPackageData = LootTableData->LootPackageData.Get();

            TArray<FFortLootTierData*> LTDTempData;
            AddToTierData(FeatureLootTierData, LTDTempData);

            for (auto& Val : LTDTempData)
                TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);

            std::unordered_map<int32, FFortLootPackageData*> LPTempData;
            AddToPackages(FeatureLootPackageData, LPTempData);

            for (auto& [_, Val] : LPTempData)
                LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);
        }
    }
    else if (!bUseGameModeGameFeatureLoot)
    {
        for (int i = 0; i < UObject::GObjects->Num(); i++)
        {
            auto Object = UObject::GObjects->GetByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA(UFortGameFeatureData::StaticClass()))
                continue;

            auto& LootTableData = ((UFortGameFeatureData*)Object)->DefaultLootTableData;

            if (auto LTDFeatureData = LootTableData.LootTierData.Get())
            {
                TArray<FFortLootTierData*> LTDTempData;

                AddToTierData(LTDFeatureData, LTDTempData);

                for (auto& Val : LTDTempData)
                    TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);
            }

            if (auto LootPackageData = LootTableData.LootPackageData.Get())
            {
                std::unordered_map<int32, FFortLootPackageData*> LPTempData;

                AddToPackages(LootPackageData, LPTempData);

                for (auto& [_, Val] : LPTempData)
                    LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);
            }
        }
    }

    *(bool*)(__int64(&GameMode->OnPlaylistLootTablesAppliedDelegate) + 0x10) = true;
    GameMode->OnPlaylistLootTablesAppliedDelegate.Process();

    SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_Warmup.Tiered_Athena_FloorLoot_Warmup_C"));
    SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_01.Tiered_Athena_FloorLoot_01_C"));

    TArray<ABGAConsumableSpawner*> ConsumableSpawners{};
    GetAll<ABGAConsumableSpawner>(ConsumableSpawners);

    for (auto& Spawner : ConsumableSpawners)
        WorldReady(Spawner);

    ConsumableSpawners.Free();

    TArray<ABuildingItemCollectorActor*> Collectors{};
    GetAll<ABuildingItemCollectorActor>(Collectors);

    for (auto& CollectorActor : Collectors)
    {
        if (Sum <= Weight)
        {
            CollectorActor->K2_DestroyActor();
            continue;
        }

        int Rarity = 0;
        bool bFilled = false;

        while (!bFilled)
        {
            bool bFound = false;

            while (!bFound)
            {
                auto RandomNum = (float)rand() / (RAND_MAX / TotalWeight);

                for (auto& [ElementRarity, ElementWeight] : WeightMap)
                {
                    if (ElementWeight == 0)
                        continue;

                    if (RandomNum <= ElementWeight)
                    {
                        Rarity = ElementRarity;
                        bFound = true;
                        break;
                    }

                    RandomNum -= ElementWeight;
                }
            }

            if (Rarity == 0)
                break;

            bFilled = true;
            int AttemptsToGetItem = 0;

            for (int i = 0; i < CollectorActor->ItemCollections.Num(); i++)
            {
                if (AttemptsToGetItem > 10)
                {
                    bFilled = false;
                    break;
                }

                auto& Collection = CollectorActor->ItemCollections[i];

                if (Collection.bUseDefinedOutputItem)
                    continue;

                TArray<FFortItemEntry*> LootDrops{};

                ChooseLootForContainer(LootDrops, CollectorActor->DefaultItemLootTierGroupName, Rarity);

                if (Collection.OutputItemEntry.Num() > 0)
                {
                    Collection.OutputItemEntry.Clear();
                    Collection.OutputItem = nullptr;
                }

                for (auto& LootDrop : LootDrops)
                {
                    if (!Collection.OutputItem && IsPrimaryQuickbar((UFortItemDefinition*)LootDrop->ItemDefinition))
                        Collection.OutputItem = (UFortWorldItemDefinition*)LootDrop->ItemDefinition;

                    Collection.OutputItemEntry.Add(*LootDrop);
                    free(LootDrop);
                }

                if (!Collection.OutputItem)
                {
                    i--;
                    AttemptsToGetItem++;

                    continue;
                }

                AttemptsToGetItem = 0;
            }
        }

        if (Rarity == 0)
        {
            CollectorActor->K2_DestroyActor();
            continue;
        }

        CollectorActor->StartingGoalLevel = Rarity;
    }
    Collectors.Free();

    ExecHook(FindObject<UFunction>(L"/Game/Athena/Items/Gameplay/VendingMachine/B_Athena_VendingMachine.B_Athena_VendingMachine_C:VendWobble__FinishedFunc"), VendWobble__FinishedFunc, VendWobble__FinishedFuncOG);
}

static void GetPhaseInfo(UObject* Context, FFrame& Stack, bool* Ret)
{
    FFortSafeZonePhaseInfo OutSafeZonePhaseTemp{};
    auto& OutSafeZonePhase = Stack.StepCompiledInRef(&OutSafeZonePhaseTemp);
    int32 InPhaseToGet;
    Stack.StepCompiledIn(&InPhaseToGet);
    Stack.IncrementCode();

    auto SafeZoneIndicator = (AFortSafeZoneIndicator*)Context;
    auto& Array = SafeZoneIndicator->SafeZonePhases;

    if (Array.IsValidIndex(InPhaseToGet))
    {
        memcpy((PBYTE)&OutSafeZonePhase, (PBYTE)&Array[InPhaseToGet], sizeof(FFortSafeZonePhaseInfo));

        *Ret = true;
        return;
    }

    *Ret = false;
}

static void OnGamePhaseStepChanged(UObject* Context, FFrame& Stack)
{
    FFortGamePhaseStepUpdatedEvent Event{};
    Stack.StepCompiledIn(&Event);
    Stack.IncrementCode();

    auto Mutator = (AFortAthenaMutator_GiveItemsAtGamePhaseStep*)Context;

    if (Event.GamePhaseStep == Mutator->PhaseToGiveItems)
        for (auto& PlayerController : Mutator->CachedGameMode->AlivePlayers)
        {
            for (auto& Item : Mutator->ItemsToGive)
                GiveItem((AFortInventory*)PlayerController->WorldInventory.ObjectPointer, Item.ItemToDrop, (int)Item.NumberToGive.Evaluate());
        }
}

void AFortGameMode__Init()
{
    Hook(ImageBase + 0x8F94F28, HandleMatchHasStarted, HandleMatchHasStartedOG);
    ExecHook(AGameMode::StaticClass()->FindFunction("ReadyToStartMatch"), ReadyToStartMatch, ReadyToStartMatchOG);
    Patch<uint16_t>(ImageBase + 0x30276D8, 0x9090);
    Hook(ImageBase + 0x8F91858, FinishWorldInitialization, FinishWorldInitializationOG);
    Hook(ImageBase + 0x982D114, SendVoiceChatToken);
    HookEvery<AFortGameModeZone>(uint32(0x1B8), SendVoiceChatChannelJoin);

    for (int i = 0; i < 6; i++)
        Patch<uint8_t>(ImageBase + 0x283EE5D + i, 0x90);
}

void AFortGameMode__PostLoad()
{
    ExecHook(AGameModeBase::StaticClass()->FindFunction("SpawnDefaultPawnFor"), SpawnDefaultPawnFor);
    Patch<uint16_t>(ImageBase + 0x6D57A3C, 0x9090);
    ExecHook(AGameModeBase::StaticClass()->FindFunction("HandleStartingNewPlayer"), HandleStartingNewPlayer, HandleStartingNewPlayerOG);
    Patch<uint16_t>(ImageBase + 0x28730CC, 0x9090);
    Hook(ImageBase + 0x8F928A4, GetGameSessionClass);
    Hook(ImageBase + 0x9924AFC, ResolvePlayerEligibility, ResolvePlayerEligibilityOG);

    ExecHook(AFortSafeZoneIndicator::StaticClass()->FindFunction("GetPhaseInfo"), GetPhaseInfo);

    ExecHook(AFortAthenaMutator_GiveItemsAtGamePhaseStep::StaticClass()->FindFunction("OnGamePhaseStepChanged"), OnGamePhaseStepChanged);
}

void AFortGameMode__Tick()
{
    for (auto It = VoiceChatTokenRequests.begin(); It != VoiceChatTokenRequests.end();)
    {
        auto GetStatus = (EHttpRequestStatus (*)(void*))(*(void***)It->HttpRequest.Object)[0x2];
        auto Status = GetStatus(It->HttpRequest.Object);

        if (Status == EHttpRequestStatus::NotStarted || Status == EHttpRequestStatus::Processing)
        {
            It++;
            continue;
        }

        OnHttpRequestJoinTokenComplete(*It, Status == EHttpRequestStatus::Succeeded);
        It = VoiceChatTokenRequests.erase(It);
    }

    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

    if (!GamePhaseLogic)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

    static bool bFinishedFlight = false;
    if (!bSkipAircraft)
    {
        if (GamePhaseLogic->GamePhase <= EAthenaGamePhase::Warmup)
        {
            static bool bGettingReady = false;
            if (!bGettingReady && GamePhaseLogic->GamePhase == EAthenaGamePhase::Warmup)
            {
                if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->WarmupCountdownEndTime != -1 && GamePhaseLogic->WarmupCountdownEndTime - 10.f <= Time)
                {
                    bGettingReady = true;

                    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::GetReady);
                    return;
                }
            }

            if (bGettingReady)
            {
                if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->WarmupCountdownEndTime != -1 && GamePhaseLogic->WarmupCountdownEndTime < Time)
                {
                    StartAircraftPhase(GamePhaseLogic);

                    return;
                }
            }
        }

        if (GamePhaseLogic->GamePhase == EAthenaGamePhase::Aircraft)
        {
            static bool bBusUnlocked = false;
            if (!bBusUnlocked)
            {
                if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Data[0].Get() && GamePhaseLogic->Aircrafts_GameState.Data[0]->DropStartTime < Time)
                {
                    bBusUnlocked = true;

                    GamePhaseLogic->bAircraftIsLocked = false;
                    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::BusFlying);
                    return;
                }
            }

            static bool bStartedForming = false;
            if (!bStartedForming)
            {
                if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Data[0].Get() && GamePhaseLogic->Aircrafts_GameState.Data[0]->DropEndTime != -1 && GamePhaseLogic->Aircrafts_GameState.Data[0]->DropEndTime < Time)
                {
                    bStartedForming = true;

                    for (auto& Player : GameMode->AlivePlayers)
                    {
                        if (Player->IsInAircraft())
                            Player->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
                    }

                    GamePhaseLogic->SafeZonesStartTime = bLateGame ? (float)Time : (float)Time + 60.f;

                    SetGamePhase(GamePhaseLogic, EAthenaGamePhase::SafeZones);
                    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormForming);
                    return;
                }
            }
        }

        if (!bFinishedFlight)
        {
            if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Num() > 0 && GamePhaseLogic->Aircrafts_GameState.Data[0].Get() && GamePhaseLogic->Aircrafts_GameState.Data[0]->FlightEndTime != -1 && GamePhaseLogic->Aircrafts_GameState.Data[0]->FlightEndTime < Time)
            {
                bFinishedFlight = true;
                auto Aircraft = GamePhaseLogic->Aircrafts_GameState.Data[0].Get();

                Aircraft->K2_DestroyActor();
                GamePhaseLogic->Aircrafts_GameState.Clear();
                GamePhaseLogic->Aircrafts_GameMode.Clear();
                return;
            }
        }
    }
    else
    {
        if (!bFinishedFlight)
        {
            bFinishedFlight = true;

            SetGamePhase(GamePhaseLogic, EAthenaGamePhase::SafeZones);
            SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormForming);
        }
    }

    if (bEnableZones)
    {
        if (GamePhaseLogic->GamePhase == EAthenaGamePhase::SafeZones)
        {
            static bool bFormedZone = false;
            if (bFinishedFlight && !bFormedZone)
            {
                if (GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->SafeZonesStartTime != -1 && GamePhaseLogic->SafeZonesStartTime < Time)
                {
                    bFormedZone = true;
                    if (!GamePhaseLogic->SafeZoneIndicator)
                    {
                        auto SafeZoneIndicator = SpawnActor<AFortSafeZoneIndicator>(GamePhaseLogic->SafeZoneIndicatorClass.Get(), FVector{});

                        if (SafeZoneIndicator)
                        {
                            auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
                            auto& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
                            float SafeZoneCount = SafeZoneDefinition.Count.Evaluate();

                            auto& Array = SafeZoneIndicator->SafeZonePhases;

                            if (Array.IsValid())
                                Array.Free();

                            const float Time = (float)UGameplayStatics::GetTimeSeconds(GameState);

                            for (float i = 0; i < SafeZoneCount; i++)
                            {
                                FFortSafeZonePhaseInfo PhaseInfo{};

                                PhaseInfo.Radius = SafeZoneDefinition.Radius.Evaluate(i);
                                PhaseInfo.WaitTime = SafeZoneDefinition.WaitTime.Evaluate(i);
                                PhaseInfo.ShrinkTime = SafeZoneDefinition.ShrinkTime.Evaluate(i);
                                PhaseInfo.PlayerCap = (int)SafeZoneDefinition.PlayerCapSolo.Evaluate(i);

                                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->AthenaGameDataTable, FName(L"Default.SafeZone.Damage"), i, nullptr, &PhaseInfo.DamageInfo.Damage, FString());
                                if (i == 0.f)
                                    PhaseInfo.DamageInfo.Damage = 0.01f;
                                PhaseInfo.DamageInfo.bPercentageBasedDamage = true;
                                PhaseInfo.TimeBetweenStormCapDamage = GamePhaseLogic->TimeBetweenStormCapDamage.Evaluate(i);
                                PhaseInfo.StormCapDamagePerTick = GamePhaseLogic->StormCapDamagePerTick.Evaluate(i);
                                PhaseInfo.StormCampingIncrementTimeAfterDelay = GamePhaseLogic->StormCampingIncrementTimeAfterDelay.Evaluate(i);
                                PhaseInfo.StormCampingInitialDelayTime = GamePhaseLogic->StormCampingInitialDelayTime.Evaluate(i);
                                PhaseInfo.MegaStormGridCellThickness = (int)SafeZoneDefinition.MegaStormGridCellThickness.Evaluate(i);
                                PhaseInfo.UsePOIStormCenter = false;

                                PhaseInfo.Center = SafeZoneLocations.Data[(int)i];

                                Array.Add(PhaseInfo);

                                SafeZoneIndicator->PhaseCount++;
                            }

                            SafeZoneIndicator->OnRep_PhaseCount();

                            SafeZoneIndicator->SafeZoneStartShrinkTime = Time + Array.Data[0].WaitTime;
                            SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + Array.Data[0].ShrinkTime;

                            SafeZoneIndicator->CurrentPhase = 0;
                            SafeZoneIndicator->OnRep_CurrentPhase();
                        }

                        GamePhaseLogic->SafeZoneIndicator = SafeZoneIndicator;
                        GamePhaseLogic->OnRep_SafeZoneIndicator();
                    }

                    StartNewSafeZonePhase(GamePhaseLogic, bLateGame ? GetLateGamePhase((AFortGameStateAthena*)UWorld::GetWorld()->GameState) : 1);
                    return;
                }
            }

            static bool bUpdatedPhase = false;
            auto SafeZoneIndicator = GamePhaseLogic->SafeZoneIndicator;
            if (bFormedZone && SafeZoneIndicator)
            {
                if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase))
                {
                    if (!bUpdatedPhase && SafeZoneIndicator->SafeZoneStartShrinkTime < Time)
                    {
                        bUpdatedPhase = true;

                        auto& SafeZoneState = *(uint8_t*)(__int64(SafeZoneIndicator) + 0x60C);
                        SafeZoneState = 3;

                        SafeZoneIndicator->OnSafeZoneStateChange(EFortSafeZoneState::Shrinking, false);
                        SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, EFortSafeZoneState::Shrinking);

                        SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormShrinking);
                    }
                    else if (SafeZoneIndicator->SafeZoneFinishShrinkTime < Time)
                    {
                        if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase + 1))
                        {
                            StartNewSafeZonePhase(GamePhaseLogic, SafeZoneIndicator->CurrentPhase + 1);
                            bUpdatedPhase = false;
                        }
                    }
                }

                for (auto& Player : GameMode->AlivePlayers)
                {
                    if (auto Pawn = Player->MyFortPawn)
                    {
                        bool bInZone = GamePhaseLogic->IsInCurrentSafeZone(Player->MyFortPawn->K2_GetActorLocation(), false);

                        if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
                        {
                            Pawn->bIsInAnyStorm = !bInZone;
                            Pawn->OnRep_IsInAnyStorm();
                            Pawn->bIsInsideSafeZone = bInZone;
                            Pawn->OnRep_IsInsideSafeZone();
                        }
                    }
                }

                TArray<AFortPlayerPawnAthena*> Pawns;
                GetAll<AFortPlayerPawnAthena>(Pawns);

                for (auto& Pawn : Pawns)
                {
                    if (!Pawn || Pawn->bActorIsBeingDestroyed || !Pawn->Controller || Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
                        continue;

                    bool bInZone = GamePhaseLogic->IsInCurrentSafeZone(Pawn->K2_GetActorLocation(), false);

                    if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
                    {
                        Pawn->bIsInAnyStorm = !bInZone;
                        Pawn->OnRep_IsInAnyStorm();
                        Pawn->bIsInsideSafeZone = bInZone;
                        Pawn->OnRep_IsInsideSafeZone();
                    }

                    auto AbilitySystemComponent = Pawn->AbilitySystemComponent;

                    if (AbilitySystemComponent && !Pawn->SafeZoneAppliedGE)
                    {
                        if (auto Effect = ApplyOutsideSafeZoneEffect(AbilitySystemComponent))
                            Pawn->SafeZoneAppliedGE = (UClass*)Effect;
                    }
                }

                Pawns.Free();
            }
        }
    }
}
