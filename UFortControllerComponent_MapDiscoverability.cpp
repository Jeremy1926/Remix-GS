#include "pch.h"
#include "Shared.h"

INIT_MODULE(UFortControllerComponent_MapDiscoverability);

struct FDiscoverablePOI
{
    FGameplayTag POITag;
    UFortQuestItemDefinition* QuestPartOf;
    FName QuestObjectiveBackendName;
};

static std::vector<FDiscoverablePOI>& GetDiscoverablePOIs(AFortPoiManager* PoiManager)
{
    static auto MatchesTag = (bool (*)(const FGameplayTag*, const FGameplayTag*))(ImageBase + 0x1D74320);
    static auto POITag = FGameplayTag{ FName(L"Athena.Location.POI") };
    static auto UnNamedPOITag = FGameplayTag{ FName(L"Athena.Location.UnNamedPOI") };
    static std::vector<FDiscoverablePOI> DiscoverablePOIs;
    static TWeakObjectPtr<AFortPoiManager> CachedPoiManager;
    static int32 CachedVolumes = -1;

    auto Volumes = PoiManager->SmallTickUpdatedPoi.Num() + PoiManager->LargePoiVolumes.Num() + PoiManager->PendingLargePoiVolumes.Num();

    if (CachedPoiManager.Get() == PoiManager && CachedVolumes == Volumes)
        return DiscoverablePOIs;

    CachedPoiManager = TWeakObjectPtr<AFortPoiManager>(PoiManager);
    CachedVolumes = Volumes;
    DiscoverablePOIs.clear();

    for (auto List : { &PoiManager->SmallTickUpdatedPoi, &PoiManager->LargePoiVolumes, &PoiManager->PendingLargePoiVolumes })
    {
        for (auto Volume : *List)
        {
            auto Discoverable = Volume ? (UFortPoi_DiscoverableComponent*)Volume->GetComponentByClass(UFortPoi_DiscoverableComponent::StaticClass()) : nullptr;

            if (!Discoverable)
                continue;

            for (auto& Tag : Volume->LocationTags.GameplayTags)
            {
                if (!MatchesTag(&Tag, &UnNamedPOITag) && !MatchesTag(&Tag, &POITag))
                    continue;

                if (std::none_of(DiscoverablePOIs.begin(), DiscoverablePOIs.end(), [&](auto& DiscoverablePOI) { return DiscoverablePOI.POITag.TagName == Tag.TagName; }))
                    DiscoverablePOIs.push_back({ Tag, Discoverable->QuestPartOf.Get(), Discoverable->QuestObjectiveBackendName });

                break;
            }
        }
    }

    return DiscoverablePOIs;
}

static FPoiDiscoverabilityDataEntry* FindEntry(UFortControllerComponent_MapDiscoverability* Component, const FGameplayTag& POITag)
{
    for (auto& Entry : Component->DiscoveredData.Entries)
    {
        if (Entry.POITag.TagName == POITag.TagName)
            return &Entry;
    }

    return nullptr;
}

static bool HasCompletedDiscoverObjective(const FDiscoverablePOI& DiscoverablePOI, TArray<UFortQuestItem*>& Quests)
{
    static auto GetObjectiveCompletionCount = (int32 (*)(UFortQuestItem*, FName*))(ImageBase + 0xA10B850);

    if (!DiscoverablePOI.QuestPartOf)
        return true;

    for (auto Quest : Quests)
    {
        if (!Quest || Quest->ItemDefinition != DiscoverablePOI.QuestPartOf)
            continue;

        for (auto& Objective : DiscoverablePOI.QuestPartOf->Objectives)
        {
            if (Objective.BackendName != DiscoverablePOI.QuestObjectiveBackendName)
                continue;

            auto BackendName = Objective.BackendName;
            return GetObjectiveCompletionCount(Quest, &BackendName) >= Objective.Count;
        }
    }

    return false;
}

static void AddDiscoveredData(AFortPoiManager* PoiManager, AFortPlayerControllerAthena* PlayerController, UFortControllerComponent_MapDiscoverability* Component)
{
    static auto GetQuestItems = (void (*)(UFortMcpProfileSubgame*, uint8, TArray<UFortQuestItem*>*))(ImageBase + 0x9D3C544);

    auto& DiscoverablePOIs = GetDiscoverablePOIs(PoiManager);
    auto& Entries = Component->DiscoveredData.Entries;

    if (Entries.Num() >= (int32)DiscoverablePOIs.size())
        return;

    auto QuestManager = PlayerController->GetQuestManager(ESubGame::Athena);
    auto Profile = QuestManager ? QuestManager->BoundProfile.Get() : nullptr;

    if (!Profile)
        return;

    TArray<UFortQuestItem*> Quests;
    GetQuestItems(Profile, 0, &Quests);

    for (auto& DiscoverablePOI : DiscoverablePOIs)
    {
        if (FindEntry(Component, DiscoverablePOI.POITag))
            continue;

        auto bDiscovered = HasCompletedDiscoverObjective(DiscoverablePOI, Quests);

        FPoiDiscoverabilityDataEntry NewEntry{};
        NewEntry.ReplicationID = -1;
        NewEntry.ReplicationKey = -1;
        NewEntry.MostRecentArrayReplicationKey = -1;
        NewEntry.POITag = DiscoverablePOI.POITag;
        NewEntry.DiscoveredState = bDiscovered ? EPoiDiscoveredState::Discovered : EPoiDiscoveredState::Undiscovered;
        NewEntry.bWasInitiallyDiscovered = bDiscovered;

        Component->DiscoveredData.MarkItemDirty(Entries.Add(NewEntry));
    }

    Quests.Free();
}

static void (*ProcessTagDeltaForControllerOG)(AFortPoiManager*, AFortPlayerControllerGameplay*, TArray<FGameplayTag>&, TArray<FGameplayTag>&);
static void ProcessTagDeltaForController(AFortPoiManager* PoiManager, AFortPlayerControllerGameplay* PlayerController, TArray<FGameplayTag>& ExitedTags, TArray<FGameplayTag>& EnteredTags)
{
    auto PlayerControllerAthena = PlayerController && PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)PlayerController : nullptr;
    auto Component = PlayerControllerAthena ? PlayerControllerAthena->DiscoverabilityComponent : nullptr;

    if (Component)
        AddDiscoveredData(PoiManager, PlayerControllerAthena, Component);

    ProcessTagDeltaForControllerOG(PoiManager, PlayerController, ExitedTags, EnteredTags);

    if (!Component || *(bool*)(__int64(Component) + 0x39C))
        return;

    for (auto& Tag : EnteredTags)
    {
        auto Entry = FindEntry(Component, Tag);

        if (!Entry || Entry->VisitCounter != 0)
            continue;

        if (Entry->DiscoveredState == EPoiDiscoveredState::Undiscovered)
            Entry->DiscoveredState = EPoiDiscoveredState::Discovered;

        Entry->VisitCounter++;
        Component->DiscoveredData.MarkItemDirty(*Entry);
    }
}

void UFortControllerComponent_MapDiscoverability__Init()
{
    Hook(ImageBase + 0x908CF5C, ProcessTagDeltaForController, ProcessTagDeltaForControllerOG);
}
