#include "pch.h"
#include "Shared.h"
#include "FortniteGame/Source/FortniteGame/Public/Online/FortGameSessionDedicatedAthena.h"
#include "Engine/Source/Runtime/Engine/Public/TimerManager.h"
#include "FortniteGame/Source/FortniteAI/Public/AI/Navigation/AthenaNavSystem.h"
#include "FortniteGame/Source/FortniteGame/Public/Athena/ControllerComponents/FortControllerComponent_TournamentStatsTracker.h"

INIT_POSTLOAD(AFortGameSessionDedicatedAthena);
INIT_TICKER(AFortGameSessionDedicatedAthena);

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

struct FMatchmakingBackfill
{
    FString BackfillId;
    TArray<TArray<TArray<FSharedRef>>> Teams;
    FSharedRef BackfillOptions;
};

static uint64 CreateSessionCompleteHandle = 0;
static ULONGLONG MatchmakingWaitStart = 0;
static bool bMatchmakingServiceLost = false;
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

FSharedRef GetSessionInterface()
{
    static auto GetSessionInterfaceInternal = (FSharedRef* (*)(FSharedRef*, UWorld*, FName))(ImageBase + 0x71D8BA0);

    FSharedRef SessionInterface{};
    GetSessionInterfaceInternal(&SessionInterface, UWorld::GetWorld(), FName());
    return SessionInterface;
}

FOnlineSessionSettings* GetSessionSettings(const FSharedRef& SessionInterface, FName SessionName)
{
    auto GetSessionSettingsInternal = (FOnlineSessionSettings* (*)(void*, FName))(*(void***)SessionInterface.Object)[0x70];

    return GetSessionSettingsInternal(SessionInterface.Object, SessionName);
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

static void ReadPlayerDivisions(void* Options, TArray<TArray<TArray<FSharedRef>>>& Teams)
{
    static auto HasTypedField = (bool (*)(void*, const FJsonKey*, int32))(ImageBase + 0x3BD6E48);
    static auto GetObjectField = (FSharedRef* (*)(void*, const FJsonKey*))(ImageBase + 0x2BECAF0);
    static auto TryGetStringField = (bool (*)(void*, const FJsonKey*, FString*))(ImageBase + 0x1D8F394);

    auto DivisionsKey = JsonKey(L"match.option.player.divisions");

    if (!Options || !HasTypedField(Options, &DivisionsKey, 6))
        return;

    auto Divisions = GetObjectField(Options, &DivisionsKey)->Object;

    for (auto& Team : Teams)
    {
        for (auto& Party : Team)
        {
            for (auto& Player : Party)
            {
                FString PlayerIdString;
                FString Division;
                auto ToString = (FString* (*)(void*, FString*))(*(void***)Player.Object)[0x6];

                ToString(Player.Object, &PlayerIdString);

                auto PlayerKey = JsonKey(PlayerIdString);

                if (TryGetStringField(Divisions, &PlayerKey, &Division) && Division.Num() > 1)
                    TournamentPlayerDivision(PlayerIdString.Data, Division.Data);

                PlayerIdString.Free();
                Division.Free();
            }
        }
    }
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

                    if (GameMode)
                    {
                        AssignString((FString*)(__int64(GameMode) + 0xE78), TournamentId.Data, JsonKey(TournamentId).Len);
                        AssignString((FString*)(__int64(GameMode) + 0xE88), WindowId.Data, JsonKey(WindowId).Len);
                    }
                }

                TournamentId.Free();
                WindowId.Free();
                ReadPlayerDivisions(MatchOptions, MatchAssignment.Teams);

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
    static auto PauseBeaconRequests = (void (*)(AFortGameSession*, bool))(ImageBase + 0x9CE6B40);
    static auto SetClientsReadyToJoinPhase = (void (*)(AContentBeaconHostObjectV2*))(ImageBase + 0x9CEB2A8);

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

            PauseBeaconRequests(GameSession, false);

            if (auto ContentBeaconHost = GameSession->ContentBeaconHostV2; ContentBeaconHost && ContentBeaconHost->IsA(AFortContentBeaconHostObjectV2::StaticClass()))
                SetClientsReadyToJoinPhase(ContentBeaconHost);

            RespondToMatchAssignment(MatchmakingService, 0);
        }
    }

    RemoveString(RegisterPlayerResults, BackfillId);
    RemoveString(QueryUserInfoResults, BackfillId);
}

static void (*OnMatchmakingSessionBackfillMatchRequestV2OG)(AFortGameSessionDedicatedAthena*, FMatchmakingBackfill*);
static void OnMatchmakingSessionBackfillMatchRequestV2(AFortGameSessionDedicatedAthena* GameSession, FMatchmakingBackfill* Backfill)
{
    ReadPlayerDivisions(Backfill->BackfillOptions.Object, Backfill->Teams);
    OnMatchmakingSessionBackfillMatchRequestV2OG(GameSession, Backfill);
}

static void (*OnMatchmakingSessionErrorV2OG)(AFortGameSessionDedicatedAthena*, void*);
static void OnMatchmakingSessionErrorV2(AFortGameSessionDedicatedAthena* GameSession, void* Error)
{
    OnMatchmakingSessionErrorV2OG(GameSession, Error);
    bMatchmakingServiceLost = true;
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

void RegisterServer(AGameSession* GameSession)
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

bool HasGameSession()
{
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);

    auto SessionInterface = GetSessionInterface();
    auto bHasGameSession = SessionInterface.Object && GetSessionSettings(SessionInterface, GetGameSessionName());

    TSharedRef_Destructor(&SessionInterface);

    return bHasGameSession;
}

void OnDestroySessionComplete(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, bool bWasSuccessful)
{
    auto Base = __int64(GameSession);

    if (*(bool*)(Base + 0xAD8))
        ClearTimer(GameSession, *(uint64*)(Base + 0xAA8));

    RegisterServer(GameSession);
}

void CleanupDedicatedServerSession(AGameSession* GameSession, void* OnComplete)
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
    {
        Patch<uint8_t>(ImageBase + 0x41D939C, 0x4C);
        RequestExitWithStatus(true, uint8(ShutdownExitCode), nullptr);
    }
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

static void Restart(AFortGameSessionDedicatedAthena* GameSession)
{
    ShutdownDedicatedServer(GameSession, *(bool*)(ImageBase + 0x12C11FE4) ? *(int32*)(ImageBase + 0x12C11FE0) : 0);
}

void AFortGameSessionDedicatedAthena__PostLoad()
{
    Hook<AFortGameSessionDedicatedAthena>(uint32(0xF7), Restart);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x12B), ShutdownDedicatedServer);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x134), OnMatchmakingSessionAssignMatchRequestV2);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x135), OnMatchmakingSessionBackfillMatchRequestV2, OnMatchmakingSessionBackfillMatchRequestV2OG);
    Hook<AFortGameSessionDedicatedAthena>(uint32(0x136), OnMatchmakingSessionErrorV2, OnMatchmakingSessionErrorV2OG);
    Hook(ImageBase + 0x9D19F18, OnRegisterPlayersComplete, OnRegisterPlayersCompleteOG);
    Hook(ImageBase + 0x9D19B70, OnQueryUserInfoComplete, OnQueryUserInfoCompleteOG);
}

void AFortGameSessionDedicatedAthena__Tick()
{
    if (MatchmakingWaitStart && (bNavCacheAttached || bNavCacheDone || GetTickCount64() - MatchmakingWaitStart >= 1800000))
    {
        auto GameSession = (AFortGameSessionDedicatedAthena*)UWorld::GetWorld()->AuthorityGameMode->GameSession;
        auto ConnectToMatchmakingServiceV2 = (bool (*)(AFortGameSessionDedicatedAthena*, bool))GameSession->VTable[0x138];

        MatchmakingWaitStart = 0;
        ConnectToMatchmakingServiceV2(GameSession, false);
    }

    if (bMatchmakingServiceLost)
    {
        auto GameSession = UWorld::GetWorld()->AuthorityGameMode->GameSession;
        auto RequestGracefulShutdown = (void (*)(AGameSession*, int32))GameSession->VTable[0x12C];

        bMatchmakingServiceLost = false;
        SetConsoleTitleA("Remix 32.00 | Lost the matchmaker, stopping");
        RequestGracefulShutdown(GameSession, 0);
    }
}
