#include "pch.h"
#include "Shared.h"
#include "ABuildingActor.h"
#include "AFortInventory.h"
#include "AFortPickup.h"
#include "UAthenaAIServiceLoot.h"
#include "UConversationInstance.h"

INIT_MODULE(ABuildingActor);
INIT_POSTLOAD(ABuildingActor);

static std::optional<UCurveTable*> ResourceRates;

static void (*BroadcastNotifyBuildingNetCullDistChangeOG)(void*, ABuildingActor*, float);
static void BroadcastNotifyBuildingNetCullDistChange(void* Delegate, ABuildingActor* BuildingActor, float CullDistance)
{
    BroadcastNotifyBuildingNetCullDistChangeOG(Delegate, BuildingActor, CullDistance);

    if (Delegate != (void*)(ImageBase + 0x12A9ACC0) || !BuildingActor)
        return;

    static auto SetCullDistanceOverride = (void (*)(AActor*, float))(ImageBase + 0x6ED5534);

    auto NetCullDistance = CullDistance + 1000.f;
    SetCullDistanceOverride(BuildingActor, NetCullDistance * NetCullDistance);
}

static void (*OnDamageServerOG)(ABuildingSMActor*, float, const FGameplayTagContainer&, const FVector&, const FHitResult&, AController*, AActor*, const FGameplayEffectContextHandle&);
static void OnDamageServer(ABuildingSMActor* Actor, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, const FHitResult& HitInfo, AController* InstigatedBy, AActor* DamageCauser,
                           const FGameplayEffectContextHandle& EffectContext)
{
    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    auto Controller = InstigatedBy && InstigatedBy->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)InstigatedBy : nullptr;
    auto BotController = InstigatedBy && InstigatedBy->IsA(AFortAthenaAIBotController::StaticClass()) ? (AFortAthenaAIBotController*)InstigatedBy : nullptr;

    if ((!Controller && !BotController) || !Actor->IsA(ABuildingSMActor::StaticClass()) || Actor->bPlayerPlaced || Actor->GetHealth() == 1)
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

    auto Inventory = (AFortInventory*)(Controller ? Controller->WorldInventory.ObjectPointer : BotController->Inventory.ObjectPointer);
    auto InstigatorPawn = Controller ? Controller->MyFortPawn : (AFortPawn*)BotController->Pawn;

    if (!Inventory || !DamageCauser || !DamageCauser->IsA(AFortWeapon::StaticClass()) || !((AFortWeapon*)DamageCauser)->WeaponData->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

    auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(Actor->ResourceType);

    if (!Resource)
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

    auto MaxMat = Resource->GetMaxStackSize(nullptr);

    if (!ResourceRates)
    {
        auto Playlist = GameState->CurrentPlaylistInfo.BasePlaylist;
        ResourceRates = Playlist ? Playlist->ResourceRates.Get() : nullptr;
    }

    if (!*ResourceRates)
        ResourceRates = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaResourceRates.AthenaResourceRates");

    static auto GetSparseClassData = (FBuildingSMActorClassData* (*)(UClass*, uint8))(ImageBase + 0x21E0B6C);

    int ResCount = 0;
    auto& BuildingResourceAmountOverride = GetSparseClassData(Actor->Class, 1)->BuildingResourceAmountOverride;

    if (!BuildingResourceAmountOverride.RowName.IsNone())
    {
        float Out = 0.f;
        UDataTableFunctionLibrary::EvaluateCurveTableRow(*ResourceRates, BuildingResourceAmountOverride.RowName, 0.f, nullptr, &Out, FString());

        float RC = Out / (Actor->GetMaxHealth() / Damage);

        ResCount = (int)round(RC);
    }

    if (ResCount > 0)
    {
        auto Item = FindItemInstance(Inventory, Resource);
        auto ItemEntry = FindReplicatedEntry(Inventory, Resource);

        if (Item)
        {
            ItemEntry->Count += ResCount;
            if (ItemEntry->Count > MaxMat)
            {
                SpawnPickup(InstigatedBy->Pawn->K2_GetActorLocation(), (UFortItemDefinition*)Item->ItemEntry.ItemDefinition, ItemEntry->Count - MaxMat, 0, EFortPickupSourceTypeFlag::Tossed,
                            EFortPickupSpawnSource::Unset, InstigatorPawn);
                ItemEntry->Count = MaxMat;
            }

            for (auto& StateValue : ItemEntry->StateValues)
            {
                if (StateValue.StateType != EFortItemEntryState::ShouldShowItemToast)
                    continue;

                StateValue.IntValue = 0;
                break;
            }

            Item->ItemEntry.Count = ItemEntry->Count;
            UpdateEntry(Inventory, *ItemEntry);
            Item->ItemEntry.bIsDirty = true;
        }
        else
        {
            if (ResCount > MaxMat)
            {
                SpawnPickup(InstigatedBy->Pawn->K2_GetActorLocation(), Resource, ResCount - MaxMat, 0, EFortPickupSourceTypeFlag::Tossed, EFortPickupSpawnSource::Unset, InstigatorPawn);
                ResCount = MaxMat;
            }

            GiveItem(Inventory, Resource, ResCount, 0, 0, false);
        }
    }

    if (ResCount > 0 && Controller)
        Controller->ClientReportDamagedResourceBuilding(Actor, Actor->ResourceType, ResCount, Actor->GetHealth() - Damage <= 0, Damage == 100.f);

    Actor->ForceNetUpdate();
    return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
}

static bool SpawnLoot(ABuildingContainer* Container)
{
    if (Container->bAlreadySearched)
        return false;

    auto RealTierGroup = Container->SearchLootTierGroup;
    auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

    for (auto& Pair : GameMode->RedirectAthenaLootTierGroups)
    {
        if (Pair.Key().ComparisonIndex == Container->SearchLootTierGroup.ComparisonIndex)
        {
            RealTierGroup.ComparisonIndex = Pair.Value().ComparisonIndex;
            break;
        }
    }

    TArray<FFortItemEntry*> LootDrops{};

    ChooseLootForContainer(LootDrops, RealTierGroup, -1, ((AFortGameStateAthena*)GameMode->GameState)->WorldLevel);

    for (auto& LootDrop : LootDrops)
    {
        SpawnPickup(Container, *LootDrop);
        free(LootDrop);
    }

    Container->bAlreadySearched = true;
    Container->OnRep_bAlreadySearched();
    Container->SearchBounceData.SearchAnimationCount++;
    Container->BounceContainer();

    return true;
}

static void (*BeginPlayOG)(ABuildingContainer*);
static void BeginPlay(ABuildingContainer* Container)
{
    BeginPlayOG(Container);
    RegisterChestLoot(Container);
}

static void (*OnAuthorityRandomUpgradeAppliedOG)(ABuildingContainer*, FName&);
static void OnAuthorityRandomUpgradeApplied(ABuildingContainer* Container, FName& UpgradeTierGroup)
{
    static auto GetSparseClassData = (FBuildingSMActorClassData* (*)(UClass*, uint8))(ImageBase + 0x21E0B6C);

    auto ChosenRandomUpgrade = Container->ChosenRandomUpgrade;
    auto ClassData = GetSparseClassData(Container->Class, 1);

    if (ChosenRandomUpgrade < 0 || ChosenRandomUpgrade >= ClassData->AlternateMeshes.Num())
        return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);

    auto& AlternateMeshSet = ClassData->AlternateMeshes[ChosenRandomUpgrade];

    Container->ReplicatedLootTier = AlternateMeshSet.Tier;
    Container->OnRep_LootTier();

    return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);
}

static void SetDynamicFoundationEnabled(UObject* Context, FFrame& Stack, void* Ret)
{
    bool bEnabled;
    Stack.StepCompiledIn(&bEnabled);
    Stack.IncrementCode();

    auto Foundation = (ABuildingFoundation*)Context;

    Foundation->DynamicFoundationRepData.EnabledState = bEnabled ? EDynamicFoundationEnabledState::Enabled : EDynamicFoundationEnabledState::Disabled;
    Foundation->OnRep_DynamicFoundationRepData();

    Foundation->FoundationEnabledState = bEnabled ? EDynamicFoundationEnabledState::Enabled : EDynamicFoundationEnabledState::Disabled;

    bool bSetup = true;

    if (bEnabled && Foundation->LevelToStream.ComparisonIndex == 0)
    {
        static auto SelectAndSetupMyBuildingLevel = (bool (*)(ABuildingFoundation*, void*))(ImageBase + 0x92F3380);
        bSetup = SelectAndSetupMyBuildingLevel(Foundation, nullptr);
    }

    static auto StreamInMyBuilding = (void (*)(ABuildingFoundation*, bool))(ImageBase + 0x92F6D28);

    if (bSetup)
        StreamInMyBuilding(Foundation, false);
}

static void SetDynamicFoundationTransform(UObject* Context, FFrame& Stack, void* Ret)
{
    FTransform NewTransformTemp{};
    auto& NewTransform = Stack.StepCompiledInRef(&NewTransformTemp);
    Stack.IncrementCode();

    auto Foundation = (ABuildingFoundation*)Context;

    Foundation->DynamicFoundationTransform = NewTransform;
    Foundation->DynamicFoundationRepData.Rotation = UKismetMathLibrary::Quat_Rotator(NewTransform.Rotation);
    Foundation->DynamicFoundationRepData.Translation = NewTransform.Translation;
    Foundation->StreamingData.FoundationLocation = NewTransform.Translation;
    Foundation->OnRep_DynamicFoundationRepData();
}

void ConsumeResource(AFortPlayerControllerAthena* PlayerController, const UFortItemDefinition* ResourceDefinition, int32 Amount)
{
    static auto RemoveInventoryItem = (bool (*)(void*, FGuid&, int32, bool, bool))(ImageBase + 0xA02DACC);

    auto InventoryOwner = PlayerController->GetInterface(IFortInventoryOwnerInterface::StaticClass());

    if (!InventoryOwner || !ResourceDefinition || Amount <= 0)
        return;

    auto& ReplicatedEntries = ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries;
    std::vector<std::pair<FGuid, int32>> Stacks;

    for (auto& Entry : ReplicatedEntries)
    {
        if (Entry.ItemDefinition == (UItemDefinitionBase*)ResourceDefinition && Entry.Count > 0)
            Stacks.push_back({ Entry.ItemGuid, Entry.Count });
    }

    for (auto& [ItemGuid, Count] : Stacks)
    {
        if (Amount <= 0)
            break;

        auto Taken = Count < Amount ? Count : Amount;
        RemoveInventoryItem(InventoryOwner, ItemGuid, Taken, false, false);
        Amount -= Taken;
    }
}

static void GrantOutput(ABuildingItemWeaponUpgradeActor* Bench, AFortPlayerControllerAthena* PlayerController, const UFortItemDefinition* UpgradedWeaponDefinition)
{
    static auto IsWeaponInDataTable = (FWeaponUpgradeItemRow* (*)(const UItemDefinitionBase*, EFortWeaponUpgradeDirection))(ImageBase + 0x93157EC);
    static auto GetUpgradeCost = (int32 (*)(const ABuildingItemWeaponUpgradeActor*, const EFortWeaponUpgradeCosts*))(ImageBase + 0x9313424);

    if (!Bench || !PlayerController || !UpgradedWeaponDefinition || !PlayerController->WorldInventory.ObjectPointer)
        return;

    auto& PendingEntry = *(FFortItemEntry*)(__int64(Bench) + 0xD50);

    if (!*(bool*)(__int64(Bench) + 0xD50 + sizeof(FFortItemEntry)) || !PendingEntry.ItemDefinition)
        return;

    auto Row = IsWeaponInDataTable(PendingEntry.ItemDefinition, EFortWeaponUpgradeDirection::Vertical);
    if (!Row || Row->UpgradedWeaponDef != UpgradedWeaponDefinition)
        Row = IsWeaponInDataTable(PendingEntry.ItemDefinition, EFortWeaponUpgradeDirection::Horizontal);
    if (!Row || Row->UpgradedWeaponDef != UpgradedWeaponDefinition)
        return;

    auto WoodCost = GetUpgradeCost(Bench, &Row->WoodCost);
    auto MetalCost = GetUpgradeCost(Bench, &Row->MetalCost);
    auto BrickCost = GetUpgradeCost(Bench, &Row->BrickCost);

    if (WoodCost == -1 || MetalCost == -1 || BrickCost == -1)
        return;

    ConsumeResource(PlayerController, Bench->WoodItem, WoodCost);
    ConsumeResource(PlayerController, Bench->MetalItem, MetalCost);
    ConsumeResource(PlayerController, Bench->BrickItem, BrickCost);

    auto Inventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto ItemGuid = PendingEntry.ItemGuid;
    auto Pawn = PlayerController->MyFortPawn;
    auto CurrentWeapon = Pawn ? Pawn->CurrentWeapon : nullptr;
    auto bWasEquipped = CurrentWeapon && CurrentWeapon->ItemEntryGuid == ItemGuid;

    for (auto& Entry : Inventory->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemGuid == ItemGuid)
        {
            Remove(Inventory, ItemGuid);
            break;
        }
    }

    auto Item = GiveItem(Inventory, UpgradedWeaponDefinition, 1, PendingEntry.LoadedAmmo, PendingEntry.Level, true, true, PendingEntry.PhantomReserveAmmo, PendingEntry.StateValues);

    if (Item && bWasEquipped)
    {
        PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
        PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
    }
}

void VendWobble__FinishedFunc(UObject* Context, FFrame& Stack, void* Ret)
{
    auto CollectorActor = (ABuildingItemCollectorActor*)Context;
    auto PlayerController = CollectorActor->ControllingPlayer;

    if (!PlayerController)
        return VendWobble__FinishedFuncOG(Context, Stack, Ret);

    FCollectorUnitInfo* Collection = nullptr;

    for (auto& ItemCollection : CollectorActor->ItemCollections)
    {
        if (ItemCollection.InputItem == CollectorActor->ClientPausedActiveInputItem)
        {
            Collection = &ItemCollection;
            break;
        }
    }

    if (!Collection)
        return VendWobble__FinishedFuncOG(Context, Stack, Ret);

    CollectorActor->ClientPausedActiveInputItem = nullptr;

    auto VMLoc = CollectorActor->K2_GetActorLocation();
    auto& SpawnLocation = CollectorActor->LootSpawnLocation;
    auto Loc = VMLoc + (CollectorActor->GetActorForwardVector() * SpawnLocation.X) + (CollectorActor->GetActorRightVector() * SpawnLocation.Y) + (CollectorActor->GetActorUpVector() * SpawnLocation.Z);

    for (auto& Item : Collection->OutputItemEntry)
    {
        SpawnPickup(Loc, Item);
        CollectorActor->PickupSpawned.Process();
    }

    return VendWobble__FinishedFuncOG(Context, Stack, Ret);
}

static AActor* GetInteractingPlayerState(AActor* InteractingActor)
{
    if (InteractingActor->IsA(AFortPlayerStateAthena::StaticClass()))
        return InteractingActor;

    if (InteractingActor->IsA(AFortPlayerPawnAthena::StaticClass()))
    {
        auto PlayerState = ((AFortPlayerPawnAthena*)InteractingActor)->PlayerState;

        if (PlayerState && PlayerState->IsA(AFortPlayerStateAthena::StaticClass()))
            return PlayerState;
    }

    return nullptr;
}

static void OnInteractingActorsChanged(UFortActorComponent_MultiInteract* Component, AActor* PlayerState, TArray<AActor*>& InteractingActors)
{
    if (PlayerState)
        Component->OnRep_InteractingPlayers();
    else
    {
        Component->InteractingNonPlayerActorCount = InteractingActors.Num();
        Component->OnRep_InteractingNonPlayerActorCount();
    }
}

static void AddInteractingActor(UObject* Context, FFrame& Stack, void* Ret)
{
    AActor* InteractingActorToAdd;
    Stack.StepCompiledIn(&InteractingActorToAdd);
    Stack.IncrementCode();

    auto Component = (UFortActorComponent_MultiInteract*)Context;
    auto Owner = Component->GetOwner();

    if (!InteractingActorToAdd || !Owner || Owner->Role != ENetRole::ROLE_Authority)
        return;

    auto PlayerState = GetInteractingPlayerState(InteractingActorToAdd);
    auto& InteractingActors = PlayerState ? *(TArray<AActor*>*)&Component->InteractingPlayers : Component->InteractingNonPlayerActors;
    auto InteractingActor = PlayerState ? PlayerState : InteractingActorToAdd;

    if (InteractingActors.Contains(InteractingActor))
        return;

    if (Component->GetInteractingActorCount(true, true) == 0)
    {
        auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;

        Component->ServerInteractionData.ID = Component->ServerInteractionData.ID == 0xFF ? 1 : Component->ServerInteractionData.ID + 1;
        Component->ServerInteractionData.ServerWorldTime = GameState ? (float)GameState->GetServerWorldTimeSeconds() : -1.f;
        Component->SetComponentTickEnabled(true);
    }

    InteractingActors.Add(InteractingActor);
    OnInteractingActorsChanged(Component, PlayerState, InteractingActors);
}

static void RemoveInteractingActor(UObject* Context, FFrame& Stack, void* Ret)
{
    AActor* InteractingActorToRemove;
    Stack.StepCompiledIn(&InteractingActorToRemove);
    Stack.IncrementCode();

    auto Component = (UFortActorComponent_MultiInteract*)Context;
    auto Owner = Component->GetOwner();

    if (!InteractingActorToRemove || !Owner || Owner->Role != ENetRole::ROLE_Authority)
        return;

    auto PlayerState = GetInteractingPlayerState(InteractingActorToRemove);
    auto& InteractingActors = PlayerState ? *(TArray<AActor*>*)&Component->InteractingPlayers : Component->InteractingNonPlayerActors;
    auto InteractingActor = PlayerState ? PlayerState : InteractingActorToRemove;

    for (int i = 0; i < InteractingActors.Num(); i++)
    {
        if (InteractingActors[i] != InteractingActor)
            continue;

        InteractingActors.Remove(i);
        OnInteractingActorsChanged(Component, PlayerState, InteractingActors);

        if (Component->GetInteractingActorCount(true, true) == 0 && !Component->bAllowGradualReset)
            Component->ResetGradualInteractionTimer();

        return;
    }
}

static int64 OpenableInterfaceOffset = 0;

static void Close_Implementation(void* OpenableInterface, AController* ControllerInstigator)
{
    static auto SetIsDoorOpen = (void (*)(ABuildingWall*, EDoorOpenStyle, TWeakObjectPtr<APawn>))(ImageBase + 0x935F1C4);

    auto BuildingWall = (ABuildingWall*)(__int64(OpenableInterface) - OpenableInterfaceOffset);

    SetIsDoorOpen(BuildingWall, EDoorOpenStyle::Close, TWeakObjectPtr<APawn>(ControllerInstigator ? ControllerInstigator->Pawn : nullptr));
}

static void Open_Implementation(void* OpenableInterface, AController* ControllerInstigator, bool bRequestFastOpen)
{
    static auto SetIsDoorOpen = (void (*)(ABuildingWall*, EDoorOpenStyle, TWeakObjectPtr<APawn>))(ImageBase + 0x935F1C4);

    auto BuildingWall = (ABuildingWall*)(__int64(OpenableInterface) - OpenableInterfaceOffset);

    BuildingWall->bFastOpenRequested = bRequestFastOpen;
    SetIsDoorOpen(BuildingWall, EDoorOpenStyle::Open, TWeakObjectPtr<APawn>(ControllerInstigator ? ControllerInstigator->Pawn : nullptr));
}

template <class T>
static bool (*ServerOnAttemptInteractOG)(void*, uint8*) = nullptr;

template <class T>
static int64 InteractInterfaceOffset = 0;

template <class T>
static bool ServerOnAttemptInteract(void* InteractInterface, uint8* Request)
{
    static auto GetValidInteractionRequestController = (AActor* (*)(uint8*))(ImageBase + 0x98851A4);

    auto Result = ServerOnAttemptInteractOG<T>(InteractInterface, Request);

    if (!Result || Request[0] != 1)
        return Result;

    auto Controller = GetValidInteractionRequestController(Request);

    if (!Controller)
        return Result;

    auto Actor = (AActor*)(__int64(InteractInterface) - InteractInterfaceOffset<T>);
    auto Component = (UFortNonPlayerConversationParticipantComponent*)Actor->GetComponentByClass(UFortNonPlayerConversationParticipantComponent::StaticClass());

    if (Component && CanStartConversationWith(Component, Controller))
        StartConversation(Component, Component->ConversationEntryTag, Controller, Actor, std::is_same_v<T, ABuildingProp_ConversationCompatible> ? UFortPropConversationInstance::StaticClass() : nullptr);

    return Result;
}

template <class T>
static void HookServerOnAttemptInteract()
{
    auto DefaultObject = T::GetDefaultObj();
    auto InteractInterface = DefaultObject->GetInterface(IFortInteractInterface::StaticClass());
    auto InteractVft = *(void***)InteractInterface;

    InteractInterfaceOffset<T> = __int64(InteractInterface) - __int64(DefaultObject);
    ServerOnAttemptInteractOG<T> = decltype(ServerOnAttemptInteractOG<T>)(InteractVft[0x15]);
    VirtualSwap(InteractVft, 0x15, ServerOnAttemptInteract<T>);
}

void ABuildingActor__Init()
{
    Hook(ImageBase + 0x50D6D84, BroadcastNotifyBuildingNetCullDistChange, BroadcastNotifyBuildingNetCullDistChangeOG);
    Hook(ImageBase + 0x92D1784, SpawnLoot);
    Hook(ImageBase + 0x92CC584, OnAuthorityRandomUpgradeApplied, OnAuthorityRandomUpgradeAppliedOG);
    Hook<ABuildingContainer>(uint32(0x74), BeginPlay, BeginPlayOG);
    ExecHook(ABuildingFoundation::StaticClass()->FindFunction("SetDynamicFoundationEnabled"), SetDynamicFoundationEnabled);
    ExecHook(ABuildingFoundation::StaticClass()->FindFunction("SetDynamicFoundationTransform"), SetDynamicFoundationTransform);

    Hook<ABuildingItemWeaponUpgradeActor>(uint32(0x194), GrantOutput);
}

void ABuildingActor__PostLoad()
{
    Hook(ImageBase + 0x92CC9FC, OnDamageServer, OnDamageServerOG);

    ExecHook(UFortActorComponent_MultiInteract::StaticClass()->FindFunction("AddInteractingActor"), AddInteractingActor);
    ExecHook(UFortActorComponent_MultiInteract::StaticClass()->FindFunction("RemoveInteractingActor"), RemoveInteractingActor);

    OpenableInterfaceOffset = __int64(ABuildingWall::GetDefaultObj()->GetInterface(IOpenableInterface::StaticClass())) - __int64(ABuildingWall::GetDefaultObj());

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (Object && Object->IsDefaultObject() && Object->IsA(ABuildingWall::StaticClass()))
        {
            auto OpenableVft = *(void***)(__int64(Object) + OpenableInterfaceOffset);

            VirtualSwap(OpenableVft, 0x1, Close_Implementation);
            VirtualSwap(OpenableVft, 0x2, Open_Implementation);
        }
    }

    HookServerOnAttemptInteract<ABuildingGameplayActorConversationCompatible>();
    HookServerOnAttemptInteract<ABuildingProp_ConversationCompatible>();
}
