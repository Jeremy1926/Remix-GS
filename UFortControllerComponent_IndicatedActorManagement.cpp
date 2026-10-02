#include "pch.h"
#include "Shared.h"

INIT_POSTLOAD(UFortControllerComponent_IndicatedActorManagement);

static void CopyFrom(FIndicatedActorData& Data, const FIndicatedActorData& Other)
{
    memcpy((PBYTE)&Data, (const PBYTE)&Other, sizeof(FIndicatedActorData));

    Data.GroupIdentifier = Other.GroupIdentifier.Num() > 0 && Other.GroupIdentifier.Data ? FString(Other.GroupIdentifier.Data) : FString();
    Data.EventTags.GameplayTags = TArray<FGameplayTag>();
    Data.EventTags.ParentTags = TArray<FGameplayTag>();

    for (auto& Tag : Other.EventTags.GameplayTags)
        Data.EventTags.GameplayTags.Add(Tag);

    for (auto& Tag : Other.EventTags.ParentTags)
        Data.EventTags.ParentTags.Add(Tag);
}

static void CopyFrom(FStenciledActorData& Data, const FStenciledActorData& Other)
{
    memcpy((PBYTE)&Data, (const PBYTE)&Other, sizeof(FStenciledActorData));

    Data.GroupIdentifier = Other.GroupIdentifier.Num() > 0 && Other.GroupIdentifier.Data ? FString(Other.GroupIdentifier.Data) : FString();
}

static void GetMarkableActors(UFortControllerComponent_IndicatedActorManagement* Component, TArray<AActor*>& Actors, TArray<AActor*>& OutActors)
{
    static auto CannotBeMarkedByAnyTag = FGameplayTag{ FName(L"Athena.Gameplay.Status.CannotBeMarkedBy.Any") };
    static auto CannotBeMarkedByEnemyOnlyTag = FGameplayTag{ FName(L"Athena.Gameplay.Status.CannotBeMarkedBy.EnemyOnly") };

    auto PlayerController = Component->GetOwner()->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Component->GetOwner() : nullptr;
    auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;

    for (auto Actor : Actors)
    {
        if (!Actor)
            continue;

        FGameplayTagContainer OwnedTags{};

        if (auto Interface = Actor->GetInterface(IGameplayTagAssetInterface::StaticClass()))
            ((void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2])(Interface, &OwnedTags);

        auto Pawn = Actor->IsA(AFortPlayerPawnAthena::StaticClass()) ? (AFortPlayerPawnAthena*)Actor : nullptr;
        auto ActorPlayerState = Pawn && Pawn->PlayerState && Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;
        auto bIsEnemy = !PlayerState || !ActorPlayerState || ActorPlayerState->TeamIndex != PlayerState->TeamIndex;

        if (!OwnedTags.HasTag(CannotBeMarkedByAnyTag) && (!bIsEnemy || !OwnedTags.HasTag(CannotBeMarkedByEnemyOnlyTag)))
            OutActors.Add(Actor);

        OwnedTags.GameplayTags.Free();
        OwnedTags.ParentTags.Free();
    }
}

static void GetSharedComponents(UFortControllerComponent_IndicatedActorManagement* Component, EShareActorWith ShareActorWith, uint8 ShareActorWithMask, TArray<UFortControllerComponent_IndicatedActorManagement*>& OutComponents)
{
    OutComponents.Add(Component);

    auto PlayerController = Component->GetOwner()->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Component->GetOwner() : nullptr;
    auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;
    auto bShareWithSquad = ShareActorWith == EShareActorWith::SquadOnTeam || (ShareActorWithMask & (uint8)EShareActorWithMask::SquadOnTeam) != 0;
    auto bShareWithTeam = ShareActorWith == EShareActorWith::AllTeam || (ShareActorWithMask & (uint8)EShareActorWithMask::AllTeam) != 0;

    if (!PlayerState || (!bShareWithSquad && !bShareWithTeam))
        return;

    TArray<AFortPlayerControllerAthena*> PlayerControllers;
    GetAll<AFortPlayerControllerAthena>(PlayerControllers);

    for (auto OtherController : PlayerControllers)
    {
        auto OtherPlayerState = OtherController && OtherController != PlayerController ? (AFortPlayerStateAthena*)OtherController->PlayerState : nullptr;

        if (!OtherPlayerState || OtherPlayerState->TeamIndex != PlayerState->TeamIndex || (!bShareWithTeam && OtherPlayerState->SquadID != PlayerState->SquadID))
            continue;

        if (auto OtherComponent = (UFortControllerComponent_IndicatedActorManagement*)OtherController->GetComponentByClass(UFortControllerComponent_IndicatedActorManagement::StaticClass()))
            OutComponents.Add(OtherComponent);
    }

    PlayerControllers.Free();
}

static uint8 GetStencilIndex(FName StencilName)
{
    static UMaterialParameterCollection* StencilDefinitionCollection = nullptr;

    if (StencilName.IsNone())
        return 0;

    if (!StencilDefinitionCollection)
    {
        auto AssetManager = GetFirstInstance<UFortAssetManager>();
        auto GameData = AssetManager ? AssetManager->GameDataCommon : nullptr;

        StencilDefinitionCollection = GameData ? GameData->StencilDefinitionCollection.Get() : nullptr;
    }

    if (!StencilDefinitionCollection)
        return 0;

    for (auto& Parameter : StencilDefinitionCollection->ScalarParameters)
    {
        if (Parameter.ParameterName == StencilName)
            return (uint8)(int32)floorf(Parameter.DefaultValue + 0.5f);
    }

    return 0;
}

static void SetSoftActor(TSoftObjectPtr<AActor>& SoftActor, AActor* Actor)
{
    auto SoftObject = UKismetSystemLibrary::Conv_ObjectToSoftObjectReference(Actor);

    memcpy((PBYTE)&SoftActor, (const PBYTE)&SoftObject, sizeof(TSoftObjectPtr<AActor>));
}

static void AddIndicatedActors(UFortControllerComponent_IndicatedActorManagement* Component, TArray<AActor*>& IndicatedActors, FIndicatedActorData& Data, bool bAddAsUnique, bool bReplaceExistingEntry, bool bRefreshExistingEntry)
{
    TArray<AActor*> MarkableActors;
    GetMarkableActors(Component, IndicatedActors, MarkableActors);

    TArray<UFortControllerComponent_IndicatedActorManagement*> Components;
    GetSharedComponents(Component, Data.ShareActorWith, Data.ShareActorWithMask, Components);

    auto GameState = UWorld::GetWorld()->GameState;
    auto StartTime = GameState ? (float)GameState->GetServerWorldTimeSeconds() : 0.f;
    auto EndTime = Data.duration > 0.f ? StartTime + Data.duration : 0.f;

    for (auto SharedComponent : Components)
    {
        auto& IndicatedList = SharedComponent->IndicatedActorList;
        auto& Entries = IndicatedList.Entries;
        bool bChanged = false;

        for (auto Actor : MarkableActors)
        {
            if (!Actor)
                continue;

            FIndicatedActorInfoEntry* ExistingEntry = nullptr;

            for (auto& Entry : Entries)
            {
                if (Entry.Actor.WeakPtr.Get() == Actor)
                {
                    ExistingEntry = &Entry;
                    break;
                }
            }

            if (ExistingEntry && (bAddAsUnique || bReplaceExistingEntry || bRefreshExistingEntry))
            {
                if (!bReplaceExistingEntry && !bRefreshExistingEntry)
                    continue;

                if (bReplaceExistingEntry)
                {
                    ExistingEntry->Data.GroupIdentifier.Free();
                    ExistingEntry->Data.EventTags.GameplayTags.Free();
                    ExistingEntry->Data.EventTags.ParentTags.Free();
                    CopyFrom(ExistingEntry->Data, Data);
                }

                ExistingEntry->StartTime = StartTime;
                ExistingEntry->EndTime = EndTime;
                ExistingEntry->bReplaceExistingWhenAdded = bReplaceExistingEntry;
                ExistingEntry->bRefreshExistingWhenAdded = bRefreshExistingEntry;

                IndicatedList.MarkItemDirty(*ExistingEntry);
                bChanged = true;
                continue;
            }

            FIndicatedActorInfoEntry NewEntry{};
            NewEntry.ReplicationID = -1;
            NewEntry.ReplicationKey = -1;
            NewEntry.MostRecentArrayReplicationKey = -1;
            SetSoftActor(NewEntry.Actor, Actor);
            NewEntry.StartTime = StartTime;
            NewEntry.EndTime = EndTime;
            CopyFrom(NewEntry.Data, Data);
            NewEntry.bReplaceExistingWhenAdded = bReplaceExistingEntry;
            NewEntry.bRefreshExistingWhenAdded = bRefreshExistingEntry;

            IndicatedList.MarkItemDirty(Entries.Add(NewEntry));
            bChanged = true;
        }

        if (!bChanged)
            continue;

        SharedComponent->SetComponentTickEnabled(true);

        if (auto Owner = SharedComponent->GetOwner())
            Owner->ForceNetUpdate();
    }

    Components.Free();
    MarkableActors.Free();
}

static void AddStenciledActors(UFortControllerComponent_IndicatedActorManagement* Component, TArray<AActor*>& StenciledActors, FStenciledActorData& Data, bool bAddAsUnique, bool bReplaceExistingEntry, bool bRefreshExistingEntry)
{
    auto PlayerController = Component->GetOwner()->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Component->GetOwner() : nullptr;
    auto MarkedBy = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;

    Data.FriendlyStencilIndex = GetStencilIndex(Data.FriendlyStencilName);
    Data.EnemyStencilIndex = GetStencilIndex(Data.EnemyStencilName);

    TArray<AActor*> MarkableActors;
    GetMarkableActors(Component, StenciledActors, MarkableActors);

    TArray<UFortControllerComponent_IndicatedActorManagement*> Components;
    GetSharedComponents(Component, Data.ShareActorWith, Data.ShareActorWithMask, Components);

    auto GameState = UWorld::GetWorld()->GameState;
    auto StartTime = GameState ? (float)GameState->GetServerWorldTimeSeconds() : 0.f;
    auto EndTime = Data.duration > 0.f ? StartTime + Data.duration : 0.f;

    for (auto SharedComponent : Components)
    {
        auto& StenciledList = SharedComponent->StenciledActorList;
        auto& Entries = StenciledList.Entries;
        bool bChanged = false;

        for (auto Actor : MarkableActors)
        {
            if (!Actor)
                continue;

            FStenciledActorInfoEntry* ExistingEntry = nullptr;

            for (auto& Entry : Entries)
            {
                if (Entry.Actor.WeakPtr.Get() == Actor)
                {
                    ExistingEntry = &Entry;
                    break;
                }
            }

            if (ExistingEntry && (bAddAsUnique || bReplaceExistingEntry || bRefreshExistingEntry))
            {
                if (!bReplaceExistingEntry && !bRefreshExistingEntry)
                    continue;

                if (bReplaceExistingEntry)
                {
                    ExistingEntry->Data.GroupIdentifier.Free();
                    CopyFrom(ExistingEntry->Data, Data);
                    ExistingEntry->MarkedBy = MarkedBy;
                }

                ExistingEntry->StartTime = StartTime;
                ExistingEntry->EndTime = EndTime;
                ExistingEntry->bReplaceExistingWhenAdded = bReplaceExistingEntry;
                ExistingEntry->bRefreshExistingWhenAdded = bRefreshExistingEntry;

                StenciledList.MarkItemDirty(*ExistingEntry);
                bChanged = true;
                continue;
            }

            FStenciledActorInfoEntry NewEntry{};
            NewEntry.ReplicationID = -1;
            NewEntry.ReplicationKey = -1;
            NewEntry.MostRecentArrayReplicationKey = -1;
            SetSoftActor(NewEntry.Actor, Actor);
            NewEntry.MarkedBy = MarkedBy;
            NewEntry.StartTime = StartTime;
            NewEntry.EndTime = EndTime;
            CopyFrom(NewEntry.Data, Data);
            NewEntry.bReplaceExistingWhenAdded = bReplaceExistingEntry;
            NewEntry.bRefreshExistingWhenAdded = bRefreshExistingEntry;

            StenciledList.MarkItemDirty(Entries.Add(NewEntry));
            bChanged = true;
        }

        if (!bChanged)
            continue;

        SharedComponent->SetComponentTickEnabled(true);

        if (auto Owner = SharedComponent->GetOwner())
            Owner->ForceNetUpdate();
    }

    Components.Free();
    MarkableActors.Free();
}

static void AddActorsToIndicatedList(UObject* Context, FFrame& Stack)
{
    AController* InstigatingController;
    TArray<AActor*> IndicatedActors;
    FIndicatedActorData IndicatedActorData{};
    bool bAddAsUnique;
    bool bAllowOwningPlayer;
    bool bReplaceExistingEntry;
    bool bRefreshExistingEntry;

    Stack.StepCompiledIn(&InstigatingController);
    Stack.StepCompiledIn(&IndicatedActors);
    Stack.StepCompiledIn(&IndicatedActorData);
    Stack.StepCompiledIn(&bAddAsUnique);
    Stack.StepCompiledIn(&bAllowOwningPlayer);
    Stack.StepCompiledIn(&bReplaceExistingEntry);
    Stack.StepCompiledIn(&bRefreshExistingEntry);
    Stack.IncrementCode();

    if (auto Component = InstigatingController ? (UFortControllerComponent_IndicatedActorManagement*)InstigatingController->GetComponentByClass(UFortControllerComponent_IndicatedActorManagement::StaticClass()) : nullptr)
        AddIndicatedActors(Component, IndicatedActors, IndicatedActorData, bAddAsUnique, bReplaceExistingEntry, bRefreshExistingEntry);

    IndicatedActorData.GroupIdentifier.Free();
    IndicatedActorData.EventTags.GameplayTags.Free();
    IndicatedActorData.EventTags.ParentTags.Free();
    IndicatedActors.Free();
}

static void AddActorsToStenciledList(UObject* Context, FFrame& Stack)
{
    AController* InstigatingController;
    TArray<AActor*> StenciledActors;
    FStenciledActorData StenciledActorData{};
    bool bAddAsUnique;
    bool bReplaceExistingEntry;
    bool bRefreshExistingEntry;

    Stack.StepCompiledIn(&InstigatingController);
    Stack.StepCompiledIn(&StenciledActors);
    Stack.StepCompiledIn(&StenciledActorData);
    Stack.StepCompiledIn(&bAddAsUnique);
    Stack.StepCompiledIn(&bReplaceExistingEntry);
    Stack.StepCompiledIn(&bRefreshExistingEntry);
    Stack.IncrementCode();

    if (auto Component = InstigatingController ? (UFortControllerComponent_IndicatedActorManagement*)InstigatingController->GetComponentByClass(UFortControllerComponent_IndicatedActorManagement::StaticClass()) : nullptr)
        AddStenciledActors(Component, StenciledActors, StenciledActorData, bAddAsUnique, bReplaceExistingEntry, bRefreshExistingEntry);

    StenciledActorData.GroupIdentifier.Free();
    StenciledActors.Free();
}

static void GetActorsInRadius(AActor* InstigatingActor, FIndicatedActorDataWithFilter& FilterData, TArray<AActor*>& OutActors)
{
    TArray<AActor*> OverlappedActors;
    TArray<AActor*> ActorsToIgnore;
    auto ActorClassFilter = FilterData.ActorClassFilter.Get();
    auto Location = InstigatingActor->K2_GetActorLocation();

    if (FilterData.OverlapHalfHeight > 0.f)
        UKismetSystemLibrary::CapsuleOverlapActors(InstigatingActor, Location, FilterData.OverlapRadius, FilterData.OverlapHalfHeight, FilterData.ObjectTypes, ActorClassFilter, ActorsToIgnore, &OverlappedActors);
    else
        UKismetSystemLibrary::SphereOverlapActors(InstigatingActor, Location, FilterData.OverlapRadius, FilterData.ObjectTypes, ActorClassFilter, ActorsToIgnore, &OverlappedActors);

    auto bHasTagQuery = !UBlueprintGameplayTagLibrary::IsTagQueryEmpty(FilterData.IndicatedActorTagQuery);

    for (auto Actor : OverlappedActors)
    {
        if (!Actor)
            continue;

        if (FilterData.WithAffiliation.Num() > 0)
        {
            auto Affiliation = UFortKismetLibrary::GetTeamAffiliation(InstigatingActor, Actor);

            if (!FilterData.WithAffiliation.Contains(Affiliation))
                continue;
        }

        FGameplayTagContainer OwnedTags{};

        if (auto Interface = Actor->GetInterface(IGameplayTagAssetInterface::StaticClass()))
            ((void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2])(Interface, &OwnedTags);

        auto bMatchesTags = FilterData.IndicatedActorTags.GameplayTags.Num() == 0 || OwnedTags.HasAny(FilterData.IndicatedActorTags);
        auto bMatchesQuery = !bHasTagQuery || UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(OwnedTags, FilterData.IndicatedActorTagQuery);

        if (bMatchesTags && bMatchesQuery)
            OutActors.Add(Actor);

        OwnedTags.GameplayTags.Free();
        OwnedTags.ParentTags.Free();
    }

    OverlappedActors.Free();
}

static void AddActorsInRadiusToIndicatedList(UObject* Context, FFrame& Stack)
{
    AController* InstigatingController;
    bool bAddAsUnique;
    bool bReplaceExistingEntry;
    bool bRefreshExistingEntry;
    AActor* InstigatingActorOverride;

    Stack.StepCompiledIn(&InstigatingController);
    TArray<FIndicatedActorDataWithFilter> IndicatedActorFilterDatasTemp;
    auto& IndicatedActorFilterDatas = Stack.StepCompiledInRef(&IndicatedActorFilterDatasTemp);
    Stack.StepCompiledIn(&bAddAsUnique);
    Stack.StepCompiledIn(&bReplaceExistingEntry);
    Stack.StepCompiledIn(&bRefreshExistingEntry);
    Stack.StepCompiledIn(&InstigatingActorOverride);
    Stack.IncrementCode();

    auto Component = InstigatingController ? (UFortControllerComponent_IndicatedActorManagement*)InstigatingController->GetComponentByClass(UFortControllerComponent_IndicatedActorManagement::StaticClass()) : nullptr;
    auto InstigatingActor = InstigatingActorOverride ? InstigatingActorOverride : (InstigatingController ? InstigatingController->Pawn : nullptr);

    if (!Component || !InstigatingActor)
        return;

    for (auto& FilterData : IndicatedActorFilterDatas)
    {
        TArray<AActor*> IndicatedActors;
        GetActorsInRadius(InstigatingActor, FilterData, IndicatedActors);

        if (IndicatedActors.Num() > 0)
            AddIndicatedActors(Component, IndicatedActors, FilterData.IndicatedData, bAddAsUnique, bReplaceExistingEntry, bRefreshExistingEntry);

        IndicatedActors.Free();
    }
}

static void AddActorsInRadiusToStenciledList(UObject* Context, FFrame& Stack)
{
    AController* InstigatingController;
    bool bAddAsUnique;
    bool bReplaceExistingEntry;
    bool bRefreshExistingEntry;
    AActor* InstigatingActorOverride;

    Stack.StepCompiledIn(&InstigatingController);
    TArray<FIndicatedActorDataWithFilter> StenciledActorFilterDatasTemp;
    auto& StenciledActorFilterDatas = Stack.StepCompiledInRef(&StenciledActorFilterDatasTemp);
    Stack.StepCompiledIn(&bAddAsUnique);
    Stack.StepCompiledIn(&bReplaceExistingEntry);
    Stack.StepCompiledIn(&bRefreshExistingEntry);
    Stack.StepCompiledIn(&InstigatingActorOverride);
    Stack.IncrementCode();

    auto Component = InstigatingController ? (UFortControllerComponent_IndicatedActorManagement*)InstigatingController->GetComponentByClass(UFortControllerComponent_IndicatedActorManagement::StaticClass()) : nullptr;
    auto InstigatingActor = InstigatingActorOverride ? InstigatingActorOverride : (InstigatingController ? InstigatingController->Pawn : nullptr);

    if (!Component || !InstigatingActor)
        return;

    for (auto& FilterData : StenciledActorFilterDatas)
    {
        TArray<AActor*> StenciledActors;
        GetActorsInRadius(InstigatingActor, FilterData, StenciledActors);

        if (StenciledActors.Num() > 0)
            AddStenciledActors(Component, StenciledActors, FilterData.StenciledData, bAddAsUnique, bReplaceExistingEntry, bRefreshExistingEntry);

        StenciledActors.Free();
    }
}

void UFortControllerComponent_IndicatedActorManagement__PostLoad()
{
    auto LibraryClass = UFortIndicatedActorManagementLibrary::StaticClass();

    ExecHook(LibraryClass->FindFunction("AddActorsToIndicatedList"), AddActorsToIndicatedList);
    ExecHook(LibraryClass->FindFunction("AddActorsToStenciledList"), AddActorsToStenciledList);
    ExecHook(LibraryClass->FindFunction("AddActorsInRadiusToIndicatedList"), AddActorsInRadiusToIndicatedList);
    ExecHook(LibraryClass->FindFunction("AddActorsInRadiusToStenciledList"), AddActorsInRadiusToStenciledList);
}
