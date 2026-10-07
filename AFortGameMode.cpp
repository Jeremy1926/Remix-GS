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
#include "UAthenaAIServicePlayerBots.h"
#include "UAthenaNavSystem.h"
#include "UFortControllerComponent_TournamentStatsTracker.h"

INIT_MODULE(AFortGameMode);
INIT_POSTLOAD(AFortGameMode);
INIT_TICKER(AFortGameMode);
INIT_RESET(AFortGameMode);

static TArray<UFortAbilitySet*> AbilitySets;
static bool bSkipWarmup = false;
static bool bSkipAircraft = false;
static uint8 PauseWarmupMask = 0;

static TArray<FVector> SafeZoneLocations;
static TArray<FName> LevelsToStream;

static std::wstring ConfiguredPlaylist;
static bool bConfiguredLateGame = false;

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

struct FOnlineSessionSettings
{
    uint8 Pad_0[0x8];
    int32 NumPublicConnections;
    int32 NumPrivateConnections;
    bool bShouldAdvertise;
    bool bAllowJoinInProgress;
    bool bIsLANMatch;
    bool bIsDedicated;
    bool bUsesStats;
    bool bAllowInvites;
    uint8 Pad_16[0xC2];
};

struct FMatchAssignment
{
    bool bResponded;
    FString MatchId;
    FString BucketId;
    FName PlaylistName;
    FString RegionId;
    FString GroupId;
    TArray<TArray<TArray<FSharedRef>>> Teams;
    TArray<FSharedRef> Spectators;
    FSharedRef MatchOptions;
    FString PayloadJwt;
};

static uint64 CreateSessionCompleteHandle = 0;
static ULONGLONG MatchmakingWaitStart = 0;
static uint64 LeaveMeshNetworkCompleteHandle = 0;
static int32 ShutdownExitCode = 0;

static FName GetGameSessionName()
{
    static auto GetHardcodedNames = (uint8* (*)())(ImageBase + 0x1723804);
    static auto DecodeName = (uint32* (*)(const uint8*, uint32*))(ImageBase + 0x50D6B70);
    static auto EncodeName = (FName* (*)(FName*, uint32))(ImageBase + 0x4A41EC0);

    uint32 NameIndex;
    FName Name;

    DecodeName(GetHardcodedNames() + 0x144BC, &NameIndex);
    EncodeName(&Name, NameIndex);
    return Name;
}

static FJsonKey JsonKey(const wchar_t* Key)
{
    return { Key, int32(wcslen(Key)) };
}

static FJsonKey JsonKey(const FString& String)
{
    return { String.Data, String.NumElements ? String.NumElements - 1 : 0 };
}

static void IncrementSharedReferenceCount(const FSharedRef& Ref)
{
    if (Ref.SharedReferenceCount)
        _InterlockedIncrement((volatile long*)((uint8*)Ref.SharedReferenceCount + 0x8));
}

static void AddSharedRef(TArray<FSharedRef>& Array, const FSharedRef& Ref)
{
    Array.Add(Ref);
    IncrementSharedReferenceCount(Ref);
}

static FSharedRef GetSessionInterface()
{
    static auto GetSessionInterfaceInternal = (FSharedRef* (*)(FSharedRef*, UWorld*, FName))(ImageBase + 0x71D8BA0);

    FSharedRef SessionInterface{};
    GetSessionInterfaceInternal(&SessionInterface, UWorld::GetWorld(), FName());
    return SessionInterface;
}

static FOnlineSessionSettings* GetSessionSettings(const FSharedRef& SessionInterface, FName SessionName)
{
    auto GetSessionSettingsInternal = (FOnlineSessionSettings* (*)(void*, FName))(*(void***)SessionInterface.Object)[0x70];

    return GetSessionSettingsInternal(SessionInterface.Object, SessionName);
}

static void ClearTimer(AActor* Actor, uint64& Handle)
{
    static auto GetWorldTimerManager = (void* (*)(AActor*))(ImageBase + 0x15C52AC);
    static auto FindTimer = (void* (*)(void*, uint64*))(ImageBase + 0x15C33A8);
    static auto InternalClearTimer = (void (*)(void*, uint64))(ImageBase + 0x15C4BA4);

    auto TimerManager = GetWorldTimerManager(Actor);

    if (FindTimer(TimerManager, &Handle))
        InternalClearTimer(TimerManager, Handle);

    Handle = 0;
}

void SetActorTimer(AActor* Actor, void* Callback, uint64& Handle, float Delay)
{
    static auto BindTimerDelegate = (void (*)(FNativeDelegate*, UObject*, void*))(ImageBase + 0x9816384);
    static auto GetWorldTimerManager = (void* (*)(AActor*))(ImageBase + 0x15C52AC);
    static auto MakeUnifiedTimerDelegate = (void* (*)(uint8*, FNativeDelegate*))(ImageBase + 0x15C4A14);
    static auto InternalSetTimer = (void (*)(void*, uint64*, uint8*, float, const void*))(ImageBase + 0x1E0C500);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);

    FNativeDelegate TimerDelegate{};
    uint8 UnifiedTimerDelegate[0x48];
    struct { uint8 bLoop; uint8 bMaxOncePerFrame; float FirstDelay; } TimerParameters{ 0, 0, -1.f };

    BindTimerDelegate(&TimerDelegate, Actor, Callback);
    MakeUnifiedTimerDelegate(UnifiedTimerDelegate, &TimerDelegate);
    InternalSetTimer(GetWorldTimerManager(Actor), &Handle, UnifiedTimerDelegate, Delay, &TimerParameters);
    DestroyDelegate((FNativeDelegate*)UnifiedTimerDelegate);
    DestroyDelegate(&TimerDelegate);
}

static bool HasJsonField(uint8* Object, const FJsonKey& Key)
{
    static auto HashKey = (uint32 (*)(const FJsonKey*))(ImageBase + 0x180B5B8);
    static auto KeyEquals = (bool (*)(const FString*, const FJsonKey*))(ImageBase + 0x1C9D40C);

    if (*(int32*)(Object + 0x8) == *(int32*)(Object + 0x34))
        return false;

    auto Buckets = *(int32**)(Object + 0x40) ? *(int32**)(Object + 0x40) : (int32*)(Object + 0x38);

    for (auto Index = Buckets[HashKey(&Key) & (*(int32*)(Object + 0x48) - 1)]; Index != -1; Index = *(int32*)(*(uint8**)Object + 0x28 * Index + 0x20))
    {
        auto Element = *(uint8**)Object + 0x28 * Index;

        if (KeyEquals((FString*)Element, &Key))
            return *(void**)(Element + 0x10) != nullptr;
    }

    return false;
}

static void OnCreateSessionComplete(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, bool bWasSuccessful)
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);

    auto SessionInterface = GetSessionInterface();

    if (SessionInterface.Object)
    {
        auto ClearOnCreateSessionCompleteDelegate = (void (*)(void*, uint64*))(*(void***)SessionInterface.Object)[0xB];

        ClearOnCreateSessionCompleteDelegate(SessionInterface.Object, &CreateSessionCompleteHandle);
    }

    TSharedRef_Destructor(&SessionInterface);

    if (SessionName == GetGameSessionName() && bWasSuccessful && !*(uint8*)(__int64(GameSession) + 0x430))
        MatchmakingWaitStart = GetTickCount64();
}

static bool HandleMatchAssignmentV2(AFortPartyBeaconHost* ReservationBeaconHost, FMatchAssignment& MatchAssignment)
{
    static auto GetAthenaPlaylist = (UFortPlaylistAthena* (*)(UFortPlaylistManager*, FName))(ImageBase + 0x9947A68);
    static auto GetMaxNumberOfTeams = (int32 (*)(UObject*, UFortPlaylistAthena*))(ImageBase + 0x9086DCC);
    static auto GetMaxPlayerCount = (int32 (*)(UObject*, UFortPlaylistAthena*))(ImageBase + 0x9086E3C);
    static auto GetMaxTeamSize = (int32 (*)(UObject*, UFortPlaylistAthena*))(ImageBase + 0x9086ED0);
    static auto AssignString = (void (*)(FString*, const wchar_t*, int32))(ImageBase + 0x18104B4);
    static auto MakeUniqueNetIdRepl = (FUniqueNetIdRepl* (*)(FUniqueNetIdRepl*, const FSharedRef*))(ImageBase + 0x6D3A7E8);
    static auto AssignUniqueNetIdRepl = (void (*)(FUniqueNetIdRepl*, const FUniqueNetIdRepl*))(ImageBase + 0x1D91138);
    static auto DestroyUniqueNetIdRepl = (void (*)(FUniqueNetIdRepl*))(ImageBase + 0x175D9E8);
    static auto IsValidUniqueNetIdRepl = (bool (*)(const FUniqueNetIdRepl*))(ImageBase + 0x15F6428);
    static auto TryGetBoolField = (bool (*)(void*, const FJsonKey*, bool*))(ImageBase + 0x180A030);
    static auto GrowPlayerReservations = (void (*)(TArray<FPlayerReservation>*, int32))(ImageBase + 0x5C173E0);
    static auto CopyPlayerReservation = (FPlayerReservation* (*)(FPlayerReservation*, const FPlayerReservation*))(ImageBase + 0x71D2C78);
    static auto DestroyPartyReservation = (void (*)(FPartyReservation*))(ImageBase + 0x71D30A8);
    static auto ChangeSessionOwner = (void (*)(UFortPartyBeaconState*, const FSharedRef*))(ImageBase + 0x9E32CE8);

    auto GameInstance = (UFortGameInstance*)UWorld::GetWorld()->OwningGameInstance;

    if (!GameInstance || !GameInstance->IsA(UFortGameInstance::StaticClass()) || !GameInstance->PlaylistManager)
        return false;

    auto Playlist = GetAthenaPlaylist(GameInstance->PlaylistManager, MatchAssignment.PlaylistName);

    if (!Playlist)
        return false;

    auto MaxTeams = GetMaxNumberOfTeams(ReservationBeaconHost, Playlist);
    auto MaxTeamSize = GetMaxTeamSize(ReservationBeaconHost, Playlist);
    auto MaxPlayers = GetMaxPlayerCount(ReservationBeaconHost, Playlist);
    auto bAllowJoinInProgress = Playlist->bAllowJoinInProgress;
    auto ReconfigureTeamAndPlayerCount = (bool (*)(AFortPartyBeaconHost*, int32, int32, int32))ReservationBeaconHost->VTable[0xEC];

    ReconfigureTeamAndPlayerCount(ReservationBeaconHost, MaxTeams, MaxTeamSize, MaxPlayers);

    auto State = ReservationBeaconHost->State;
    auto FortState = *(UFortPartyBeaconState**)(__int64(ReservationBeaconHost) + 0x348);
    auto SetTeamAssignmentMethod = (void (*)(UFortPartyBeaconState*, FName))FortState->VTable[0x5C];

    SetTeamAssignmentMethod(FortState, *(FName*)(ImageBase + 0x12D186A4));

    auto& ReservationData = FortState->ReservationData;
    FUniqueNetIdRepl InvalidId{};
    *(void**)&InvalidId = (void*)(ImageBase + 0xD593168);

    ReservationData.PlaylistId = -1;
    AssignString(&ReservationData.ZoneInstanceId, L"", 0);
    AssignString(&ReservationData.WUID, L"", 0);
    AssignUniqueNetIdRepl(&ReservationData.WorldDataOwner, &InvalidId);
    ReservationData.bMakePrivate = false;
    ReservationData.MatchmakingPool = EFortMatchmakingPool(0);
    ReservationData.bUsesMatchmakingV2 = bAllowJoinInProgress;

    bool bCrossplayLocked = false;
    auto CrossplayLockedKey = JsonKey(L"match.option.crossplayLocked");
    TryGetBoolField(MatchAssignment.MatchOptions.Object, &CrossplayLockedKey, &bCrossplayLocked);
    *(bool*)(__int64(FortState) + 0x148) = bCrossplayLocked;

    auto DoesReservationFit = (bool (*)(UPartyBeaconState*, FPartyReservation*))State->VTable[0x6B];
    auto AddReservation = (bool (*)(UPartyBeaconState*, FPartyReservation*))State->VTable[0x5D];
    int32 NumTooManyPlayers = 0;
    int32 NumAddFailures = 0;

    for (int32 TeamIdx = 0; TeamIdx < MatchAssignment.Teams.Num(); TeamIdx++)
    {
        for (auto& Party : MatchAssignment.Teams[TeamIdx])
        {
            FPartyReservation Reservation{};
            Reservation.TeamNum = TeamIdx;
            Reservation.PartyLeader = InvalidId;

            for (auto& Player : Party)
            {
                FPlayerReservation PlayerReservation{};
                FUniqueNetIdRepl PlayerId;

                PlayerReservation.UniqueID = InvalidId;
                PlayerReservation.bAllowCrossplay = !bCrossplayLocked;
                AssignUniqueNetIdRepl(&PlayerReservation.UniqueID, MakeUniqueNetIdRepl(&PlayerId, &Player));
                DestroyUniqueNetIdRepl(&PlayerId);

                auto Index = Reservation.PartyMembers.NumElements++;

                if (Reservation.PartyMembers.NumElements > Reservation.PartyMembers.MaxElements)
                    GrowPlayerReservations(&Reservation.PartyMembers, Index);

                CopyPlayerReservation(&Reservation.PartyMembers.Data[Index], &PlayerReservation);
                DestroyUniqueNetIdRepl(&PlayerReservation.UniqueID);
            }

            if (Reservation.PartyMembers.Num() > 0)
            {
                AssignUniqueNetIdRepl(&Reservation.PartyLeader, &Reservation.PartyMembers.Data[0].UniqueID);

                if (bAllowJoinInProgress && !*(void**)(__int64(&FortState->GameSessionOwner) + 0x8) && IsValidUniqueNetIdRepl(&Reservation.PartyLeader))
                    ChangeSessionOwner(FortState, &Party.Data[0]);

                if (!DoesReservationFit(State, &Reservation))
                    NumTooManyPlayers++;
                else if (!AddReservation(State, &Reservation))
                    NumAddFailures++;
            }

            DestroyPartyReservation(&Reservation);
        }
    }

    auto GetNumConsumedReservations = (int32 (*)(UPartyBeaconState*))State->VTable[0x6A];

    if (NumTooManyPlayers || NumAddFailures || !GetNumConsumedReservations(State))
    {
        auto DumpReservations = (void (*)(AFortPartyBeaconHost*))ReservationBeaconHost->VTable[0x105];

        DumpReservations(ReservationBeaconHost);
        return false;
    }

    return true;
}

static void OnPlaylistAssetUpdateComplete(AFortGameSessionDedicatedAthena* GameSession, uint8 Result, void* PatchedAssetIds)
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto AssignString = (void (*)(FString*, const wchar_t*, int32))(ImageBase + 0x18104B4);
    static auto NameToString = (FString* (*)(const FName*, FString*))(ImageBase + 0x17222B8);
    static auto SetStringSetting = (void (*)(FOnlineSessionSettings*, FName, const FString*, int32))(ImageBase + 0x7183BAC);
    static auto SetBoolSetting = (void (*)(FOnlineSessionSettings*, FName, const bool*, int32))(ImageBase + 0x7183E78);
    static auto MakeOnUpdateSessionCompleteDelegate = (FNativeDelegate* (*)(FNativeDelegate*, AGameSession*))(ImageBase + 0x9CFA688);
    static auto MakeOnRegisterPlayersCompleteDelegate = (FNativeDelegate* (*)(FNativeDelegate*, AGameSession*, void*, const FString*))(ImageBase + 0x9CFA6CC);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);
    static auto UpdateSession = (void (*)(AGameSession*, FName, FOnlineSessionSettings*, FNativeDelegate*))(ImageBase + 0x9CF3A70);
    static auto AdvertiseSessionJoinability = (void (*)(AGameSession*))(ImageBase + 0x9CCD0FC);
    static auto TryGetStringField = (bool (*)(void*, const FJsonKey*, FString*))(ImageBase + 0x1D8F394);
    static auto HasTypedField = (bool (*)(void*, const FJsonKey*, int32))(ImageBase + 0x3BD6E48);
    static auto GetObjectField = (FSharedRef* (*)(void*, const FJsonKey*))(ImageBase + 0x2BECAF0);
    static auto TryGetObjectField = (bool (*)(void*, const FJsonKey*, FSharedRef**))(ImageBase + 0x5AE4FCC);
    static auto TryGetBoolField = (bool (*)(void*, const FJsonKey*, bool*))(ImageBase + 0x180A030);
    static auto ParseIntoArray = (int32 (*)(const FString*, TArray<FString>*, const wchar_t*, bool))(ImageBase + 0x1D85898);
    static auto DestroyStringArray = (void (*)(TArray<FString>*))(ImageBase + 0x17656C0);
    static auto MakeUniqueNetIdRepl = (FUniqueNetIdRepl* (*)(FUniqueNetIdRepl*, const FSharedRef*))(ImageBase + 0x6D3A7E8);
    static auto CopyUniqueNetIdRepl = (FUniqueNetIdRepl* (*)(FUniqueNetIdRepl*, const FUniqueNetIdRepl*))(ImageBase + 0x175D5A0);
    static auto DestroyUniqueNetIdRepl = (void (*)(FUniqueNetIdRepl*))(ImageBase + 0x175D9E8);
    static auto GrowUniqueNetIdRepls = (void (*)(TArray<FUniqueNetIdRepl>*, int32))(ImageBase + 0x15DD080);
    static auto EmplacePlayerToSquadId = (int32* (*)(void*, int32*, const void*))(ImageBase + 0x9CFAA4C);
    static auto FindSquadIdToPlayer = (int32 (*)(void*, uint32, const uint8*))(ImageBase + 0x1921EDC);
    static auto AddSquadIdToPlayer = (int32* (*)(void*, int32*, uint32, const uint8**))(ImageBase + 0x9CFD4C0);
    static auto FindInitialTeamAssignment = (int32 (*)(void*, uint32, const uint8*))(ImageBase + 0x264ACA0);
    static auto AddInitialTeamAssignment = (int32* (*)(void*, int32*, uint32, const uint8**))(ImageBase + 0x9CFD5F0);
    static auto EmplaceTeamMember = (int32* (*)(void*, int32*, const FUniqueNetIdRepl*))(ImageBase + 0x7FA437C);
    static auto EmplaceSquadFill = (int32* (*)(void*, int32*, const void*))(ImageBase + 0x8F84848);
    static auto FindTeamIndexToSquadId = (int32 (*)(void*, uint32, const int32*))(ImageBase + 0x151C4F8);
    static auto AddTeamIndexToSquadId = (int32* (*)(void*, int32*, uint32, const int32**))(ImageBase + 0x79C313C);
    static auto QueryUserInfoForMatchmaking = (void (*)(AGameSession*, const FString*, const TArray<FSharedRef>*))(ImageBase + 0x9D26D68);
    static auto EmplaceRegisterPlayerHandle = (int32* (*)(void*, int32*, const void*))(ImageBase + 0x7BDD844);
    static auto DestroySharedRefArray = (void (*)(TArray<FSharedRef>*))(ImageBase + 0x1F7A130);
    static auto DeclineMatchmaking = (void (*)(AFortGameSessionDedicatedAthena*, uint8, const FString*))(ImageBase + 0x9D15934);

    auto Base = __int64(GameSession);
    auto ReservationBeaconHost = GameSession->ReservationBeaconHost;
    FString NoBackfillId;

    if (!ReservationBeaconHost)
        return DeclineMatchmaking(GameSession, 7, &NoBackfillId);

    auto& MatchAssignment = *(FMatchAssignment*)(Base + 0x7D8);
    uint8 DeclineReason = 0;

    if (uint8(Result - 1) > 1)
        DeclineReason = 5;
    else if (!HandleMatchAssignmentV2(ReservationBeaconHost, MatchAssignment))
        DeclineReason = 8;
    else
    {
        auto MatchOptions = MatchAssignment.MatchOptions.Object;

        ClearTimer(GameSession, *(uint64*)(Base + 0x4A0));

        auto GetNumConsumedReservations = (int32 (*)(AFortPartyBeaconHost*))ReservationBeaconHost->VTable[0xF1];
        GameSession->MaxPlayers = GameSession->MaxSpectators + GetNumConsumedReservations(ReservationBeaconHost);

        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

        if (GameMode && GameMode->IsA(AFortGameMode::StaticClass()))
        {
            AssignString(&GameMode->CurrentBucketId, MatchAssignment.BucketId.Data, JsonKey(MatchAssignment.BucketId).Len);
            AssignString(&GameMode->MatchmakingRegionId, MatchAssignment.RegionId.Data, JsonKey(MatchAssignment.RegionId).Len);
        }
        else
            GameMode = nullptr;

        auto bMeshNode = GameSession->bEnableMeshNetwork && *(uint8*)(Base + 0x430) != 0;

        auto SessionInterface = GetSessionInterface();

        if (SessionInterface.Object)
        {
            auto GameSessionName = GetGameSessionName();

            if (auto SessionSettings = GetSessionSettings(SessionInterface, GameSessionName))
            {
                FString PlaylistName;
                bool bAllowBroadcasting = false;

                NameToString(&GameMode->CurrentPlaylistName, &PlaylistName);
                SetStringSetting(SessionSettings, FName(L"PLAYLISTNAME"), &PlaylistName, 2);
                SetBoolSetting(SessionSettings, FName(L"ALLOWBROADCASTING"), &bAllowBroadcasting, 2);
                PlaylistName.Free();

                SessionSettings->bShouldAdvertise = false;
                SessionSettings->bAllowJoinInProgress = bMeshNode;
                SessionSettings->bAllowInvites = bMeshNode;

                if (bMeshNode)
                    GameSession->BulkUnregisterTimerDelay = 0.f;

                FString TournamentId;
                FString WindowId;
                auto TournamentIdKey = JsonKey(L"match.option.events.tournamentid");
                auto WindowIdKey = JsonKey(L"match.option.events.windowid");

                TryGetStringField(MatchOptions, &TournamentIdKey, &TournamentId);
                TryGetStringField(MatchOptions, &WindowIdKey, &WindowId);

                if (TournamentId.Num() > 1 && WindowId.Num() > 1)
                {
                    SetStringSetting(SessionSettings, FName(L"EVENTID"), &TournamentId, 2);
                    SetStringSetting(SessionSettings, FName(L"EVENTWINDOWID"), &WindowId, 2);
                }

                TournamentId.Free();
                WindowId.Free();

                FNativeDelegate OnUpdateSessionComplete;
                MakeOnUpdateSessionCompleteDelegate(&OnUpdateSessionComplete, GameSession);
                UpdateSession(GameSession, GameSessionName, SessionSettings, &OnUpdateSessionComplete);
                DestroyDelegate(&OnUpdateSessionComplete);
                AdvertiseSessionJoinability(GameSession);
            }

            std::vector<std::vector<int32>> TeamLayouts;
            FString Squads;
            auto SquadsKey = JsonKey(L"match.option.squads");

            if (TryGetStringField(MatchOptions, &SquadsKey, &Squads) && Squads.Num() > 1)
            {
                TArray<FString> TeamStrings;
                ParseIntoArray(&Squads, &TeamStrings, L";", true);

                for (auto& TeamString : TeamStrings)
                {
                    TArray<FString> SquadSizes;
                    auto& TeamLayout = TeamLayouts.emplace_back();

                    ParseIntoArray(&TeamString, &SquadSizes, L",", true);

                    for (auto& SquadSize : SquadSizes)
                        TeamLayout.push_back(_wtoi(SquadSize.Num() ? SquadSize.Data : L""));

                    DestroyStringArray(&SquadSizes);
                }

                DestroyStringArray(&TeamStrings);
            }

            Squads.Free();

            FSharedRef PlayerData{};
            auto PlayerDataKey = JsonKey(L"match.option.player.data");

            if (HasTypedField(MatchOptions, &PlayerDataKey, 6))
            {
                PlayerData = *GetObjectField(MatchOptions, &PlayerDataKey);
                IncrementSharedReferenceCount(PlayerData);
            }

            uint8 SquadId = 1;
            TArray<FSharedRef> Players;

            if (GameSession->MaxPlayers > 0)
                Players.Reserve(GameSession->MaxPlayers);

            for (int32 TeamIdx = 0; TeamIdx < MatchAssignment.Teams.Num(); TeamIdx++)
            {
                int32 PartiesInSquad = 0;
                int32 SquadLayoutIdx = 0;

                for (auto& Party : MatchAssignment.Teams[TeamIdx])
                {
                    for (auto& Player : Party)
                    {
                        FUniqueNetIdRepl PlayerId;
                        int32 ElementId;
                        uint8 TeamKey = uint8(TeamIdx);
                        bool bSquadFilled = false;

                        AddSharedRef(Players, Player);
                        MakeUniqueNetIdRepl(&PlayerId, &Player);

                        const void* PlayerToSquadId[] = { &PlayerId, &SquadId };
                        EmplacePlayerToSquadId((void*)(Base + 0x648), &ElementId, PlayerToSquadId);

                        auto SquadIndex = FindSquadIdToPlayer((void*)(Base + 0x698), SquadId, &SquadId);

                        if (SquadIndex == -1)
                        {
                            const uint8* SquadKey = &SquadId;
                            AddSquadIdToPlayer((void*)(Base + 0x698), &SquadIndex, SquadId, &SquadKey);
                        }

                        auto& SquadMembers = *(TArray<FUniqueNetIdRepl>*)(*(uint8**)(Base + 0x698) + 0x20 * SquadIndex + 0x8);
                        auto MemberIndex = SquadMembers.NumElements++;

                        if (SquadMembers.NumElements > SquadMembers.MaxElements)
                            GrowUniqueNetIdRepls(&SquadMembers, MemberIndex);

                        CopyUniqueNetIdRepl(&SquadMembers.Data[MemberIndex], &PlayerId);

                        auto TeamIndex = FindInitialTeamAssignment((void*)(Base + 0x788), TeamKey, &TeamKey);

                        if (TeamIndex == -1)
                        {
                            const uint8* TeamKeyPtr = &TeamKey;
                            AddInitialTeamAssignment((void*)(Base + 0x788), &TeamIndex, TeamKey, &TeamKeyPtr);
                        }

                        EmplaceTeamMember(*(uint8**)(Base + 0x788) + 0x60 * TeamIndex + 0x8, &ElementId, &PlayerId);

                        if (PlayerData.Object)
                        {
                            FString PlayerIdString;
                            FSharedRef* PlayerObject = nullptr;
                            auto ToString = (FString* (*)(void*, FString*))(*(void***)Player.Object)[0x6];

                            ToString(Player.Object, &PlayerIdString);

                            auto PlayerKey = JsonKey(PlayerIdString);
                            auto bFound = TryGetObjectField(PlayerData.Object, &PlayerKey, &PlayerObject);

                            PlayerIdString.Free();

                            if (bFound)
                            {
                                auto FilledKey = JsonKey(L"filled");
                                TryGetBoolField(PlayerObject->Object, &FilledKey, &bSquadFilled);
                            }
                        }

                        const void* SquadFill[] = { &PlayerId, &bSquadFilled };
                        EmplaceSquadFill((void*)(Base + 0x738), &ElementId, SquadFill);
                        DestroyUniqueNetIdRepl(&PlayerId);
                    }

                    if (!TeamLayouts.empty())
                    {
                        auto bLayoutValid = TeamIdx < int32(TeamLayouts.size()) && SquadLayoutIdx < int32(TeamLayouts[TeamIdx].size());

                        if (++PartiesInSquad == (bLayoutValid ? TeamLayouts[TeamIdx][SquadLayoutIdx] : 0))
                        {
                            PartiesInSquad = 0;
                            SquadLayoutIdx++;
                            SquadId++;
                        }
                    }
                }

                if (TeamLayouts.empty())
                {
                    auto TeamIndexKey = TeamIdx;
                    auto Index = FindTeamIndexToSquadId((void*)(Base + 0x6E8), uint32(TeamIndexKey), &TeamIndexKey);

                    if (Index == -1)
                    {
                        const int32* TeamIndexKeyPtr = &TeamIndexKey;
                        AddTeamIndexToSquadId((void*)(Base + 0x6E8), &Index, uint32(TeamIndexKey), &TeamIndexKeyPtr);
                    }

                    *(*(uint8**)(Base + 0x6E8) + 0x10 * Index + 0x4) = SquadId;
                    SquadId++;
                }
            }

            for (auto& Spectator : MatchAssignment.Spectators)
                AddSharedRef(Players, Spectator);

            FString BackfillId;
            FNativeDelegate OnRegisterPlayersComplete;
            uint64 RegisterPlayersCompleteHandle = 0;
            int32 ElementId;
            auto AddOnRegisterPlayersCompleteDelegate = (uint64* (*)(void*, uint64*, FNativeDelegate*))(*(void***)SessionInterface.Object)[0x74];
            auto RegisterPlayers = (bool (*)(void*, FName, TArray<FSharedRef>*, bool))(*(void***)SessionInterface.Object)[0x73];

            QueryUserInfoForMatchmaking(GameSession, &BackfillId, &Players);
            MakeOnRegisterPlayersCompleteDelegate(&OnRegisterPlayersComplete, GameSession, nullptr, &BackfillId);
            AddOnRegisterPlayersCompleteDelegate(SessionInterface.Object, &RegisterPlayersCompleteHandle, &OnRegisterPlayersComplete);

            const void* RegisterPlayerHandle[] = { &BackfillId, &RegisterPlayersCompleteHandle };
            EmplaceRegisterPlayerHandle((void*)(Base + 0x940), &ElementId, RegisterPlayerHandle);

            if (!RegisterPlayers(SessionInterface.Object, GameSessionName, &Players, false))
                DeclineReason = 13;

            DestroyDelegate(&OnRegisterPlayersComplete);
            DestroySharedRefArray(&Players);
            TSharedRef_Destructor(&PlayerData);
        }
        else
            DeclineReason = 12;

        TSharedRef_Destructor(&SessionInterface);
    }

    if (DeclineReason)
        DeclineMatchmaking(GameSession, DeclineReason, &NoBackfillId);
}

static void OnMatchmakingSessionAssignMatchRequestV2(AFortGameSessionDedicatedAthena* GameSession, FMatchAssignment* MatchAssignment)
{
    static auto CopyString = (FString* (*)(FString*, const FString*))(ImageBase + 0x160DDC4);
    static auto MoveMatchAssignment = (FMatchAssignment* (*)(FMatchAssignment*, FMatchAssignment*))(ImageBase + 0x8A2D654);
    static auto DestroyMatchAssignment = (void (*)(FMatchAssignment*))(ImageBase + 0x8A2D11C);
    static auto ResizeAllocation = (void (*)(void*, int32, int32, uint64))(ImageBase + 0x16C9908);
    static auto GetNumberField = (double (*)(void*, const FJsonKey*))(ImageBase + 0x40C5710);
    static auto GetAthenaPlaylist = (UFortPlaylistAthena* (*)(UFortPlaylistManager*, FName))(ImageBase + 0x9947A68);
    static auto GetGlobalGameplayTags = (uint8* (*)())(ImageBase + 0x1BC4494);
    static auto ContainsTag = (bool (*)(const TArray<FGameplayTag>*, const FGameplayTag*))(ImageBase + 0x1BC4FB4);
    static auto AllocateDelegate = (uint8* (*)(int32, FNativeDelegate*))(ImageBase + 0x26E76B0);
    static auto ConstructDataAssetDirectoryDelegate = (void* (*)(uint8*, UObject*))(ImageBase + 0x9CC3A18);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);

    auto Base = __int64(GameSession);
    FMatchAssignment Copy{};

    Copy.bResponded = MatchAssignment->bResponded;
    CopyString(&Copy.MatchId, &MatchAssignment->MatchId);
    CopyString(&Copy.BucketId, &MatchAssignment->BucketId);
    Copy.PlaylistName = MatchAssignment->PlaylistName;
    CopyString(&Copy.RegionId, &MatchAssignment->RegionId);
    CopyString(&Copy.GroupId, &MatchAssignment->GroupId);

    for (auto& Team : MatchAssignment->Teams)
    {
        TArray<TArray<FSharedRef>> TeamCopy;

        for (auto& Party : Team)
        {
            TArray<FSharedRef> PartyCopy;

            for (auto& Player : Party)
                AddSharedRef(PartyCopy, Player);

            TeamCopy.Add(PartyCopy);
        }

        Copy.Teams.Add(TeamCopy);
    }

    for (auto& Spectator : MatchAssignment->Spectators)
        AddSharedRef(Copy.Spectators, Spectator);

    Copy.MatchOptions = MatchAssignment->MatchOptions;
    IncrementSharedReferenceCount(Copy.MatchOptions);
    CopyString(&Copy.PayloadJwt, &MatchAssignment->PayloadJwt);
    MoveMatchAssignment((FMatchAssignment*)(Base + 0x7D8), &Copy);
    DestroyMatchAssignment(&Copy);

    *(bool*)(Base + 0x8A9) = true;

    auto& TemplateFilter = *(TArray<FName>*)(Base + 0x4B8);
    TemplateFilter.NumElements = 0;

    if (TemplateFilter.MaxElements)
    {
        ResizeAllocation(&TemplateFilter.Data, 0, 0, sizeof(FName));
        TemplateFilter.MaxElements = 0;
    }

    auto GameInstance = (UFortGameInstance*)UWorld::GetWorld()->OwningGameInstance;

    if (!GameInstance || !GameInstance->IsA(UFortGameInstance::StaticClass()) || !GameInstance->DataAssetDirectoryManager)
        return OnPlaylistAssetUpdateComplete(GameSession, 0, nullptr);

    auto& CurrentMatchAssignment = *(FMatchAssignment*)(Base + 0x7D8);
    auto MatchOptions = CurrentMatchAssignment.MatchOptions.Object;
    auto RevisionKey = JsonKey(L"match.option.playlistRevision");
    int32 Revision = 0;

    if (HasJsonField((uint8*)MatchOptions, RevisionKey))
        Revision = int32(GetNumberField(MatchOptions, &RevisionKey));

    auto Playlist = GetAthenaPlaylist(GameInstance->PlaylistManager, CurrentMatchAssignment.PlaylistName);

    if (!Playlist)
        return OnPlaylistAssetUpdateComplete(GameSession, 0, nullptr);

    auto GlobalGameplayTags = GetGlobalGameplayTags();
    auto HasPlaylistTag = [&](uint32 TagOffset)
    {
        auto Tag = (FGameplayTag*)(GlobalGameplayTags + TagOffset);

        return !Tag->TagName.IsNone() && (ContainsTag(&Playlist->GameplayTagContainer.GameplayTags, Tag) || ContainsTag(&Playlist->GameplayTagContainer.ParentTags, Tag));
    };

    if (HasPlaylistTag(0x1270) || HasPlaylistTag(0x1244) || HasPlaylistTag(0x1248))
    {
        ResizeAllocation(&TemplateFilter.Data, 0, 4, sizeof(FName));
        TemplateFilter.MaxElements = 4;
        TemplateFilter.NumElements = 4;
        TemplateFilter.Data[0] = *(FName*)(ImageBase + 0x12DF0304);
        TemplateFilter.Data[1] = *(FName*)(ImageBase + 0x12DF0388);
        TemplateFilter.Data[2] = *(FName*)(ImageBase + 0x12DF02C0);
        TemplateFilter.Data[3] = *(FName*)(ImageBase + 0x12DF028C);
    }
    else
        ClearTimer(GameSession, *(uint64*)(Base + 0x4B0));

    *(int32*)(Base + 0xAD0) = Playlist->DADTestValue;

    FPrimaryAssetId AssetId;
    FNativeDelegate OnComplete{};
    auto GetPrimaryAssetId = (FPrimaryAssetId* (*)(UObject*, FPrimaryAssetId*))Playlist->VTable[0x3E];
    auto DataAssetDirectoryManager = (UObject*)GameInstance->DataAssetDirectoryManager;
    auto ApplyAssetRevision = (void (*)(UObject*, int32, FPrimaryAssetId*, int32, FNativeDelegate*))DataAssetDirectoryManager->VTable[0x5F];
    auto Instance = AllocateDelegate(0x28, &OnComplete);

    GetPrimaryAssetId(Playlist, &AssetId);
    ConstructDataAssetDirectoryDelegate(Instance, GameSession);
    *(void**)(Instance + 0x20) = OnPlaylistAssetUpdateComplete;
    ApplyAssetRevision(DataAssetDirectoryManager, 0, &AssetId, Revision, &OnComplete);
    DestroyDelegate(&OnComplete);
}

static void ProcessQueryResults(AFortGameSessionDedicatedAthena* GameSession, const FString* BackfillId)
{
    static auto HashString = (uint32 (*)(int32, const wchar_t*))(ImageBase + 0x160F930);
    static auto FindString = (int32 (*)(void*, uint32, const FString*))(ImageBase + 0x5144CF8);
    static auto RemoveString = (int32 (*)(void*, const FString*))(ImageBase + 0x5A96088);
    static auto DeclineMatchmaking = (void (*)(AFortGameSessionDedicatedAthena*, uint8, const FString*))(ImageBase + 0x9D15934);

    auto Base = __int64(GameSession);
    auto RegisterPlayerResults = (void*)(Base + 0x8F0);
    auto QueryUserInfoResults = (void*)(Base + 0x990);
    auto Hash = HashString(JsonKey(*BackfillId).Len, BackfillId->Num() ? BackfillId->Data : L"");
    auto RegisterPlayerResult = FindString(RegisterPlayerResults, Hash, BackfillId);

    if (RegisterPlayerResult == -1 || FindString(QueryUserInfoResults, Hash, BackfillId) == -1)
        return;

    auto MatchmakingService = *(void**)(Base + 0x8B0);

    if (!*(bool*)(*(uint8**)RegisterPlayerResults + 0x20 * RegisterPlayerResult + 0x10))
        DeclineMatchmaking(GameSession, 15, BackfillId);
    else if (MatchmakingService && ((bool (*)(void*))(*(void***)MatchmakingService)[0x3])(MatchmakingService))
    {
        if (*(bool*)(Base + 0x8D8))
        {
            auto RespondToMatchBackfill = (bool (*)(void*, const FString*, uint8))(*(void***)MatchmakingService)[0x5];

            RespondToMatchBackfill(MatchmakingService, BackfillId, 0);
            SetActorTimer(GameSession, (void*)(ImageBase + 0x5375284), *(uint64*)(Base + 0x8E8), *(float*)(Base + 0x8DC));
        }
        else
        {
            auto RespondToMatchAssignment = (bool (*)(void*, uint8))(*(void***)MatchmakingService)[0x4];

            RespondToMatchAssignment(MatchmakingService, 0);
        }
    }

    RemoveString(RegisterPlayerResults, BackfillId);
    RemoveString(QueryUserInfoResults, BackfillId);
}

static void (*OnRegisterPlayersCompleteOG)(AFortGameSessionDedicatedAthena*, FName, void*, bool, FString*);
static void OnRegisterPlayersComplete(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, void* Players, bool bWasSuccessful, FString* BackfillId)
{
    static auto CopyString = (FString* (*)(FString*, const FString*))(ImageBase + 0x160DDC4);

    FString Id;
    CopyString(&Id, BackfillId);

    OnRegisterPlayersCompleteOG(GameSession, SessionName, Players, bWasSuccessful, BackfillId);

    if (*(bool*)(__int64(GameSession) + 0x8A9) && SessionName == GetGameSessionName())
        ProcessQueryResults(GameSession, &Id);

    Id.Free();
}

static void (*OnQueryUserInfoCompleteOG)(AFortGameSessionDedicatedAthena*, int32, bool, void*, const FString*, FString*);
static void OnQueryUserInfoComplete(AFortGameSessionDedicatedAthena* GameSession, int32 LocalUserNum, bool bWasSuccessful, void* Users, const FString* ErrorString, FString* BackfillId)
{
    static auto CopyString = (FString* (*)(FString*, const FString*))(ImageBase + 0x160DDC4);

    FString Id;
    CopyString(&Id, BackfillId);

    OnQueryUserInfoCompleteOG(GameSession, LocalUserNum, bWasSuccessful, Users, ErrorString, BackfillId);

    if (*(bool*)(__int64(GameSession) + 0x8A9))
        ProcessQueryResults(GameSession, &Id);

    Id.Free();
}

static void RegisterServer(AGameSession* GameSession)
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto InitHostBeacon = (void (*)(AGameSession*, FOnlineSessionSettings*))(ImageBase + 0x9CD9E24);
    static auto CreateGameSessionDelegate = (FNativeDelegate* (*)(FNativeDelegate*, AGameSession*))(ImageBase + 0x9CFA688);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);

    auto GameInstance = UWorld::GetWorld()->OwningGameInstance;
    auto& EligibilityResolver = *(uint8**)(__int64(GameInstance) + 0xD38);

    if (!EligibilityResolver)
    {
        auto ReferenceController = (uint8*)FMemory::Malloc(0x28, 0x10);

        *(void**)ReferenceController = (void*)(ImageBase + 0xF396090);
        *(int32*)(ReferenceController + 0x8) = 1;
        *(int32*)(ReferenceController + 0xC) = 1;
        *(void**)(ReferenceController + 0x10) = nullptr;
        *(void**)(ReferenceController + 0x18) = nullptr;
        *(FWeakObjectPtr*)(ReferenceController + 0x20) = FWeakObjectPtr(GameInstance);

        EligibilityResolver = ReferenceController + 0x10;
        *(uint8**)(__int64(GameInstance) + 0xD40) = ReferenceController;
    }

    auto SessionInterface = GetSessionInterface();

    if (SessionInterface.Object)
    {
        auto AddOnCreateSessionCompleteDelegate = (uint64* (*)(void*, uint64*, FNativeDelegate*))(*(void***)SessionInterface.Object)[0xA];
        auto CreateSession = (bool (*)(void*, int32, FName, const FOnlineSessionSettings&))(*(void***)SessionInterface.Object)[0x9];
        FOnlineSessionSettings SessionSettings{};
        FNativeDelegate OnCreateSessionCompleteDelegate;

        SessionSettings.NumPublicConnections = GameSession->MaxPlayers;
        SessionSettings.bAllowJoinInProgress = true;

        CreateGameSessionDelegate(&OnCreateSessionCompleteDelegate, GameSession);
        *(void**)(OnCreateSessionCompleteDelegate.Instance + 0x20) = OnCreateSessionComplete;
        AddOnCreateSessionCompleteDelegate(SessionInterface.Object, &CreateSessionCompleteHandle, &OnCreateSessionCompleteDelegate);
        DestroyDelegate(&OnCreateSessionCompleteDelegate);

        CreateSession(SessionInterface.Object, 0, GameSession->SessionName, SessionSettings);

        if (auto CreatedSettings = GetSessionSettings(SessionInterface, GameSession->SessionName))
        {
            auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
            auto UpdateSessionSettings = (void (*)(AGameSession*, FOnlineSessionSettings*, int32, FString*, FString*))GameSession->VTable[0x11D];

            UpdateSessionSettings(GameSession, CreatedSettings, GameMode->CurrentPlaylistId, &GameMode->CurrentZoneInstanceId, &GameMode->CurrentWUID);
            InitHostBeacon(GameSession, CreatedSettings);
        }
    }

    TSharedRef_Destructor(&SessionInterface);
}

static bool HasGameSession()
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);

    auto SessionInterface = GetSessionInterface();
    auto bHasGameSession = SessionInterface.Object && GetSessionSettings(SessionInterface, GetGameSessionName());

    TSharedRef_Destructor(&SessionInterface);

    return bHasGameSession;
}

static void OnDestroySessionComplete(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, bool bWasSuccessful)
{
    auto Base = __int64(GameSession);

    if (*(bool*)(Base + 0xAD8))
        ClearTimer(GameSession, *(uint64*)(Base + 0xAA8));

    RegisterServer(GameSession);
}

static void CleanupDedicatedServerSession(AGameSession* GameSession, void* OnComplete)
{
    static auto CreateGameSessionDelegate = (FNativeDelegate* (*)(FNativeDelegate*, AGameSession*))(ImageBase + 0x9CFA688);
    static auto SafeSessionDelete = (void (*)(UGameInstance*, FName, FNativeDelegate*, void*))(ImageBase + 0x980D828);

    FNativeDelegate CompletionDelegate;

    CreateGameSessionDelegate(&CompletionDelegate, GameSession);
    *(void**)(CompletionDelegate.Instance + 0x20) = OnComplete;
    SafeSessionDelete(UWorld::GetWorld()->OwningGameInstance, GetGameSessionName(), &CompletionDelegate, nullptr);
}

static void ClearSessionTimers(AFortGameSessionDedicatedAthena* GameSession)
{
    auto Base = __int64(GameSession);

    ClearTimer(GameSession, *(uint64*)(Base + 0x5D8));
    ClearTimer(GameSession, *(uint64*)(Base + 0x4A0));
    ClearTimer(GameSession, *(uint64*)(Base + 0x4B0));
    ClearTimer(GameSession, *(uint64*)(Base + 0x4D0));
}

static void OnShutdownSessionDestroyed(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, bool bWasSuccessful)
{
    static auto RequestExitWithStatus = (void (*)(bool, uint8, const wchar_t*))(ImageBase + 0x41D939C);

    if (SessionName == GetGameSessionName())
        RequestExitWithStatus(false, uint8(ShutdownExitCode), nullptr);
}

static void DedicatedShutdownDedicatedServer(AFortGameSessionDedicatedAthena* GameSession, int32 ExitCode)
{
    static auto Broadcast = (void (*)(void*, int32))(ImageBase + 0x59E4F90);

    auto StopMeshNetwork = (void (*)(AFortGameSessionDedicatedAthena*))GameSession->VTable[0x125];
    auto ClearServerDelegates = (void (*)(AFortGameSessionDedicatedAthena*))GameSession->VTable[0x11C];
    auto DestroyHostBeacon = (void (*)(AFortGameSessionDedicatedAthena*, bool))GameSession->VTable[0x11A];

    Broadcast((void*)(ImageBase + 0x12A9CEB0), ExitCode);
    StopMeshNetwork(GameSession);
    ClearServerDelegates(GameSession);
    DestroyHostBeacon(GameSession, false);
    ClearSessionTimers(GameSession);
    CleanupDedicatedServerSession(GameSession, OnShutdownSessionDestroyed);
}

static void OnAllPlayersUnregistered(AFortGameSessionDedicatedAthena* GameSession)
{
    static auto Broadcast = (void (*)(void*))(ImageBase + 0x59E4F90);
    static auto GetMapName = (FString* (*)(UWorld*, FString*))(ImageBase + 0x2268120);
    static auto AssignString = (FString* (*)(FString*, const FString*))(ImageBase + 0x4A3CC98);
    static auto LeavingMap = FName(L"LeavingMap");

    if (*(bool*)(ImageBase + 0x12C11FE4))
    {
        auto ShutdownDedicatedServer = (void (*)(AFortGameSessionDedicatedAthena*, int32))GameSession->VTable[0x12B];

        return ShutdownDedicatedServer(GameSession, *(int32*)(ImageBase + 0x12C11FE0));
    }

    Broadcast((void*)(ImageBase + 0x12A9CE98));

    auto World = UWorld::GetWorld();
    auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
    auto DestroyHostBeacon = (void (*)(AFortGameSessionDedicatedAthena*, bool))GameSession->VTable[0x11A];

    DestroyHostBeacon(GameSession, false);

    if (GameMode->MatchState == LeavingMap)
        return;

    auto CanServerTravel = (bool (*)(AFortGameMode*, const FString*, bool))GameMode->VTable[0xFF];
    FString URL;

    GameMode->bUseSeamlessTravel = false;
    GetMapName(World, &URL);

    if (CanServerTravel(GameMode, &URL, true))
    {
        *(uint8*)(__int64(World) + 0x749) = 0;

        if (*(int32*)(__int64(World) + 0x758) <= 1)
            AssignString((FString*)(__int64(World) + 0x750), &URL);
    }

    URL.Free();
}

static void DedicatedRestart(AFortGameSessionDedicatedAthena* GameSession)
{
    static auto GetCsvProfiler = (void* (*)())(ImageBase + 0x175B328);
    static auto EndCapture = (void (*)(void*, FSharedRef*, void**))(ImageBase + 0x5AB6E74);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto IsInSeamlessTravel = (bool (*)(UWorld*))(ImageBase + 0x2009328);
    static auto PauseBeaconRequests = (void (*)(AFortGameSessionDedicatedAthena*, bool))(ImageBase + 0x9CE6B40);
    static auto BindTimerDelegate = (void (*)(FNativeDelegate*, UObject*, void*))(ImageBase + 0x9816384);
    static auto UnregisterAllPlayerInfos = (void (*)(UGameInstance*, FNativeDelegate*))(ImageBase + 0x9811B74);
    static auto GetAllRegistries = (void (*)(void*, TArray<UDataRegistry*>*, bool))(ImageBase + 0x74CFAB0);
    static auto PurgeAllOverrides = (void (*)(UDataRegistry*))(ImageBase + 0x97E9D1C);

    auto CsvProfiler = GetCsvProfiler();

    if (CsvProfiler && *(bool*)(ImageBase + 0x12D5C3C0))
    {
        FSharedRef Result{};
        void* EventToSignal = nullptr;

        EndCapture(CsvProfiler, &Result, &EventToSignal);
        TSharedRef_Destructor(&Result);
    }

    ClearSessionTimers(GameSession);

    auto World = UWorld::GetWorld();
    auto GameMode = (AFortGameMode*)World->AuthorityGameMode;

    if (GameMode->bTravelInitiated || IsInSeamlessTravel(World))
        return;

    auto ClearServerDelegates = (void (*)(AFortGameSessionDedicatedAthena*))GameSession->VTable[0x11C];
    auto StopMeshNetwork = (void (*)(AFortGameSessionDedicatedAthena*))GameSession->VTable[0x125];

    ClearServerDelegates(GameSession);
    GameMode->bTravelInitiated = true;
    PauseBeaconRequests(GameSession, true);

    FNativeDelegate OnAllPlayersUnregisteredDelegate{};

    BindTimerDelegate(&OnAllPlayersUnregisteredDelegate, GameSession, OnAllPlayersUnregistered);
    UnregisterAllPlayerInfos(World->OwningGameInstance, &OnAllPlayersUnregisteredDelegate);

    if (auto DataRegistrySubsystem = *(void**)(ImageBase + 0x12D18AE0))
    {
        TArray<UDataRegistry*> Registries;
        GetAllRegistries(DataRegistrySubsystem, &Registries, false);

        for (auto Registry : Registries)
        {
            if (Registry && Registry->IsA(UFortDataRegistry::StaticClass()))
                PurgeAllOverrides(Registry);
        }

        Registries.Free();
    }

    StopMeshNetwork(GameSession);
}

static void StopWaitingForLeaveMeshNetwork()
{
    static auto RemoveDelegateInstance = (bool (*)(void*, uint64))(ImageBase + 0x1C1C39C);

    RemoveDelegateInstance((void*)(ImageBase + 0x12A9CF10), LeaveMeshNetworkCompleteHandle);
    LeaveMeshNetworkCompleteHandle = 0;
}

static bool LeaveMatchmakingAndMeshNetwork(AFortGameSessionDedicatedAthena* GameSession, void* OnLeaveMeshNetworkComplete)
{
    static auto LeaveMeshNetwork = (bool (*)(AFortGameSessionDedicatedAthena*))(ImageBase + 0x9D1507C);
    static auto CreateLeaveMeshNetworkDelegate = (FNativeDelegate* (*)(FNativeDelegate*, AFortGameSessionDedicatedAthena*, void*))(ImageBase + 0x9CFA634);
    static auto AddDelegateInstance = (uint64* (*)(void*, uint64*, FNativeDelegate*))(ImageBase + 0x2710030);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);

    auto DisconnectFromMatchmakingServiceV2 = (bool (*)(AFortGameSessionDedicatedAthena*, bool))GameSession->VTable[0x139];

    ClearTimer(GameSession, *(uint64*)(__int64(GameSession) + 0xAA0));
    MatchmakingWaitStart = 0;
    DisconnectFromMatchmakingServiceV2(GameSession, true);

    if (!LeaveMeshNetwork(GameSession))
        return false;

    FNativeDelegate OnLeaveMeshNetworkCompleteDelegate;

    CreateLeaveMeshNetworkDelegate(&OnLeaveMeshNetworkCompleteDelegate, GameSession, OnLeaveMeshNetworkComplete);
    AddDelegateInstance((void*)(ImageBase + 0x12A9CF10), &LeaveMeshNetworkCompleteHandle, &OnLeaveMeshNetworkCompleteDelegate);
    DestroyDelegate(&OnLeaveMeshNetworkCompleteDelegate);
    return true;
}

static void OnRestartLeaveMeshNetworkComplete(AFortGameSessionDedicatedAthena* GameSession, const FName& MeshName, const void* Error)
{
    StopWaitingForLeaveMeshNetwork();
    DedicatedRestart(GameSession);
}

static void Restart(AFortGameSessionDedicatedAthena* GameSession)
{
    if (!LeaveMatchmakingAndMeshNetwork(GameSession, OnRestartLeaveMeshNetworkComplete))
        DedicatedRestart(GameSession);
}

static void OnShutdownLeaveMeshNetworkComplete(AFortGameSessionDedicatedAthena* GameSession, const FName& MeshName, const void* Error)
{
    StopWaitingForLeaveMeshNetwork();
    DedicatedShutdownDedicatedServer(GameSession, ShutdownExitCode);
}

static void ShutdownDedicatedServer(AFortGameSessionDedicatedAthena* GameSession, int32 ExitCode)
{
    ShutdownExitCode = ExitCode;

    if (!LeaveMatchmakingAndMeshNetwork(GameSession, OnShutdownLeaveMeshNetworkComplete))
        DedicatedShutdownDedicatedServer(GameSession, ExitCode);
}

void PauseWarmup(EGameModePauseWarmupMaskId MaskId)
{
    PauseWarmupMask |= uint8(MaskId);
}

void UnPauseWarmup(EGameModePauseWarmupMaskId MaskId)
{
    auto bWasPaused = PauseWarmupMask != 0;

    PauseWarmupMask &= ~uint8(MaskId);

    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

    if (bWasPaused && !PauseWarmupMask && GamePhaseLogic && GamePhaseLogic->GamePhase == EAthenaGamePhase::Warmup)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

        GamePhaseLogic->WarmupCountdownStartTime = Time;
        GamePhaseLogic->WarmupCountdownEndTime = Time + GamePhaseLogic->WarmupCountdownDuration;
    }
}

static void SetGamePhase(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, EAthenaGamePhase GamePhase)
{
    static auto SetGamePhaseInternal = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, EAthenaGamePhase))(ImageBase + 0xA4C8F3C);

    SetGamePhaseInternal(GamePhaseLogic, GamePhase);
}

static void SetGamePhaseStep(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, EAthenaGamePhaseStep GamePhaseStep)
{
    static auto BroadcastGamePhaseStepUpdatedEvent = (void (*)(const UClass*, UObject*, EAthenaGamePhaseStep*, UObject*))(ImageBase + 0x3639070);

    auto OldGamePhaseStep = GamePhaseLogic->GamePhaseStep;

    GamePhaseLogic->GamePhaseStep = GamePhaseStep;
    GamePhaseLogic->HandleGamePhaseStepChanged(GamePhaseStep);

    if (OldGamePhaseStep != GamePhaseStep)
        BroadcastGamePhaseStepUpdatedEvent(AFortGameState::StaticClass(), GamePhaseLogic->GetOwner(), &GamePhaseStep, GamePhaseLogic);
}

static void SetSafeZoneState(AFortSafeZoneIndicator* SafeZoneIndicator, EFortSafeZoneState SafeZoneState)
{
    static auto SetSafeZoneStateInternal = (void (*)(AFortSafeZoneIndicator*, EFortSafeZoneState))(ImageBase + 0x1FE498C);

    SetSafeZoneStateInternal(SafeZoneIndicator, SafeZoneState);
}

static void StartAircraftPhase(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto Reset = (void (*)(AFortPlayerControllerAthena*))(ImageBase + 0x6F854DC);

    if (GamePhaseLogic->GamePhase >= EAthenaGamePhase::Aircraft)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
    auto Playlist = (UFortPlaylistAthena*)((AFortGameStateAthena*)GameMode->GameState)->CurrentPlaylistInfo.BasePlaylist;

    SetConsoleTitleA("Remix 32.00 | Match started");

    TournamentMatchStarted(GameMode);

    if (Playlist && Playlist->bAllowJoinInProgress)
        *(bool*)(__int64(GameMode) + 0x1018) = false;

    auto SessionInterface = GetSessionInterface();

    if (SessionInterface.Object)
    {
        auto UpdateSession = (bool (*)(void*, FName, FOnlineSessionSettings&, bool))(*(void***)SessionInterface.Object)[0x13];

        if (auto SessionSettings = GetSessionSettings(SessionInterface, GameMode->GameSession->SessionName))
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
            GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;

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
                if (Pawn->Role == ENetRole::ROLE_Authority && Pawn->bIsInAnyStorm)
                {
                    Pawn->bIsInAnyStorm = false;
                    Pawn->OnRep_IsInAnyStorm();
                }

                Pawn->bIsInsideSafeZone = true;
                Pawn->OnRep_IsInsideSafeZone();
                Pawn->OnEnteredAircraft.Process();
            }

            Player->ClientActivateSlot(EFortQuickBars(0), 0, 0.f, true, true);

            if (Pawn)
                Pawn->K2_DestroyActor();

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
    static auto UpdateGamePhaseStep = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, float))(ImageBase + 0x1BB24D0);

    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    auto SafeZoneIndicator = GamePhaseLogic->SafeZoneIndicator;
    auto& SafeZonePhases = SafeZoneIndicator->SafeZonePhases;

    if (!SafeZonePhases.IsValidIndex(NewSafeZonePhase))
        return;

    if (SafeZonePhases.IsValidIndex(NewSafeZonePhase - 1))
    {
        auto& PreviousPhaseInfo = SafeZonePhases[NewSafeZonePhase - 1];

        (FVector&)SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
        SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
    }

    auto& PhaseInfo = SafeZonePhases[NewSafeZonePhase];

    (FVector&)SafeZoneIndicator->NextCenter = PhaseInfo.Center;
    SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
    SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

    if (SafeZonePhases.IsValidIndex(NewSafeZonePhase + 1))
    {
        auto& NextPhaseInfo = SafeZonePhases[NewSafeZonePhase + 1];

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

    bool bInitial = OldPhase <= 0;

    SetSafeZoneState(SafeZoneIndicator, EFortSafeZoneState::Holding);
    UpdateGamePhaseStep(GamePhaseLogic, 0.f);

    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

    if (!bInitial)
    {
        for (auto& PlayerController : GameMode->AlivePlayers)
            SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::StormPhase, 1, true);
    }
}

static void InitializeSafeZoneLocations(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    static auto SetSafeZoneLocations = (void (*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, TArray<FVector>*))(ImageBase + 0xA4C93D4);

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

static bool bCalendarPlaylistDone = false;
static ULONGLONG CalendarWaitStart = 0;

static bool SelectCalendarPlaylist()
{
    if (bCalendarPlaylistDone || bReload)
        return true;

    auto ManagerClass = UObject::FindClassFast("BP_Clyde_DataLayerManager_C");
    auto Manager = ManagerClass ? GetFirstInstance(ManagerClass) : nullptr;
    auto CurrentPlaylist = FindObject<UFortPlaylistAthena>(Playlist.c_str());

    if (!Manager || !CurrentPlaylist || CurrentPlaylist->bIsTournament)
        return bCalendarPlaylistDone = true;

    auto EventsProperty = FindProperty(Manager, "ClydeWeekCalendarEventsToWatch");
    auto WeekTagsProperty = FindProperty(Manager, "Apollo Retro Seasonly Gameplay Tags");
    auto UsePlaylistsProperty = (FBoolProperty*)FindProperty(Manager, "bShouldUseClydePlaylists");

    if (!EventsProperty || !WeekTagsProperty)
        return bCalendarPlaylistDone = true;

    if (UsePlaylistsProperty && (GetProperty<uint8>(Manager, UsePlaylistsProperty) & UsePlaylistsProperty->FieldMask))
        return bCalendarPlaylistDone = true;

    auto& Events = GetProperty<TArray<FString>>(Manager, EventsProperty);
    auto& WeekTags = GetProperty<TArray<FGameplayTag>>(Manager, WeekTagsProperty);
    auto& PlaylistTags = CurrentPlaylist->GameplayTagContainer.GameplayTags;

    for (auto& Tag : PlaylistTags)
    {
        if (HasTag(WeekTags, Tag))
            return bCalendarPlaylistDone = true;
    }

    auto World = UWorld::GetWorld();
    auto GameInstance = World ? (UFortGameInstance*)World->OwningGameInstance : nullptr;
    auto EventManager = GameInstance ? GameInstance->SeasonalEventManager : nullptr;

    if (!EventManager)
        return bCalendarPlaylistDone = true;

    if (*(uint8*)(__int64(EventManager) + 0x180))
    {
        if (!CalendarWaitStart)
            CalendarWaitStart = GetTickCount64();

        if (GetTickCount64() - CalendarWaitStart < 60000)
            return false;

        return bCalendarPlaylistDone = true;
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
        return bCalendarPlaylistDone = true;

    auto& WeekTag = WeekTags[Week];

    const UObject* ManagerPackage = Manager->Class;

    while (ManagerPackage && ManagerPackage->Outer)
        ManagerPackage = ManagerPackage->Outer;

    auto PackageName = ManagerPackage ? ManagerPackage->Name.GetRawString() : std::string();
    auto RootEnd = PackageName.size() > 1 ? PackageName.find('/', 1) : std::string::npos;

    if (RootEnd == std::string::npos)
        return bCalendarPlaylistDone = true;

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

    return bCalendarPlaylistDone = true;
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

static UFortPlaylistAthena* FindPlaylist()
{
    if (auto Found = FindObject<UFortPlaylistAthena>(Playlist.c_str()))
        return Found;

    return FindObject<UFortPlaylistAthena>(L"/BRPlaylists/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");
}

static std::unordered_map<int, float> VendingRarityWeights;
static float VendingRarityWeightSum = 0;
static float VendingRarityZeroWeight;
static float VendingTotalWeight;

static void (*ReadyToStartMatchOG)(UObject*, FFrame&, bool*);
static void ReadyToStartMatch(UObject* Context, FFrame& Stack, bool* Ret)
{
    static auto GetWorldContextFromWorld = (void* (*)(UEngine*, UWorld*))(ImageBase + 0x175C254);
    static auto CreateNetDriver_Local = (UNetDriver* (*)(UEngine*, void*, FName, FName))(ImageBase + 0x1536974);
    static auto SetWorld = (void (*)(UNetDriver*, UWorld*))(ImageBase + 0x2494394);
    static auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))(ImageBase + 0x71BBFDC);
    static auto SetAISettings = (void (*)(AFortGameModeAthena*, UAthenaAISettings*))(ImageBase + 0x8F991FC);
    static auto StartStreamingAdditionalPlaylistLevel = (void (*)(AFortGameStateAthena*, FName, bool))(ImageBase + 0x984CF30);
    static auto GetLevelsToStream = (bool (*)(UFortGameStateComponent_UEFNProjects*, TArray<FName>*))(ImageBase + 0x90D8CA0);
    static auto ChangeGameFeatureTargetState = (void (*)(UGameFeaturesSubsystem*, const FString&, EGameFeatureTargetState, const void*))(ImageBase + 0x189B27C);
    static auto InitializePoiManager = (void (*)(AFortPoiManager*))(ImageBase + 0x908918C);
    static auto UpdateCullDistanceServer = (void (*)(ABuildingActor*))(ImageBase + 0x92D226C);
    static auto InitializeMapInfo = (AFortAthenaMapInfo* (*)(AFortGameModeAthena*, AFortGameStateAthena*))(ImageBase + 0x8F9DDA0);
    static auto InitializeFlightPath = (void (*)(AFortAthenaMapInfo*, AFortGameStateAthena*, UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, bool, double, float, float))(ImageBase + 0x8F0EF30);

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

        FString Error;

        SetWorld(NetDriver, World);

        if (InitListen(NetDriver, World, &URL, false, Error))
            SetWorld(NetDriver, World);

        GameMode->WarmupRequiredPlayerCount = 1;

        auto Playlist = FindPlaylist();

        if (Playlist)
        {
            Playlist->GarbageCollectionFrequency = 9999999999999999.f;
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

            for (auto& AdditionalLevel : StreamingPlaylist->AdditionalLevels)
                StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, false);

            for (auto& AdditionalLevel : StreamingPlaylist->AdditionalLevelsServerOnly)
                StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, true);

            if (StreamingPlaylist->SharedAssetGroup)
            {
                for (auto& SharedAsset : StreamingPlaylist->SharedAssetGroup->SharedAssetsToLoad)
                {
                    for (auto& AdditionalLevel : SharedAsset->SharedAdditionalLevels)
                        StartStreamingAdditionalPlaylistLevel(GameState, AdditionalLevel.ObjectID.AssetPath.PackageName, false);
                }
            }

            if (auto UEFNProjects = (UFortGameStateComponent_UEFNProjects*)GameState->GetComponentByClass(UFortGameStateComponent_UEFNProjects::StaticClass()))
            {
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

        if (HasGameSession())
            CleanupDedicatedServerSession(GameMode->GameSession, OnDestroySessionComplete);
        else
            RegisterServer(GameMode->GameSession);

        if (auto MeshNetworkSubsystem = (UMeshNetworkSubsystem*)USubsystemBlueprintLibrary::GetGameInstanceSubsystem(World, UMeshNetworkSubsystem::StaticClass()))
            MeshNetworkSubsystem->NodeType = EMeshNetworkNodeType::Edge;

        if (!GameMode->AIDirector)
        {
            GameMode->AIDirector = SpawnActor<AFortAIDirector>(AAthenaAIDirector::StaticClass(), FVector{}, {}, GameMode);

            if (GameMode->AIDirector)
                GameMode->AIDirector->Activate();
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
            InitializePoiManager(PoiManager);

        if (World->NetDriver && GameState->HasBegunPlay())
        {
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

        if (bLateGame && GameState->MapInfo->FlightInfos.Num() > 0)
        {
            auto& FlightInfo = GameState->MapInfo->FlightInfos[0];
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

            if (GameState->TeamFlightPaths.Num() > 0)
                GameState->TeamFlightPaths[0] = FlightInfo;

            if (GameState->TeamFlightPaths.Num() == 1)
                GameState->FlightPathMidLine = FlightInfo;
        }

        AbilitySets.Add(FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer"));

        auto TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprintGame/Gameplay/AS_TacticalSprint.AS_TacticalSprint");

        if (!TacticalSprintAbility)
            TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprint/Gameplay/AS_TacticalSprint.AS_TacticalSprint");

        AbilitySets.Add(TacticalSprintAbility);
        AbilitySets.Add(FindObject<UFortAbilitySet>(L"/Ascender/Gameplay/Ascender/AS_Ascender.AS_Ascender"));
        AbilitySets.Add(FindObject<UFortAbilitySet>(L"/DoorBashContent/Gameplay/AS_DoorBash.AS_DoorBash"));
        AbilitySets.Add(FindObject<UFortAbilitySet>(L"/HillScramble/Gameplay/AS_HillScramble.AS_HillScramble"));
        AbilitySets.Add(FindObject<UFortAbilitySet>(L"/SlideImpulse/Gameplay/AS_SlideImpulse.AS_SlideImpulse"));

        if (Playlist)
        {
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
        }

        for (auto& Set : AbilitySets)
        {
            if (Set)
                Set->AddToRoot();
        }

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

                VendingRarityWeights[i] = RarityWeight;
                VendingRarityWeightSum += RarityWeight;
            }

            UDataTableFunctionLibrary::EvaluateCurveTableRow(VendingMachineRarityCount.Curve.CurveTable, VendingMachineRarityCount.Curve.RowName, 0.f, nullptr, &VendingRarityZeroWeight, FString());

            VendingTotalWeight = 0.0f;

            for (auto& [Rarity, RarityWeight] : VendingRarityWeights)
                VendingTotalWeight += RarityWeight;
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

    if (!NewPlayer || (!NewPlayer->IsA(AFortPlayerControllerAthena::StaticClass()) && (!NewPlayer->PlayerState || !NewPlayer->PlayerState->bIsABot)) || !StartSpot)
        return;

    auto PawnClass = GameMode->GetDefaultPawnClassForController(NewPlayer);

    if (!PawnClass)
        return;

    auto Pawn = (APawn*)SpawnActor(PawnClass, StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding);

    while (!Pawn)
    {
        auto PlayerStart = GameMode->ChoosePlayerStart(NewPlayer);

        if (PlayerStart)
            Pawn = (APawn*)SpawnActor(PawnClass, PlayerStart->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding);
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

struct FVoiceChatTokenRequest
{
    TWeakObjectPtr<AFortPlayerControllerZone> PlayerController;
    int32 RequestID;
    FString ChannelName;
    FSharedRef HttpRequest;
};

static std::vector<FVoiceChatTokenRequest> VoiceChatTokenRequests;
static std::wstring CurrentGameSessionId;

static const std::wstring& GetCurrentGameSessionId()
{
    if (CurrentGameSessionId.empty())
    {
        std::random_device Device;
        CurrentGameSessionId = std::format(L"{:08x}{:08x}{:08x}{:08x}", Device(), Device(), Device(), Device());
    }

    return CurrentGameSessionId;
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
    static auto TryGetStringField = (bool (*)(void*, const FJsonKey*, FString*))(ImageBase + 0x1D8F394);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    alignas(16) static const FJsonKey TokenField{ L"token", 5 };
    alignas(16) static const FJsonKey ErrorCodeField{ L"errorCode", 9 };

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
    static auto CopyGameMemberInfo = (FGameMemberInfo* (*)(FGameMemberInfo*, const FGameMemberInfo*))(ImageBase + 0x3301224);
    static auto NotifyGameMemberAdded = (void (*)(AFortGameStateAthena*, uint8, uint8, FUniqueNetIdRepl*))(ImageBase + 0x24AB834);

    APlayerController* NewPlayer;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.IncrementCode();

    auto GameMode = (AFortGameModeAthena*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    auto PlayerController = (AFortPlayerControllerAthena*)NewPlayer;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    PlayerState->OnRep_SquadId();

    FGameMemberInfo Member{};

    Member.MostRecentArrayReplicationKey = -1;
    Member.ReplicationID = -1;
    Member.ReplicationKey = -1;
    Member.TeamIndex = PlayerState->TeamIndex;
    Member.SquadID = PlayerState->SquadID;
    Member.MemberUniqueId = PlayerState->UniqueID;

    auto& Members = GameState->GameMemberInfoArray.Members;

    Members.Reserve(1);

    auto& NewMember = Members.Data[Members.NumElements++];

    CopyGameMemberInfo(&NewMember, &Member);
    GameState->GameMemberInfoArray.MarkItemDirty(NewMember);
    NotifyGameMemberAdded(GameState, NewMember.SquadID, NewMember.TeamIndex, &NewMember.MemberUniqueId);

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

        if (SquadPlayerState && SquadPlayerState->IsA(AFortPlayerStateAthena::StaticClass()) && SquadPlayerState->SquadID == PlayerState->SquadID && SquadPlayerState->Owner && SquadPlayerState->Owner->IsA(AFortPlayerControllerZone::StaticClass()))
            SendVoiceChatChannelJoin(GameMode, (AFortPlayerControllerZone*)SquadPlayerState->Owner);
    }
}

static UClass** GetGameSessionClass(AFortGameModeAthena* GameMode, UClass** Result)
{
    *Result = AFortGameSessionDedicatedAthena::StaticClass();
    return Result;
}

static double GetServerWorldTimeSeconds(AGameStateBase* GameState)
{
    static auto GetTimeSeconds = (double (*)(UObject*))(ImageBase + 0x1732950);

    return GetTimeSeconds(GameState);
}

static void (*InitGameOG)(AFortGameModeAthena*, const FString*, const FString*, FString*);
static void InitGame(AFortGameModeAthena* GameMode, const FString* MapName, const FString* Options, FString* ErrorMessage)
{
    for (auto& Resetter : Resetters)
        Resetter();

    InitGameOG(GameMode, MapName, Options, ErrorMessage);
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

static void AddLootTierData(UDataTable* Table)
{
    if (!Table)
        return;

    Table->AddToRoot();

    for (auto& Pair : Table->RowMap)
    {
        auto TierData = (FFortLootTierData*)Pair.Value();

        TierDataMap[TierData->TierGroup.ComparisonIndex].Add(TierData);
    }
}

static void AddLootPackageData(UDataTable* Table)
{
    if (!Table)
        return;

    Table->AddToRoot();

    std::unordered_map<int32, FFortLootPackageData*> PackagesByRowName;

    for (auto& Pair : Table->RowMap)
        PackagesByRowName[Pair.Key().ComparisonIndex] = (FFortLootPackageData*)Pair.Value();

    for (auto& [_, Package] : PackagesByRowName)
        LootPackageMap[Package->LootPackageID.ComparisonIndex].Add(Package);
}

static void (*FinishWorldInitializationOG)(AFortGameModeAthena*, AActor*);
static void FinishWorldInitialization(AFortGameModeAthena* GameMode, AActor* WorldManager)
{
    static auto GetGameFeatureDataForActivePlugin = (UGameFeatureData* (*)(UGameFeaturePluginStateMachine*))(ImageBase + 0x29EB49C);

    FinishWorldInitializationOG(GameMode, WorldManager);

    auto Playlist = FindPlaylist();
    auto LootTierData = Playlist ? Playlist->LootTierData.Get() : nullptr;

    if (!LootTierData)
        LootTierData = FindObject<UDataTable>(L"/Game/Items/Datatables/AthenaLootTierData_Client.AthenaLootTierData_Client");

    AddLootTierData(LootTierData);

    auto LootPackages = Playlist ? Playlist->LootPackages.Get() : nullptr;

    if (!LootPackages)
        LootPackages = FindObject<UDataTable>(L"/Game/Items/Datatables/AthenaLootPackages_Client.AthenaLootPackages_Client");

    AddLootPackageData(LootPackages);

    if (GameMode->GameFeatureLootTierData.Num() > 0)
    {
        for (auto& FeatureLootTierData : GameMode->GameFeatureLootTierData)
            AddLootTierData(FeatureLootTierData.Get());

        for (auto& FeatureLootPackageData : GameMode->GameFeatureLootPackageData)
            AddLootPackageData(FeatureLootPackageData.Get());
    }
    else if (auto GameFeaturesSubsystem = (UGameFeaturesSubsystem*)USubsystemBlueprintLibrary::GetEngineSubsystem(UGameFeaturesSubsystem::StaticClass()))
    {
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

            AddLootTierData(FeatureLootTierData);
            AddLootPackageData(FeatureLootPackageData);
        }
    }
    else
    {
        for (int i = 0; i < UObject::GObjects->Num(); i++)
        {
            auto Object = UObject::GObjects->GetByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA(UFortGameFeatureData::StaticClass()))
                continue;

            auto& LootTableData = ((UFortGameFeatureData*)Object)->DefaultLootTableData;

            AddLootTierData(LootTableData.LootTierData.Get());
            AddLootPackageData(LootTableData.LootPackageData.Get());
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
        if (VendingRarityWeightSum <= VendingRarityZeroWeight)
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
                auto RandomNum = (float)rand() / (RAND_MAX / VendingTotalWeight);

                for (auto& [ElementRarity, ElementWeight] : VendingRarityWeights)
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

    auto VendWobbleFinishedFunc = FindObject<UFunction>(L"/Game/Athena/Items/Gameplay/VendingMachine/B_Athena_VendingMachine.B_Athena_VendingMachine_C:VendWobble__FinishedFunc");

    if (VendWobbleFinishedFunc && VendWobbleFinishedFunc->ExecFunction != (UFunction::FNativeFuncPtr)VendWobble__FinishedFunc)
        ExecHook(VendWobbleFinishedFunc, VendWobble__FinishedFunc, VendWobble__FinishedFuncOG);
}

static void GetPhaseInfo(UObject* Context, FFrame& Stack, bool* Ret)
{
    FFortSafeZonePhaseInfo OutSafeZonePhaseTemp{};
    auto& OutSafeZonePhase = Stack.StepCompiledInRef(&OutSafeZonePhaseTemp);
    int32 InPhaseToGet;
    Stack.StepCompiledIn(&InPhaseToGet);
    Stack.IncrementCode();

    auto SafeZoneIndicator = (AFortSafeZoneIndicator*)Context;
    auto& SafeZonePhases = SafeZoneIndicator->SafeZonePhases;

    if (SafeZonePhases.IsValidIndex(InPhaseToGet))
    {
        memcpy((PBYTE)&OutSafeZonePhase, (PBYTE)&SafeZonePhases[InPhaseToGet], sizeof(FFortSafeZonePhaseInfo));

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

    if (Event.GamePhaseStep != Mutator->PhaseToGiveItems)
        return;

    for (auto& PlayerController : Mutator->CachedGameMode->AlivePlayers)
    {
        for (auto& Item : Mutator->ItemsToGive)
            GiveItem((AFortInventory*)PlayerController->WorldInventory.ObjectPointer, Item.ItemToDrop, (int)Item.NumberToGive.Evaluate());
    }
}

void AFortGameMode__Init()
{
    ConfiguredPlaylist = Playlist;
    bConfiguredLateGame = bLateGame;

    Hook(ImageBase + 0x8F95D80, InitGame, InitGameOG);
    Hook(ImageBase + 0x8F94F28, HandleMatchHasStarted, HandleMatchHasStartedOG);
    ExecHook(AGameMode::StaticClass()->FindFunction("ReadyToStartMatch"), ReadyToStartMatch, ReadyToStartMatchOG);
    Patch<uint16_t>(ImageBase + 0x30276D8, 0x9090);
    Hook(ImageBase + 0x8F91858, FinishWorldInitialization, FinishWorldInitializationOG);
    Hook(ImageBase + 0x2010398, GetServerWorldTimeSeconds);
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
    Hook<AFortGameSessionDedicatedAthena>(uint32(0xF7), Restart);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x12B), ShutdownDedicatedServer);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x134), OnMatchmakingSessionAssignMatchRequestV2);
    Hook(ImageBase + 0x9D19F18, OnRegisterPlayersComplete, OnRegisterPlayersCompleteOG);
    Hook(ImageBase + 0x9D19B70, OnQueryUserInfoComplete, OnQueryUserInfoCompleteOG);

    ExecHook(AFortSafeZoneIndicator::StaticClass()->FindFunction("GetPhaseInfo"), GetPhaseInfo);

    ExecHook(AFortAthenaMutator_GiveItemsAtGamePhaseStep::StaticClass()->FindFunction("OnGamePhaseStepChanged"), OnGamePhaseStepChanged);
}

static bool bFinishedFlight = false;
static bool bGettingReady = false;
static bool bBusUnlocked = false;
static bool bStartedForming = false;
static bool bFormedZone = false;
static bool bSafeZoneShrinking = false;

static AFortAthenaAircraft* GetActiveAircraft(AFortGameModeAthena* GameMode, UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic)
{
    if (GameMode->AlivePlayers.Num() <= 0 || GamePhaseLogic->Aircrafts_GameState.Num() <= 0)
        return nullptr;

    return GamePhaseLogic->Aircrafts_GameState.Data[0].Get();
}

static void UpdatePawnStormState(UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GamePhaseLogic, AFortPlayerPawn* Pawn)
{
    bool bInZone = GamePhaseLogic->IsInCurrentSafeZone(Pawn->K2_GetActorLocation(), false);

    if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
    {
        Pawn->bIsInAnyStorm = !bInZone;
        Pawn->OnRep_IsInAnyStorm();
        Pawn->bIsInsideSafeZone = bInZone;
        Pawn->OnRep_IsInsideSafeZone();
    }
}

void AFortGameMode__Tick()
{
    static auto GetPlayerBotsService = (UAthenaAIServicePlayerBots* (*)(UWorld*))(ImageBase + 0x8D45068);

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

    if (MatchmakingWaitStart && (bNavCacheAttached || bNavCacheDone || GetTickCount64() - MatchmakingWaitStart >= 600000))
    {
        auto GameSession = (AFortGameSessionDedicatedAthena*)UWorld::GetWorld()->AuthorityGameMode->GameSession;
        auto ConnectToMatchmakingServiceV2 = (bool (*)(AFortGameSessionDedicatedAthena*, bool))GameSession->VTable[0x138];

        MatchmakingWaitStart = 0;
        ConnectToMatchmakingServiceV2(GameSession, false);
    }

    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

    if (!GamePhaseLogic)
        return;

    auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

    if (!bSkipAircraft)
    {
        if (GamePhaseLogic->GamePhase <= EAthenaGamePhase::Warmup && !PauseWarmupMask)
        {
            if (!bGettingReady && GamePhaseLogic->GamePhase == EAthenaGamePhase::Warmup && GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->WarmupCountdownEndTime != -1 && GamePhaseLogic->WarmupCountdownEndTime - 10.f <= Time)
            {
                bGettingReady = true;

                SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::GetReady);
                return;
            }

            if (bGettingReady && GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->WarmupCountdownEndTime != -1 && GamePhaseLogic->WarmupCountdownEndTime < Time)
            {
                StartAircraftPhase(GamePhaseLogic);
                return;
            }
        }

        if (GamePhaseLogic->GamePhase == EAthenaGamePhase::Aircraft)
        {
            if (!bBusUnlocked)
            {
                auto Aircraft = GetActiveAircraft(GameMode, GamePhaseLogic);

                if (Aircraft && Aircraft->DropStartTime < Time)
                {
                    bBusUnlocked = true;

                    GamePhaseLogic->bAircraftIsLocked = false;
                    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::BusFlying);
                    return;
                }
            }

            if (!bStartedForming)
            {
                auto Aircraft = GetActiveAircraft(GameMode, GamePhaseLogic);

                if (Aircraft && Aircraft->DropEndTime != -1 && Aircraft->DropEndTime < Time)
                {
                    bStartedForming = true;

                    for (auto& Player : GameMode->AlivePlayers)
                    {
                        if (Player->IsInAircraft())
                            Player->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
                    }

                    if (auto PlayerBotsService = GetPlayerBotsService(UWorld::GetWorld()))
                        ForceAllExitAircraft(PlayerBotsService);

                    GamePhaseLogic->SafeZonesStartTime = bLateGame ? (float)Time : (float)Time + 60.f;

                    SetGamePhase(GamePhaseLogic, EAthenaGamePhase::SafeZones);
                    SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormForming);
                    return;
                }
            }
        }

        if (!bFinishedFlight)
        {
            auto Aircraft = GetActiveAircraft(GameMode, GamePhaseLogic);

            if (Aircraft && Aircraft->FlightEndTime != -1 && Aircraft->FlightEndTime < Time)
            {
                bFinishedFlight = true;

                Aircraft->K2_DestroyActor();
                GamePhaseLogic->Aircrafts_GameState.Clear();
                GamePhaseLogic->Aircrafts_GameMode.Clear();
                return;
            }
        }
    }
    else if (!bFinishedFlight)
    {
        bFinishedFlight = true;

        SetGamePhase(GamePhaseLogic, EAthenaGamePhase::SafeZones);
        SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormForming);
    }

    if (!bEnableZones || GamePhaseLogic->GamePhase != EAthenaGamePhase::SafeZones)
        return;

    if (bFinishedFlight && !bFormedZone && GameMode->AlivePlayers.Num() > 0 && GamePhaseLogic->SafeZonesStartTime != -1 && GamePhaseLogic->SafeZonesStartTime < Time)
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

                auto& SafeZonePhases = SafeZoneIndicator->SafeZonePhases;

                if (SafeZonePhases.IsValid())
                    SafeZonePhases.Free();

                const float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(GameState);

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

                    SafeZonePhases.Add(PhaseInfo);

                    SafeZoneIndicator->PhaseCount++;
                }

                SafeZoneIndicator->OnRep_PhaseCount();

                SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + SafeZonePhases.Data[0].WaitTime;
                SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + SafeZonePhases.Data[0].ShrinkTime;

                SafeZoneIndicator->CurrentPhase = 0;
                SafeZoneIndicator->OnRep_CurrentPhase();
            }

            GamePhaseLogic->SafeZoneIndicator = SafeZoneIndicator;
            GamePhaseLogic->OnRep_SafeZoneIndicator();
        }

        StartNewSafeZonePhase(GamePhaseLogic, bLateGame ? GetLateGamePhase((AFortGameStateAthena*)UWorld::GetWorld()->GameState) : 1);
        return;
    }

    auto SafeZoneIndicator = GamePhaseLogic->SafeZoneIndicator;

    if (!bFormedZone || !SafeZoneIndicator)
        return;

    if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase))
    {
        if (!bSafeZoneShrinking && SafeZoneIndicator->SafeZoneStartShrinkTime < Time)
        {
            bSafeZoneShrinking = true;

            SetSafeZoneState(SafeZoneIndicator, EFortSafeZoneState::Shrinking);
            SetGamePhaseStep(GamePhaseLogic, EAthenaGamePhaseStep::StormShrinking);
        }
        else if (SafeZoneIndicator->SafeZoneFinishShrinkTime < Time && SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase + 1))
        {
            StartNewSafeZonePhase(GamePhaseLogic, SafeZoneIndicator->CurrentPhase + 1);
            bSafeZoneShrinking = false;
        }
    }

    for (auto& Player : GameMode->AlivePlayers)
    {
        if (auto Pawn = Player->MyFortPawn)
            UpdatePawnStormState(GamePhaseLogic, Pawn);
    }

    TArray<AFortPlayerPawnAthena*> Pawns;
    GetAll<AFortPlayerPawnAthena>(Pawns);

    for (auto& Pawn : Pawns)
    {
        if (!Pawn || Pawn->bActorIsBeingDestroyed || !Pawn->Controller || Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
            continue;

        UpdatePawnStormState(GamePhaseLogic, Pawn);

        auto AbilitySystemComponent = Pawn->AbilitySystemComponent;

        if (AbilitySystemComponent && !Pawn->SafeZoneAppliedGE)
        {
            if (auto Effect = ApplyOutsideSafeZoneEffect(AbilitySystemComponent))
                Pawn->SafeZoneAppliedGE = (UClass*)Effect;
        }
    }

    Pawns.Free();
}

void AFortGameMode__Reset()
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);

    for (auto& Request : VoiceChatTokenRequests)
    {
        Request.ChannelName.Free();
        TSharedRef_Destructor(&Request.HttpRequest);
    }

    VoiceChatTokenRequests.clear();
    CurrentGameSessionId.clear();
    AbilitySets.Free();
    SafeZoneLocations.Free();
    LevelsToStream.Free();
    VendingRarityWeights.clear();
    BuildingClassMap.clear();

    VendingRarityWeightSum = 0;
    VendingRarityZeroWeight = 0;
    VendingTotalWeight = 0;
    PauseWarmupMask = 0;
    CalendarWaitStart = 0;
    CreateSessionCompleteHandle = 0;
    MatchmakingWaitStart = 0;
    LeaveMeshNetworkCompleteHandle = 0;

    bSkipWarmup = false;
    bSkipAircraft = false;
    bCalendarPlaylistDone = false;
    bFinishedFlight = false;
    bGettingReady = false;
    bBusUnlocked = false;
    bStartedForming = false;
    bFormedZone = false;
    bSafeZoneShrinking = false;
    bReady = false;

    Playlist = ConfiguredPlaylist;
    bLateGame = bConfiguredLateGame;

    SetConsoleTitleA("Remix 32.00 | Setting up");
}
