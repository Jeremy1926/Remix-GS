#include "pch.h"
#include "Shared.h"
#include "UFortQuestManager.h"
#include "SDK/SDK/FortniteGame_parameters.hpp"

INIT_POSTLOAD(UFortQuestManager);
INIT_TICKER(UFortQuestManager);

struct FFortQuestStatEvent
{
    EFortQuestObjectiveStatEvent Type;
    uint8 Pad_1[0x3];
    FWeakObjectPtr TargetObject;
    uint8 Pad_C[0x4];
    FGameplayTagContainer TargetTags;
    FGameplayTagContainer SourceTags;
    FGameplayTagContainer ContextTags;
    FDataTableRowHandle ObjectiveStat;
    FName ObjectiveBackendName;
    int32 Count;
    uint8 Pad_88[0x88];
};
static_assert(offsetof(FFortQuestStatEvent, TargetTags) == 0x10 && offsetof(FFortQuestStatEvent, ObjectiveStat) == 0x70 && offsetof(FFortQuestStatEvent, Count) == 0x84 && sizeof(FFortQuestStatEvent) == 0x110);

struct FFortQuestObjectiveCompletionEntry
{
    FString StatName;
    int32 Count;
    int32 TimestampOffset;
    uint8 Pad_18[0x8];
    TArray<FWeakObjectPtr> Quests;
};
static_assert(offsetof(FFortQuestObjectiveCompletionEntry, Quests) == 0x20 && sizeof(FFortQuestObjectiveCompletionEntry) == sizeof(FFortQuestObjectiveCompletion));

struct FFortQuestSaveRequest
{
    int32 SaveId;
    uint8 Pad_4[0x4];
    TArray<FFortQuestObjectiveCompletion> Changes;
    uint8 Pad_18[0x10];
};
static_assert(sizeof(FFortQuestSaveRequest) == 0x28);

struct FDelegateAllocation
{
    uint8* Data = nullptr;
    int32 Num = 0;
};

struct FQuestManagerFlushState
{
    FWeakObjectPtr QuestManager;
    FWeakObjectPtr PlayerController;
    std::unordered_map<int32, int32> ElapsedSeconds;
    double NextFlushTime = -1.0;
    int32 UpdateSaveId = -1;
    int32 EndSaveId = -1;
    bool bUpdateInFlight = false;
    bool bEnded = false;
};

static std::unordered_map<UFortQuestManager*, FQuestManagerFlushState> FlushStates;

static const char* QuestDiagReason = "";
static bool bQuestDiagHookEntered = false;

static void QuestLog(const char* Format, ...)
{
    static std::mutex Mutex;
    static HANDLE File = INVALID_HANDLE_VALUE;

    std::lock_guard<std::mutex> Lock(Mutex);

    if (File == INVALID_HANDLE_VALUE)
    {
        char Path[MAX_PATH]{};
        auto Length = GetEnvironmentVariableA("LOCALAPPDATA", Path, MAX_PATH);
        auto FilePath = std::string(Path, Length) + "\\FortniteGame\\Saved\\Logs\\RemixGS-Quests.log";
        File = CreateFileA(FilePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (File == INVALID_HANDLE_VALUE)
            return;
    }

    char Line[2048];
    SYSTEMTIME Time;
    GetLocalTime(&Time);
    auto Length = snprintf(Line, sizeof(Line), "[%02d:%02d:%02d.%03d] ", Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds);

    va_list Args;
    va_start(Args, Format);
    auto Written = vsnprintf(Line + Length, sizeof(Line) - Length - 2, Format, Args);
    va_end(Args);

    Length = Written < 0 ? Length : (std::min)((int)sizeof(Line) - 2, Length + Written);
    Line[Length++] = '\n';

    DWORD Bytes = 0;
    WriteFile(File, Line, (DWORD)Length, &Bytes, nullptr);
}

static std::string QuestDiagName(UObject* Object)
{
    return Object ? Object->GetName() : std::string("None");
}

static std::string QuestDiagPlayer(AActor* PlayerController)
{
    auto Controller = PlayerController && PlayerController->IsA(APlayerController::StaticClass()) ? (APlayerController*)PlayerController : nullptr;
    return Controller && Controller->PlayerState ? Controller->PlayerState->GetPlayerName().ToString() : QuestDiagName(PlayerController);
}

static FQuestManagerFlushState& GetFlushState(UFortQuestManager* QuestManager)
{
    auto& State = FlushStates[QuestManager];

    if (!State.QuestManager.IsValid())
        State = { FWeakObjectPtr(QuestManager) };

    return State;
}

static AFortPlayerControllerAthena* GetPlayerController(UFortQuestManager* QuestManager)
{
    static auto GetPlayerControllerInternal = (AController* (*)(UFortQuestManager*))(ImageBase + 0xA129990);

    auto PlayerController = GetPlayerControllerInternal(QuestManager);

    if (!PlayerController)
    {
        auto It = FlushStates.find(QuestManager);
        PlayerController = It != FlushStates.end() && It->second.QuestManager.Get() == QuestManager ? (AController*)It->second.PlayerController.Get() : nullptr;
    }

    return PlayerController && PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)PlayerController : nullptr;
}

static void LinkQuestManagerController(UFortQuestManager* QuestManager, AFortPlayerControllerAthena* PlayerController)
{
    static auto GetPlayerControllerInternal = (AController* (*)(UFortQuestManager*))(ImageBase + 0xA129990);

    if (!QuestManager || !PlayerController)
        return;

    auto& State = GetFlushState(QuestManager);

    if (State.PlayerController.Get() != PlayerController)
        State.PlayerController = FWeakObjectPtr(PlayerController);

    auto PlayerInfo = QuestManager->Outer;

    if (!GetPlayerControllerInternal(QuestManager) && PlayerInfo && PlayerInfo->IsA(UFortRegisteredPlayerInfo::StaticClass()))
        *(FWeakObjectPtr*)(__int64(PlayerInfo) + 0x21C) = FWeakObjectPtr(PlayerController);
}

static UFortPlaylistAthena* GetCurrentPlaylist()
{
    return ((AFortGameStateAthena*)UWorld::GetWorld()->GameState)->CurrentPlaylistInfo.BasePlaylist;
}

static UFortMcpProfileSubgame* GetBoundProfile(UFortQuestManager* QuestManager)
{
    return QuestManager->BoundProfile.Get();
}

static TArray<UFortQuestItem*> GetProfileQuests(UFortQuestManager* QuestManager)
{
    static auto GetQuestItems = (void (*)(UFortMcpProfileSubgame*, uint8, TArray<UFortQuestItem*>*))(ImageBase + 0x9D3C544);

    TArray<UFortQuestItem*> Quests;

    if (auto Profile = GetBoundProfile(QuestManager))
        GetQuestItems(Profile, 0, &Quests);

    return Quests;
}

static void ReapplyPendingChanges(UFortQuestManager* QuestManager)
{
    static auto ReapplyPendingChangesInternal = (void (*)(UFortQuestManager*))(ImageBase + 0xA132570);

    ReapplyPendingChangesInternal(QuestManager);
}

static void ScheduleFlush(UFortQuestManager* QuestManager)
{
    auto& State = GetFlushState(QuestManager);

    if (State.bEnded || State.NextFlushTime >= 0.0)
        return;

    State.NextFlushTime = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + QuestManager->DamageEventFlushDelaySeconds;
    QuestLog("schedule flush qm=%p at=%.1f delay=%.1f", QuestManager, State.NextFlushTime, QuestManager->DamageEventFlushDelaySeconds);
}

static bool DoesTagQueryMatch(const FGameplayTagQuery& TagQuery, const FGameplayTagContainer& Tags)
{
    return TagQuery.QueryTokenStream.Num() == 0 || UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(Tags, TagQuery);
}

static bool DoesLocationMatch(const FGameplayTagQuery& TagQuery, FFortQuestStatEvent* Event)
{
    return TagQuery.QueryTokenStream.Num() == 0 || UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(Event->SourceTags, TagQuery) ||
           UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(Event->TargetTags, TagQuery);
}

static bool IsInRange(const FInt32Range& Range, int32 Value)
{
    if ((Range.LowerBound.Type == ERangeBoundTypes::Inclusive && Value < Range.LowerBound.Value) || (Range.LowerBound.Type == ERangeBoundTypes::Exclusive && Value <= Range.LowerBound.Value))
        return false;

    if ((Range.UpperBound.Type == ERangeBoundTypes::Inclusive && Value > Range.UpperBound.Value) || (Range.UpperBound.Type == ERangeBoundTypes::Exclusive && Value >= Range.UpperBound.Value))
        return false;

    return true;
}

template <typename T>
static bool IsAllowed(const TArray<T>& Allowed, const TArray<T>& Prohibited, T Value)
{
    for (auto Entry : Prohibited)
    {
        if (Entry == Value)
            return false;
    }

    if (Allowed.Num() == 0)
        return true;

    for (auto Entry : Allowed)
    {
        if (Entry == Value)
            return true;
    }

    return false;
}

static EFortTeamAffiliation GetTeamAffiliation(AFortPlayerControllerAthena* PlayerController, UObject* TargetObject)
{
    auto TargetPawn = TargetObject && TargetObject->IsA(APawn::StaticClass()) ? (APawn*)TargetObject : nullptr;
    auto TargetPlayerState = TargetPawn && TargetPawn->PlayerState && TargetPawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)TargetPawn->PlayerState : nullptr;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    if (!TargetPlayerState || !PlayerState)
        return EFortTeamAffiliation::Neutral;

    return TargetPlayerState->TeamIndex == PlayerState->TeamIndex ? EFortTeamAffiliation::Friendly : EFortTeamAffiliation::Hostile;
}

static EEliminationKillType GetEliminationKillType(UObject* TargetObject)
{
    return TargetObject && TargetObject->IsA(AFortPlayerPawn::StaticClass()) ? EEliminationKillType::Player : EEliminationKillType::NPC;
}

static int32 EvaluateObjectiveFilter(const FInstancedStruct& Filter, UFortQuestManager* QuestManager, FName BackendName, AFortPlayerControllerAthena* PlayerController, FFortQuestStatEvent* Event, UObject* TargetObject)
{
    static auto PawnEliminated = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_PawnEliminated");
    static auto KillContribution = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_KillContribution");
    static auto Visit = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Visit");
    static auto DiscoverPOI = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_DiscoverPOI");
    static auto Land = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Land");
    static auto LandFromBus = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_LandFromBus");
    static auto Emote = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Emote");
    static auto Spray = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Spray");
    static auto Build = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Build");
    static auto AddToInventory = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_AddToInventory");
    static auto Acquire = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Acquire");
    static auto Interact = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_Interact");
    static auto RevivePlayer = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_RevivePlayer");
    static auto RevivedFromDBNO = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_RevivedFromDBNO");
    static auto StormPhaseChanged = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_StormPhaseChanged");
    static auto PlacementUpdate = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_PlacementUpdate");
    static auto MatchFinished = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_MatchFinished");

    static auto MatchTimeElapsed = FindObject<UScriptStruct>(L"/Script/FortniteGame.FortObjectiveFilter_MatchTimeElapsed");

    auto Struct = Filter.ScriptStruct;
    auto Memory = Filter.StructMemory;

    QuestDiagReason = "struct";

    if (!Struct || !Memory)
        return 0;

    UScriptStruct* ExpectedStructs[2] = {};

    switch (Event->Type)
    {
    case EFortQuestObjectiveStatEvent::Kill:
        ExpectedStructs[0] = PawnEliminated;
        break;
    case EFortQuestObjectiveStatEvent::KillContribution:
        ExpectedStructs[0] = KillContribution;
        break;
    case EFortQuestObjectiveStatEvent::Visit:
        ExpectedStructs[0] = Visit;
        break;
    case EFortQuestObjectiveStatEvent::VisitDiscoverPOI:
        ExpectedStructs[0] = DiscoverPOI;
        break;
    case EFortQuestObjectiveStatEvent::Land:
        ExpectedStructs[0] = Land;
        ExpectedStructs[1] = LandFromBus;
        break;
    case EFortQuestObjectiveStatEvent::Emote:
        ExpectedStructs[0] = Emote;
        break;
    case EFortQuestObjectiveStatEvent::Spray:
        ExpectedStructs[0] = Spray;
        break;
    case EFortQuestObjectiveStatEvent::Build:
        ExpectedStructs[0] = Build;
        break;
    case EFortQuestObjectiveStatEvent::Collect:
        ExpectedStructs[0] = AddToInventory;
        break;
    case EFortQuestObjectiveStatEvent::AthenaAcquire:
        ExpectedStructs[0] = Acquire;
        break;
    case EFortQuestObjectiveStatEvent::Interact:
        ExpectedStructs[0] = Interact;
        break;
    case EFortQuestObjectiveStatEvent::RevivePlayer:
        ExpectedStructs[0] = RevivePlayer;
        break;
    case EFortQuestObjectiveStatEvent::RevivedFromDBNO:
        ExpectedStructs[0] = RevivedFromDBNO;
        break;
    case EFortQuestObjectiveStatEvent::StormPhase:
        ExpectedStructs[0] = StormPhaseChanged;
        break;
    case EFortQuestObjectiveStatEvent::PlacementUpdate:
        ExpectedStructs[0] = PlacementUpdate;
        break;
    case EFortQuestObjectiveStatEvent::Finish:
        ExpectedStructs[0] = MatchFinished;
        break;
    case EFortQuestObjectiveStatEvent::TimeElapsed:
        ExpectedStructs[0] = MatchTimeElapsed;
        break;
    default:
        break;
    }

    if (!ExpectedStructs[0] || (Struct != ExpectedStructs[0] && Struct != ExpectedStructs[1]))
        return 0;

    auto ObjectiveFilter = (FObjectiveFilter*)Memory;

    QuestDiagReason = "author_progress";

    if (!ObjectiveFilter->Author.bCanGainProgressForThisObjective)
        return 0;

    QuestDiagReason = "author_query";

    if (!DoesTagQueryMatch(ObjectiveFilter->Author.TagQuery, Event->SourceTags))
        return 0;

    QuestDiagReason = "context_query";

    if (!DoesTagQueryMatch(ObjectiveFilter->Context.TagQuery, Event->ContextTags))
        return 0;

    QuestDiagReason = "type_check";

    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

    switch (Event->Type)
    {
    case EFortQuestObjectiveStatEvent::Kill:
    {
        if (Struct != PawnEliminated)
            return 0;

        auto Data = (FFortObjectiveFilter_PawnEliminated*)Memory;

        if (Data->bRequirePawnWasEliminatedByConvertedPawn || Data->bUseDistance || Data->bIncrementByDistance || Data->bUseKillStreak)
            return 0;

        if (!IsAllowed(Data->AllowedTeamAffiliations, Data->ProhibitedTeamAffiliations, GetTeamAffiliation(PlayerController, TargetObject)) ||
            !IsAllowed(Data->AllowedKilledPawnTypes, Data->ProhibitedKilledPawnTypes, GetEliminationKillType(TargetObject)))
            return 0;

        return DoesTagQueryMatch(Data->EliminatedPawn.TagQuery, Event->TargetTags) && DoesTagQueryMatch(Data->Killer.TagQuery, Event->SourceTags) &&
                       DoesTagQueryMatch(Data->EliminationSource.TagQuery, Event->SourceTags)
                   ? 1
                   : 0;
    }
    case EFortQuestObjectiveStatEvent::KillContribution:
    {
        if (Struct != KillContribution)
            return 0;

        auto Data = (FFortObjectiveFilter_KillContribution*)Memory;

        if (Data->bUseDamageContributed || Data->bUseDamageContributedPercent || Data->bUseDistanceFromEliminatedPawn)
            return 0;

        if (!IsAllowed(Data->AllowedTeamAffiliations, Data->ProhibitedTeamAffiliations, GetTeamAffiliation(PlayerController, TargetObject)))
            return 0;

        return DoesTagQueryMatch(Data->KilledActor.TagQuery, Event->TargetTags) && DoesTagQueryMatch(Data->KillerTags.TagQuery, Event->SourceTags) ? 1 : 0;
    }
    case EFortQuestObjectiveStatEvent::Visit:
        return Struct == Visit && DoesLocationMatch(((FFortObjectiveFilter_Visit*)Memory)->Location.TagQuery, Event) ? Event->Count : 0;
    case EFortQuestObjectiveStatEvent::VisitDiscoverPOI:
        return Struct == DiscoverPOI && DoesLocationMatch(((FFortObjectiveFilter_DiscoverPOI*)Memory)->PoiTags.TagQuery, Event) ? Event->Count : 0;
    case EFortQuestObjectiveStatEvent::Land:
    {
        if (Struct != Land && Struct != LandFromBus)
            return 0;

        auto Data = (FFortObjectiveFilter_Land*)Memory;

        if (Data->NearbyPlayersFilter.bFilterEnabled || Data->AllowedSurfaces.Num() > 0)
            return 0;

        if (Struct == LandFromBus && !(TargetObject && TargetObject->IsA(AFortPlayerPawn::StaticClass()) && ((AFortPlayerPawn*)TargetObject)->bIsSkydivingFromBus))
            return 0;

        return 1;
    }
    case EFortQuestObjectiveStatEvent::Emote:
    {
        if (Struct != Emote)
            return 0;

        auto Data = (FFortObjectiveFilter_Emote*)Memory;

        if (Data->NearbyPlayersFilter.bFilterEnabled || Data->Target.TagQuery.QueryTokenStream.Num() > 0)
            return 0;

        return DoesTagQueryMatch(Data->Emote.TagQuery, Event->TargetTags) ? 1 : 0;
    }
    case EFortQuestObjectiveStatEvent::Spray:
    {
        if (Struct != Spray)
            return 0;

        auto Data = (FFortObjectiveFilter_Spray*)Memory;

        return Data->Target.TagQuery.QueryTokenStream.Num() == 0 && DoesTagQueryMatch(Data->Spray.TagQuery, Event->TargetTags) ? 1 : 0;
    }
    case EFortQuestObjectiveStatEvent::Build:
    {
        if (Struct != Build)
            return 0;

        auto Data = (FFortObjectiveFilter_Build*)Memory;

        return !Data->bUseResourcesSpent && !Data->bIncrementByResourcesSpent && DoesTagQueryMatch(Data->Building.TagQuery, Event->TargetTags) ? 1 : 0;
    }
    case EFortQuestObjectiveStatEvent::Collect:
    {
        auto Data = (FFortObjectiveFilter_AddToInventory*)Memory;

        return DoesTagQueryMatch(Data->Item.TagQuery, Event->TargetTags) ? (Data->bIncrementByItemCount ? Event->Count : 1) : 0;
    }
    case EFortQuestObjectiveStatEvent::AthenaAcquire:
    {
        auto Data = (FFortObjectiveFilter_Acquire*)Memory;

        if ((Data->bUseItemCount && !IsInRange(Data->ItemCount, Event->Count)) || !DoesTagQueryMatch(Data->ItemAcquired.TagQuery, Event->TargetTags) ||
            !DoesTagQueryMatch(Data->AcquireSource.TagQuery, Event->SourceTags))
            return 0;

        switch (Data->IncrementType)
        {
        case EAcquireIncrementType::HighestMatchingItemCount:
        case EAcquireIncrementType::TotalMatchingItemCounts:
        case EAcquireIncrementType::TotalItemCount:
            return Event->Count;
        default:
            return 1;
        }
    }
    case EFortQuestObjectiveStatEvent::TimeElapsed:
    {
        auto Data = (FFortObjectiveFilter_MatchTimeElapsed*)Memory;
        auto UpdateTime = (int32)Data->UpdateTime;

        if (UpdateTime <= 0)
            return 0;

        auto& Elapsed = GetFlushState(QuestManager).ElapsedSeconds[BackendName.ComparisonIndex];
        Elapsed += Event->Count;

        auto Updates = Elapsed / UpdateTime;
        Elapsed -= Updates * UpdateTime;

        return Data->IncrementType == ETimeElapsedFilterIncrementType::TimeElapsed ? Updates * UpdateTime : Updates;
    }
    case EFortQuestObjectiveStatEvent::Interact:
        return Struct == Interact && DoesTagQueryMatch(((FFortObjectiveFilter_Interact*)Memory)->Target.TagQuery, Event->TargetTags) ? 1 : 0;
    case EFortQuestObjectiveStatEvent::RevivePlayer:
        return Struct == RevivePlayer && DoesTagQueryMatch(((FFortObjectiveFilter_RevivePlayer*)Memory)->RevivedPlayer.TagQuery, Event->TargetTags) ? 1 : 0;
    case EFortQuestObjectiveStatEvent::RevivedFromDBNO:
        return Struct == RevivedFromDBNO && DoesTagQueryMatch(((FFortObjectiveFilter_RevivedFromDBNO*)Memory)->RevivingPlayer.TagQuery, Event->TargetTags) ? 1 : 0;
    case EFortQuestObjectiveStatEvent::StormPhase:
    {
        if (Struct != StormPhaseChanged)
            return 0;

        auto Data = (FFortObjectiveFilter_StormPhaseChanged*)Memory;

        if (Data->StormStepRequirement == EStormStepVerbRequirement::OnlyStormShrinking)
            return 0;

        if (Data->bFilterByStormPhase)
        {
            auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

            if (!GamePhaseLogic || !GamePhaseLogic->SafeZoneIndicator || !IsInRange(Data->StormPhase, GamePhaseLogic->SafeZoneIndicator->CurrentPhase))
                return 0;
        }

        return 1;
    }
    case EFortQuestObjectiveStatEvent::PlacementUpdate:
    {
        if (Struct != PlacementUpdate || !GameState)
            return 0;

        auto Data = (FFortObjectiveFilter_PlacementUpdate*)Memory;
        bool bAuthorSetPlacement = TargetObject == PlayerController;

        if ((Data->PlacementAuthorSettings == EPlacementAuthorHandling::RequireAuthorSetPlacement && !bAuthorSetPlacement) ||
            (Data->PlacementAuthorSettings == EPlacementAuthorHandling::ProhibitAuthorSetPlacement && bAuthorSetPlacement))
            return 0;

        if ((Data->bUseCurrentPlacement && !IsInRange(Data->CurrentPlacement, GameState->TeamsLeft)) || (Data->bUsePlayersAlive && !IsInRange(Data->PlayersAlive, GameState->PlayersLeft)))
            return 0;

        return DoesTagQueryMatch(Data->PlayerThatSetPlacement.TagQuery, Event->TargetTags) ? 1 : 0;
    }
    case EFortQuestObjectiveStatEvent::Finish:
    {
        if (Struct != MatchFinished || !GameState || !PlayerState)
            return 0;

        auto Data = (FFortObjectiveFilter_MatchFinished*)Memory;

        if ((Data->bUseFinishingRankRange && !IsInRange(Data->FinishingRankRange, PlayerState->Place)) ||
            (Data->bUsePlayersOutLived && !IsInRange(Data->PlayersOutLived, (std::max)(GameState->TotalPlayers - PlayerState->Place, 0))))
            return 0;

        return DoesTagQueryMatch(Data->MatchMetaData.TagQuery, Event->ContextTags) ? 1 : 0;
    }
    default:
        return 0;
    }
}

static void AddPendingChange(UFortQuestManager* QuestManager, UFortQuestItem* Quest, FName BackendName, int32 Count, std::vector<int32>& CountedNames)
{
    auto& PendingChanges = *(TArray<FFortQuestObjectiveCompletionEntry>*)&QuestManager->PendingChanges;
    auto RawName = BackendName.ToString();
    auto StatName = std::wstring(RawName.begin(), RawName.end());
    FFortQuestObjectiveCompletionEntry* Entry = nullptr;

    for (auto& Change : PendingChanges)
    {
        if (Change.StatName.Num() > 0 && _wcsicmp(Change.StatName.CStr(), StatName.c_str()) == 0)
        {
            Entry = &Change;
            break;
        }
    }

    if (!Entry)
    {
        FFortQuestObjectiveCompletionEntry NewEntry{};
        NewEntry.StatName = FString(StatName.c_str());
        Entry = &PendingChanges.Add(NewEntry);
    }

    if (std::find(CountedNames.begin(), CountedNames.end(), BackendName.ComparisonIndex) == CountedNames.end())
    {
        Entry->Count += Count;
        CountedNames.push_back(BackendName.ComparisonIndex);
    }

    for (auto& QuestPtr : Entry->Quests)
    {
        if (QuestPtr.Get() == Quest)
            return;
    }

    Entry->Quests.Add(FWeakObjectPtr(Quest));
}

static bool ProcessStatEventForQuests(UFortQuestManager* QuestManager, AFortPlayerControllerAthena* PlayerController, FFortQuestStatEvent* Event, UObject* TargetObject)
{
    static auto HasCompletedQuest = (bool (*)(UFortQuestItem*))(ImageBase + 0xA111AB0);
    static auto IsObjectiveInStage = (bool (*)(FFortMcpQuestObjectiveInfo*, int32, UFortQuestItemDefinition*))(ImageBase + 0xA11360C);
    static auto DoesObjectiveStatHandleMatch = (bool (*)(FDataTableRowHandle*, FDataTableRowHandle*))(ImageBase + 0x2197B60);
    static auto DoesObjectiveMatchStatEvent = (bool (*)(FFortMcpQuestObjectiveInfo*, FFortQuestStatEvent*, FString*))(ImageBase + 0xA107868);

    std::vector<int32> CountedNames;
    FString ContextString;
    bool bChanged = false;
    bool bHasObjectiveStat = Event->ObjectiveStat.DataTable || !Event->ObjectiveStat.RowName.IsNone();
    int32 DiagQuests = 0;
    int32 DiagActive = 0;
    int32 DiagConsidered = 0;
    int32 DiagMatched = 0;
    int32 DiagLogged = 0;

    static auto GetObjectiveCompletionCount = (int32 (*)(UFortQuestItem*, FName*))(ImageBase + 0xA10B850);

    auto ProfileQuests = GetProfileQuests(QuestManager);

    for (auto Quest : ProfileQuests)
    {
        DiagQuests++;

        if (!Quest || HasCompletedQuest(Quest))
            continue;

        DiagActive++;

        auto Definition = (UFortQuestItemDefinition*)Quest->ItemDefinition;

        if (!Definition)
            continue;

        for (auto& Objective : Definition->Objectives)
        {
            if (!IsObjectiveInStage(&Objective, Quest->CurrentStage, Definition))
                continue;

            UFortQuestObjectiveInfo* ObjectiveInfo = nullptr;

            for (auto Info : Quest->Objectives)
            {
                if (Info && Info->BackendName == Objective.BackendName)
                {
                    ObjectiveInfo = Info;
                    break;
                }
            }

            auto BackendName = Objective.BackendName;
            auto AchievedCount = ObjectiveInfo ? ObjectiveInfo->AchievedCount : GetObjectiveCompletionCount(Quest, &BackendName);
            auto RequiredCount = ObjectiveInfo ? ObjectiveInfo->RequiredCount : Objective.Count;

            if (AchievedCount >= RequiredCount)
            {
                if (DiagLogged < 3 && Event->Type == EFortQuestObjectiveStatEvent::TimeElapsed)
                {
                    DiagLogged++;
                    QuestLog("  skip %s %s info=%p achieved=%d required=%d", QuestDiagName(Definition).c_str(), Objective.BackendName.ToString().c_str(), ObjectiveInfo, AchievedCount, RequiredCount);
                }

                continue;
            }

            DiagConsidered++;
            QuestDiagReason = "";

            int32 Increment = 0;

            if (Objective.ObjectiveFilters.Num() > 0)
            {
                for (auto& Filter : Objective.ObjectiveFilters)
                {
                    if ((Increment = EvaluateObjectiveFilter(Filter, QuestManager, Objective.BackendName, PlayerController, Event, TargetObject)) > 0)
                        break;
                }
            }
            else
            {
                bool bMatched = bHasObjectiveStat && Objective.InlineObjectiveStats.Num() <= 0 ? DoesObjectiveStatHandleMatch(&Objective.ObjectiveStatHandle, &Event->ObjectiveStat)
                                                                                              : DoesObjectiveMatchStatEvent(&Objective, Event, &ContextString);
                Increment = bMatched ? Event->Count : 0;
            }

            if (Increment <= 0)
            {
                if (DiagLogged < 3)
                {
                    DiagLogged++;
                    QuestLog("  nomatch %s %s filters=%d first=%s legacy=%d reason=%s achieved=%d/%d", QuestDiagName(Definition).c_str(), Objective.BackendName.ToString().c_str(),
                             Objective.ObjectiveFilters.Num(), Objective.ObjectiveFilters.Num() > 0 ? QuestDiagName(Objective.ObjectiveFilters[0].ScriptStruct).c_str() : "None",
                             Objective.InlineObjectiveStats.Num(), QuestDiagReason, AchievedCount, RequiredCount);
                }

                continue;
            }

            DiagMatched++;
            QuestLog("  match %s %s +%d", QuestDiagName(Definition).c_str(), Objective.BackendName.ToString().c_str(), Increment);

            AddPendingChange(QuestManager, Quest, Objective.BackendName, Increment, CountedNames);
            bChanged = true;
        }
    }

    QuestLog("  result type=%d quests=%d active=%d considered=%d matched=%d pending=%d", (int)Event->Type, DiagQuests, DiagActive, DiagConsidered, DiagMatched, QuestManager->PendingChanges.Num());

    ProfileQuests.Free();

    return bChanged;
}

static void CallMcpOperation(UFortQuestManager* QuestManager, UFortMcpProfile* Profile, UFunction* Function, void* Parms, FDedicatedServerUrlContext* Context, void (*OnComplete)(UFortQuestManager*, uint8*))
{
    static auto AllocateDelegateInstance = (uint8* (*)(int32, FDelegateAllocation*))(ImageBase + 0x26E76B0);
    static auto ConstructQuestManagerDelegateInstance = (void (*)(uint8*, UFortQuestManager*))(ImageBase + 0xA11C088);
    static auto DestroyDelegate = (void (*)(FDelegateAllocation*))(ImageBase + 0x1C1D0C0);
    static auto ConstructDedicatedServerUrlContext = (void (*)(FDedicatedServerUrlContext*, FDelegateAllocation*))(ImageBase + 0x4982030);
    static auto DestroyUrlContext = (void (*)(FDedicatedServerUrlContext*))(ImageBase + 0x4A430D4);

    FDelegateAllocation OnCompleteDelegate{};

    if (auto Instance = AllocateDelegateInstance(40, &OnCompleteDelegate))
    {
        ConstructQuestManagerDelegateInstance(Instance, QuestManager);
        *(void**)(Instance + 32) = (void*)OnComplete;
    }

    ConstructDedicatedServerUrlContext(Context, &OnCompleteDelegate);
    DestroyDelegate(&OnCompleteDelegate);

    auto Flags = Function->FunctionFlags;
    Function->FunctionFlags |= 0x400;
    Profile->ProcessEvent(Function, Parms);
    Function->FunctionFlags = Flags;

    DestroyUrlContext(Context);
}

static void HandleQuestUpdate(UFortQuestManager* QuestManager, uint8* Result, int32 SaveId)
{
    static auto HandleQuestUpdateInternal = (void (*)(UFortQuestManager*, uint8*, int32))(ImageBase + 0xA12A8F8);

    HandleQuestUpdateInternal(QuestManager, Result, SaveId);
}

static void OnUpdateQuestsComplete(UFortQuestManager* QuestManager, uint8* Result)
{
    auto It = FlushStates.find(QuestManager);

    if (It == FlushStates.end())
        return;

    auto& State = It->second;

    QuestLog("UpdateQuests complete qm=%p success=%d retry=%d http=%d saveid=%d", QuestManager, Result[0], Result[56], *(int32*)(Result + 144), State.UpdateSaveId);

    HandleQuestUpdate(QuestManager, Result, State.UpdateSaveId);
    State.UpdateSaveId = -1;
    State.bUpdateInFlight = false;

    ReapplyPendingChanges(QuestManager);

    if (Result[0] && QuestManager->PendingChanges.Num() > 0)
        ScheduleFlush(QuestManager);
}

static void OnEndBattleRoyaleGameComplete(UFortQuestManager* QuestManager, uint8* Result)
{
    auto It = FlushStates.find(QuestManager);

    if (It == FlushStates.end())
        return;

    QuestLog("EndBattleRoyaleGameV2 complete qm=%p success=%d http=%d saveid=%d", QuestManager, Result[0], *(int32*)(Result + 144), It->second.EndSaveId);

    HandleQuestUpdate(QuestManager, Result, It->second.EndSaveId);
    It->second.EndSaveId = -1;

    auto PlayerController = GetPlayerController(QuestManager);
    auto Profile = GetBoundProfile(QuestManager);

    if (Result[0] && PlayerController && PlayerController->XPComponent && Profile)
        PlayerController->XPComponent->HasBRMatchReportCompletedProfileVer = Profile->ProfileRevision;
}

static void SendUpdateQuests(UFortQuestManager* QuestManager, FQuestManagerFlushState& State)
{
    static auto ProcessPendingStatEvents = (FFortQuestSaveRequest* (*)(UFortQuestManager*))(ImageBase + 0xA12E178);
    static auto UpdateQuests = UFortMcpProfileSubgame::StaticClass()->FindFunction("UpdateQuests");

    auto Profile = GetBoundProfile(QuestManager);

    QuestLog("flush qm=%p profile=%p function=%p pending=%d", QuestManager, Profile, UpdateQuests, QuestManager->PendingChanges.Num());

    if (!Profile || !UpdateQuests)
        return;

    auto SaveRequest = ProcessPendingStatEvents(QuestManager);

    QuestLog("  saverequest=%p saveid=%d entries=%d", SaveRequest, SaveRequest ? SaveRequest->SaveId : -1, SaveRequest ? SaveRequest->Changes.Num() : 0);

    if (!SaveRequest)
        return;

    Params::FortMcpProfileSubgame_UpdateQuests Parms{};
    Parms.Advance = SaveRequest->Changes;

    for (auto& Change : SaveRequest->Changes)
        QuestLog("  advance %s %d", Change.StatName.ToString().c_str(), Change.Count);

    State.UpdateSaveId = SaveRequest->SaveId;
    State.bUpdateInFlight = true;

    CallMcpOperation(QuestManager, Profile, UpdateQuests, &Parms, &Parms.Context, OnUpdateQuestsComplete);
}

static std::wstring GetMatchId()
{
    static std::wstring MatchId;

    if (!MatchId.empty())
        return MatchId;

    auto GameState = (AFortGameState*)UWorld::GetWorld()->GameState;

    if (GameState && GameState->GameSessionId.Num() > 1)
        MatchId = GameState->GameSessionId.ToWString();
    else
    {
        auto GuidString = UKismetGuidLibrary::Conv_GuidToString(UKismetGuidLibrary::NewGuid());
        MatchId = GuidString.ToWString();
        GuidString.Free();
    }

    return MatchId;
}

void EndBattleRoyaleGame(AFortPlayerControllerAthena* PlayerController)
{
    static auto ProcessPendingStatEvents = (FFortQuestSaveRequest* (*)(UFortQuestManager*))(ImageBase + 0xA12E178);
    static auto EndBattleRoyaleGameV2 = UFortMcpProfileAthena::StaticClass()->FindFunction("EndBattleRoyaleGameV2");

    auto QuestManager = PlayerController ? PlayerController->GetQuestManager(ESubGame::Athena) : nullptr;

    if (!QuestManager || GetFlushState(QuestManager).bEnded)
        return;

    auto Profile = GetBoundProfile(QuestManager);

    if (!Profile || !Profile->IsA(UFortMcpProfileAthena::StaticClass()) || !EndBattleRoyaleGameV2)
        return;

    SendStatEvent(QuestManager, PlayerController, EFortQuestObjectiveStatEvent::Finish, 1, true);

    auto& State = GetFlushState(QuestManager);
    State.bEnded = true;
    State.NextFlushTime = -1.0;

    auto SaveRequest = ProcessPendingStatEvents(QuestManager);
    auto Playlist = GetCurrentPlaylist();
    auto RawPlaylistName = Playlist ? Playlist->PlaylistName.ToString() : std::string();
    auto PlaylistName = std::wstring(RawPlaylistName.begin(), RawPlaylistName.end());
    auto MatchId = GetMatchId();

    Params::FortMcpProfileAthena_EndBattleRoyaleGameV2 Parms{};

    if (SaveRequest)
    {
        Parms.Advance = SaveRequest->Changes;
        State.EndSaveId = SaveRequest->SaveId;
    }

    Parms.PlaylistId = FString(PlaylistName.c_str());
    Parms.MatchStats.MatchID = FString(MatchId.c_str());
    Parms.TotalXPAccum = PlayerController->XPComponent ? PlayerController->XPComponent->TotalXpEarned : 0;

    QuestLog("EndBattleRoyaleGameV2 player=%s qm=%p profile=%p saveid=%d entries=%d playlist=%ls match=%ls xp=%d", QuestDiagPlayer(PlayerController).c_str(), QuestManager, Profile, State.EndSaveId,
             Parms.Advance.Num(), PlaylistName.c_str(), MatchId.c_str(), Parms.TotalXPAccum);

    CallMcpOperation(QuestManager, Profile, EndBattleRoyaleGameV2, &Parms, &Parms.Context, OnEndBattleRoyaleGameComplete);

    Parms.PlaylistId.Free();
    Parms.MatchStats.MatchID.Free();
}

void SendPlacementUpdate(AFortPlayerControllerAthena* Eliminator)
{
    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

    for (auto PlayerController : GameMode->AlivePlayers)
    {
        if (PlayerController)
            SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::PlacementUpdate, 1, true, PlayerController == Eliminator ? PlayerController : nullptr);
    }
}

static void BindScriptDelegate(TArray<FScriptDelegate>& InvocationList, UObject* Object, FName FunctionName)
{
    for (auto& Delegate : InvocationList)
    {
        if (Delegate.Object.Get() == Object && Delegate.FunctionName == FunctionName)
            return;
    }

    FScriptDelegate Delegate{};
    Delegate.Object = FWeakObjectPtr(Object);
    Delegate.FunctionName = FunctionName;
    InvocationList.Add(Delegate);
}

void RegisterWithQuestManager(AFortPlayerControllerAthena* PlayerController)
{
    static auto OnXPEvent = FName(L"OnXPEvent");
    static auto OnXpUpdated = FName(L"OnXpUpdated");

    static auto GetPlayerControllerInternal = (AController* (*)(UFortQuestManager*))(ImageBase + 0xA129990);
    static auto HasCompletedQuest = (bool (*)(UFortQuestItem*))(ImageBase + 0xA111AB0);

    auto XPComponent = PlayerController->XPComponent;
    auto QuestManager = PlayerController->GetQuestManager(ESubGame::Athena);

    int32 DiagActive = 0;

    for (int32 i = 0; QuestManager && i < QuestManager->CurrentQuests.Num(); i++)
    {
        if (QuestManager->CurrentQuests[i] && !HasCompletedQuest(QuestManager->CurrentQuests[i]))
            DiagActive++;
    }

    QuestLog("register player=%s pc=%p xp=%p qm=%p outer=%s gamelink=%p profile=%p quests=%d active=%d components=%d", QuestDiagPlayer(PlayerController).c_str(), PlayerController, XPComponent,
             QuestManager, QuestManager ? QuestDiagName(QuestManager->Outer).c_str() : "None", QuestManager ? GetPlayerControllerInternal(QuestManager) : nullptr,
             QuestManager ? GetBoundProfile(QuestManager) : nullptr, QuestManager ? QuestManager->CurrentQuests.Num() : -1, DiagActive, QuestManager ? QuestManager->Components.Num() : -1);

    if (!XPComponent)
        return;

    LinkQuestManagerController(QuestManager, PlayerController);

    QuestLog("  link after=%p", QuestManager ? GetPlayerControllerInternal(QuestManager) : nullptr);

    if (QuestManager)
    {
        auto ProfileQuests = GetProfileQuests(QuestManager);
        int32 DiagResolved = 0;
        int32 DiagFilters = 0;
        int32 DiagFailed = 0;

        for (auto Quest : ProfileQuests)
        {
            auto Definition = Quest ? (UFortQuestItemDefinition*)Quest->ItemDefinition : nullptr;

            if (!Definition)
            {
                if (DiagFailed++ < 10)
                    QuestLog("  unresolved quest item %s", QuestDiagName(Quest).c_str());

                continue;
            }

            DiagResolved++;

            if (Definition->Objectives.Num() > 0 && Definition->Objectives[0].ObjectiveFilters.Num() > 0)
                DiagFilters++;
        }

        QuestLog("  profile quests=%d resolved=%d qs2=%d unresolved=%d currentquests=%d", ProfileQuests.Num(), DiagResolved, DiagFilters, DiagFailed, QuestManager->CurrentQuests.Num());

        ProfileQuests.Free();
    }

    for (int32 i = 0; QuestManager && i < QuestManager->Components.Num(); i++)
    {
        auto Component = QuestManager->Components[i];

        if (Component && Component->IsA(UFortQuestManagerComponent_Athena::StaticClass()))
        {
            BindScriptDelegate(*(TArray<FScriptDelegate>*)(__int64(Component) + 0x3E8), XPComponent, OnXPEvent);
            BindScriptDelegate(*(TArray<FScriptDelegate>*)(__int64(Component) + 0x3F8), XPComponent, OnXpUpdated);
            break;
        }
    }

    XPComponent->bRegisteredWithQuestManager = true;
    XPComponent->OnRep_bRegisteredWithQuestManager();
}

void SendStatEvent(UFortQuestManager* QuestManager, AActor* PlayerController, EFortQuestObjectiveStatEvent StatEvent, int32 Count, bool bAllowQueueStatEvent, UObject* TargetObject, FGameplayTagContainer TargetTags,
                   FGameplayTagContainer AdditionalSourceTags)
{
    static auto SendStatEventWithTags = (void (*)(UFortQuestManager*, EFortQuestObjectiveStatEvent, UObject*, FGameplayTagContainer*, FGameplayTagContainer*, FGameplayTagContainer*, int32))(ImageBase + 0xA131518);

    if (!bAllowQueueStatEvent || !QuestManager)
        return;

    if (PlayerController && PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()))
        LinkQuestManagerController(QuestManager, (AFortPlayerControllerAthena*)PlayerController);

    FGameplayTagContainer SourceTags{};
    FGameplayTagContainer ContextTags{};

    QuestManager->GetSourceAndContextTags(&SourceTags, &ContextTags);
    SourceTags.AppendTags(AdditionalSourceTags);

    bQuestDiagHookEntered = false;

    SendStatEventWithTags(QuestManager, StatEvent, TargetObject, &TargetTags, &SourceTags, &ContextTags, Count);

    QuestLog("SendStatEvent player=%s qm=%p type=%d count=%d target=%s source=%d context=%d hook=%d", QuestDiagPlayer(PlayerController).c_str(), QuestManager, (int)StatEvent, Count,
             QuestDiagName(TargetObject).c_str(), SourceTags.GameplayTags.Num(), ContextTags.GameplayTags.Num(), bQuestDiagHookEntered);

    SourceTags.GameplayTags.Free();
    SourceTags.ParentTags.Free();
    ContextTags.GameplayTags.Free();
    ContextTags.ParentTags.Free();
}

static void QueueStatEvent(UFortQuestManager* QuestManager, EFortQuestObjectiveStatEvent InType, UObject* InTargetObject, FGameplayTagContainer* InTargetTags, FGameplayTagContainer* InSourceTags,
                           FGameplayTagContainer* InContextTags, FDataTableRowHandle* InObjectiveStat, FName InObjectiveBackendName, int32 InCount)
{
    static auto ConstructStatEvent = (FFortQuestStatEvent* (*)(FFortQuestStatEvent*, EFortQuestObjectiveStatEvent, UObject*, FGameplayTagContainer*, FGameplayTagContainer*, FGameplayTagContainer*,
                                                               FDataTableRowHandle*, FName, int32, FGuid*))(ImageBase + 0xA190B9C);
    static auto DestroyStatEvent = (void (*)(FFortQuestStatEvent*))(ImageBase + 0x9060550);
    static auto BroadcastStatEvent = (void (*)(void*, FFortQuestStatEvent*, void*))(ImageBase + 0x59E03D4);

    auto PlayerController = GetPlayerController(QuestManager);

    bQuestDiagHookEntered = true;

    QuestLog("QueueStatEvent qm=%p player=%s type=%d count=%d target=%s targettags=%d sourcetags=%d contexttags=%d", QuestManager, QuestDiagPlayer(PlayerController).c_str(), (int)InType, InCount,
             QuestDiagName(InTargetObject).c_str(), InTargetTags ? InTargetTags->GameplayTags.Num() : -1, InSourceTags ? InSourceTags->GameplayTags.Num() : -1,
             InContextTags ? InContextTags->GameplayTags.Num() : -1);

    if (!PlayerController)
        return;

    if (auto Playlist = GetCurrentPlaylist())
        InContextTags->AppendTags(Playlist->GameplayTagContainer);

    FDataTableRowHandle EmptyObjectiveStat{};
    FGuid EventId{};
    alignas(16) uint8 EventStorage[sizeof(FFortQuestStatEvent)];

    auto Event = ConstructStatEvent((FFortQuestStatEvent*)EventStorage, InType, InTargetObject, InTargetTags, InSourceTags, InContextTags, InObjectiveStat ? InObjectiveStat : &EmptyObjectiveStat,
                                    InObjectiveBackendName, InCount, &EventId);

    BroadcastStatEvent((void*)(__int64(QuestManager) + 0x140), Event, nullptr);

    if (ProcessStatEventForQuests(QuestManager, PlayerController, Event, InTargetObject))
    {
        ReapplyPendingChanges(QuestManager);
        ScheduleFlush(QuestManager);
    }

    DestroyStatEvent(Event);
}

static void SendTimeElapsed(double Now)
{
    static double LastTime = -1.0;

    if (LastTime < 0.0 || Now < LastTime)
    {
        LastTime = Now;
        return;
    }

    auto Seconds = (int32)(Now - LastTime);

    if (Seconds <= 0)
        return;

    LastTime += Seconds;

    auto World = UWorld::GetWorld();
    auto GameMode = (AFortGameModeAthena*)World->AuthorityGameMode;
    auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(World);

    static int32 DiagLastPhase = -1;
    int32 DiagPhase = GamePhaseLogic ? (int32)GamePhaseLogic->GamePhase : -1;

    if (DiagPhase != DiagLastPhase)
    {
        DiagLastPhase = DiagPhase;
        QuestLog("time elapsed phase=%d gamemode=%p alive=%d", DiagPhase, GameMode, GameMode && GameMode->IsA(AFortGameModeAthena::StaticClass()) ? GameMode->AlivePlayers.Num() : -1);
    }

    if (!GameMode || !GameMode->IsA(AFortGameModeAthena::StaticClass()) || !GamePhaseLogic || GamePhaseLogic->GamePhase < EAthenaGamePhase::Aircraft ||
        GamePhaseLogic->GamePhase >= EAthenaGamePhase::EndGame)
        return;

    QuestLog("time elapsed fire seconds=%d alive=%d", Seconds, GameMode->AlivePlayers.Num());

    for (auto PlayerController : GameMode->AlivePlayers)
    {
        if (PlayerController)
            SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::TimeElapsed, Seconds, true);
    }
}

void UFortQuestManager__Tick()
{
    auto World = UWorld::GetWorld();

    if (!World)
        return;

    auto Now = UGameplayStatics::GetTimeSeconds(World);

    static bool bDiagHookChecked = false;

    if (!bDiagHookChecked)
    {
        bDiagHookChecked = true;
        QuestLog("tick first run hookbyte=%02X now=%.1f", *(uint8*)(ImageBase + 0xA12E4E8), Now);
    }

    SendTimeElapsed(Now);

    for (auto It = FlushStates.begin(); It != FlushStates.end();)
    {
        auto& State = It->second;

        if (!State.QuestManager.IsValid())
        {
            It = FlushStates.erase(It);
            continue;
        }

        if (!State.bEnded && !State.bUpdateInFlight && State.NextFlushTime >= 0.0 && Now >= State.NextFlushTime)
        {
            State.NextFlushTime = -1.0;
            SendUpdateQuests(It->first, State);
        }

        ++It;
    }
}

void UFortQuestManager__PostLoad()
{
    auto Status = MH_CreateHook((LPVOID)(ImageBase + 0xA12E4E8), (LPVOID)QueueStatEvent, nullptr);

    QuestLog("init imagebase=%p queuestatevent=%p status=%d qs1disabled=%d", (void*)ImageBase, (void*)(ImageBase + 0xA12E4E8), (int)Status, **(uint8**)(ImageBase + 0x12D3EE50));

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"fort.DisablePartialQS1InNonCampaign 0", nullptr);

    if (auto DisablePartialQS1InNonCampaign = *(uint8**)(ImageBase + 0x12D3EE50))
        *DisablePartialQS1InNonCampaign = 0;

    QuestLog("init qs1disabled after=%d", **(uint8**)(ImageBase + 0x12D3EE50));
}
