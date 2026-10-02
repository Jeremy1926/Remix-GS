#include "pch.h"
#include "Shared.h"
#include "AFortPlayerController.h"
#include "AFortInventory.h"
#include "AFortPickup.h"
#include "UFortQuestManager.h"
#include "AFortAthenaAIBotController.h"
#include "UConversationInstance.h"

INIT_MODULE(AFortPlayerPawn);
INIT_POSTLOAD(AFortPlayerPawn);
INIT_TICKER(AFortPlayerPawn);

static bool (*FinishedTargetSplineOG)(AFortPickupAthena*);
static void (*OnCapsuleBeginOverlapOG)(UObject*, FFrame&, void*);
static void (*ServerOnExitVehicleOG)(UObject*, FFrame&, void*);
static void (*EmoteStoppedOG)(UObject*, FFrame&, void*);
static void (*EndSkydivingOG)(AFortPlayerPawnAthena*);
static void (*TriggerAbilityFromAnimationOG)(AFortPlayerPawnAthena*, UObject*, uint32, uint32, bool, uint32);
static bool (*ServerOnAttemptInteractOG)(void*, uint8*);

static int64 InteractInterfaceOffset = 0;
static std::vector<std::pair<TWeakObjectPtr<AFortPlayerPawnAthena>, TWeakObjectPtr<AActor>>> DBNORevivePawns;

static void ServerHandlePickupInfo(UObject* Context, FFrame& Stack)
{
    AFortPickupAthena* Pickup;
    FFortPickupRequestInfo Params;

    Stack.StepCompiledIn(&Pickup);
    Stack.StepCompiledIn(&Params);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;

    if (!Pawn || !Pickup || Pickup->bPickedUp)
        return;

    if (Params.bUseRequestedSwap && Pawn->Controller && Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()) && Pawn->CurrentWeapon && IsPrimaryQuickbar(Pawn->CurrentWeapon->WeaponData) &&
        IsPrimaryQuickbar((UFortItemDefinition*)Pickup->PrimaryPickupItemEntry.ItemDefinition))
        ((AFortPlayerControllerAthena*)Pawn->Controller)->bTryPickupSwap = true;

    static auto SetPickupTarget = (void (*)(AFortPickupAthena*, AFortPlayerPawnAthena*, float, FVector&, bool))(ImageBase + 0x9AC26B8);
    SetPickupTarget(Pickup, Pawn, Params.FlyTime / Pawn->PickupSpeedMultiplier, Params.Direction, Params.bPlayPickupSound);
}

static bool FinishedTargetSpline(AFortPickupAthena* Pickup)
{
    auto Pawn = (AFortPlayerPawnAthena*)Pickup->PickupLocationData.PickupTarget.Get();

    if (!Pawn)
        return FinishedTargetSplineOG(Pickup);

    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;

    if (!PlayerController || !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()))
    {
        if (PlayerController && PlayerController->IsA(AFortAthenaAIBotController::StaticClass()))
            return BotFinishedTargetSpline(Pickup, PlayerController, (bool (*)(void*))FinishedTargetSplineOG);

        return FinishedTargetSplineOG(Pickup);
    }

    if (!PlayerController->bTryPickupSwap)
    {
        InternalPickup(PlayerController, &Pickup->PrimaryPickupItemEntry);
        return FinishedTargetSplineOG(Pickup);
    }

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto FinalLoc = Pawn->K2_GetActorLocation();
    auto ForwardVector = Pawn->GetActorForwardVector();

    ForwardVector.Z = 0.0f;
    ForwardVector.Normalize();

    FinalLoc = FinalLoc + ForwardVector * 450.f;
    FinalLoc.Z += 50.f;

    auto FinalAngle = (((float)rand() * 0.00109866634f) - 18.f) * 0.017453292519943295f;

    FinalLoc.X += cos(FinalAngle) * 100.f;
    FinalLoc.Y += sin(FinalAngle) * 100.f;

    if (!IsPrimaryQuickbar(Pawn->CurrentWeapon->WeaponData) || !IsPrimaryQuickbar((UFortItemDefinition*)Pickup->PrimaryPickupItemEntry.ItemDefinition))
    {
        SpawnPickup(PlayerController->GetViewTarget()->K2_GetActorLocation(), Pickup->PrimaryPickupItemEntry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, PlayerController->MyFortPawn, -1, true,
                    true, true, nullptr, FinalLoc);
        return FinishedTargetSplineOG(Pickup);
    }

    PlayerController->bTryPickupSwap = false;

    auto CurrentWeapon = ((AFortPlayerPawnAthena*)PlayerController->Pawn)->CurrentWeapon;
    auto& ReplicatedEntries = WorldInventory->Inventory.ReplicatedEntries;
    FFortItemEntry* Entry = nullptr;

    for (int i = 0; i < ReplicatedEntries.Num(); i++)
    {
        if (ReplicatedEntries[i].ItemGuid == CurrentWeapon->ItemEntryGuid)
        {
            Entry = &ReplicatedEntries[i];
            break;
        }
    }

    SpawnPickup(PlayerController->GetViewTarget()->K2_GetActorLocation(), *Entry, EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, PlayerController->MyFortPawn, -1, true, true, true, nullptr,
                FinalLoc);
    Remove(WorldInventory, Entry->ItemGuid);

    auto Item = GiveItem(WorldInventory, Pickup->PrimaryPickupItemEntry);
    PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);

    return FinishedTargetSplineOG(Pickup);
}

static void ServerSendZiplineState(UObject* Context, FFrame& Stack)
{
    FZiplinePawnState State;

    Stack.StepCompiledIn(&State);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;

    if (!Pawn)
        return;

    auto Zipline = Pawn->GetActiveZipline();
    auto PreviousState = Pawn->ZiplineState;

    Pawn->ZiplineState = State;

    static auto OnRep_ZiplineState = (void (*)(AFortPlayerPawnAthena*))(ImageBase + 0x9F6E408);
    OnRep_ZiplineState(Pawn);

    auto NewZipline = Pawn->GetActiveZipline();

    if (State.bIsZiplining && (!PreviousState.bIsZiplining || NewZipline != Zipline))
        Pawn->ZiplinePostBegin.Process();

    if (State.bJumped)
    {
        auto VelocityX = Pawn->CharacterMovement->Velocity.X * -0.5f;
        auto VelocityY = Pawn->CharacterMovement->Velocity.Y * -0.5f;

        Pawn->LaunchCharacterJump(FVector{ (std::clamp)(VelocityX, -750., 750.), (std::clamp)(VelocityY, -750., 750.), 1200 }, false, false, true, true);
    }

    static auto ZiplineClass = FindObject<UClass>(L"/Ascender/Gameplay/Ascender/B_Athena_Zipline_Ascender.B_Athena_Zipline_Ascender_C");

    if (Zipline && Zipline->IsA(ZiplineClass))
    {
        auto Ascender = (AFortAscenderZipline*)Zipline;
        Ascender->PawnUsingHandle = nullptr;
        Ascender->PreviousPawnUsingHandle = Pawn;
        Ascender->OnRep_PawnUsingHandle();
    }
    else if (NewZipline && NewZipline->IsA(ZiplineClass))
    {
        auto Ascender = (AFortAscenderZipline*)NewZipline;
        Ascender->PawnUsingHandle = Pawn;
        Ascender->PreviousPawnUsingHandle = nullptr;
        Ascender->OnRep_PawnUsingHandle();
    }
}

static void OnCapsuleBeginOverlap(UObject* Context, FFrame& Stack)
{
    UPrimitiveComponent* OverlappedComp;
    AActor* OtherActor;
    UPrimitiveComponent* OtherComp;
    int32 OtherBodyIndex;
    bool bFromSweep;
    FHitResult SweepResult;

    Stack.StepCompiledIn(&OverlappedComp);
    Stack.StepCompiledIn(&OtherActor);
    Stack.StepCompiledIn(&OtherComp);
    Stack.StepCompiledIn(&OtherBodyIndex);
    Stack.StepCompiledIn(&bFromSweep);
    Stack.StepCompiledIn(&SweepResult);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;
    auto PlayerController = Pawn && Pawn->Controller && Pawn->Controller->IsA(AFortPlayerController::StaticClass()) ? (AFortPlayerControllerAthena*)Pawn->Controller : nullptr;
    auto Pickup = PlayerController && OtherActor && OtherActor->IsA(AFortPickupAthena::StaticClass()) ? (AFortPickupAthena*)OtherActor : nullptr;
    auto ItemDefinition = Pickup ? (UFortItemDefinition*)Pickup->PrimaryPickupItemEntry.ItemDefinition : nullptr;
    auto WorldInventory = PlayerController ? (AFortInventory*)PlayerController->WorldInventory.ObjectPointer : nullptr;

    if (ItemDefinition && WorldInventory && Pickup->PawnWhoDroppedPickup.Get() != Pawn)
    {
        auto MaxStack = ItemDefinition->GetMaxStackSize(nullptr);
        auto& ReplicatedEntries = WorldInventory->Inventory.ReplicatedEntries;
        FFortItemEntry* ItemEntry = nullptr;

        for (int i = 0; i < ReplicatedEntries.Num(); i++)
        {
            if (ReplicatedEntries[i].ItemDefinition == ItemDefinition && ReplicatedEntries[i].Count <= MaxStack)
            {
                ItemEntry = &ReplicatedEntries[i];
                break;
            }
        }

        if (ItemEntry ? ItemEntry->Count < MaxStack : !IsPrimaryQuickbar(ItemDefinition))
            Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
    }

    callOG(Pawn, Stack.CurrentNativeFunction, OnCapsuleBeginOverlap, OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);
}

static void MovingEmoteStopped(UObject* Context, FFrame& Stack)
{
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;
    auto OldEmote = Pawn->LastReplicatedEmoteExecuted;

    Pawn->bMovingEmote = false;
    Pawn->bMovingEmoteForwardOnly = false;
    Pawn->bMovingEmoteFollowingOnly = false;
    Pawn->LastReplicatedEmoteExecuted = nullptr;
    Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);
}

static void ServerOnExitVehicle(UObject* Context, FFrame& Stack, AFortAthenaVehicle** Ret)
{
    FVehicleExitData VehicleExitData;

    Stack.StepCompiledIn(&VehicleExitData);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;
    auto Vehicle = Pawn->GetVehicleActor();

    if (!Vehicle && Pawn->IsA(AFortCharacterVehicle::StaticClass()))
        Vehicle = Pawn;

    auto SeatWeaponComponent = Vehicle ? (UFortVehicleSeatWeaponComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatWeaponComponent::StaticClass()) : nullptr;

    if (!SeatWeaponComponent)
        return callOG(Pawn, Stack.CurrentNativeFunction, ServerOnExitVehicle, VehicleExitData);

    auto SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());
    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto& ReplicatedEntries = WorldInventory->Inventory.ReplicatedEntries;
    int32 SeatIndex = -1;
    UFortWeaponItemDefinition* Weapon = nullptr;

    for (int i = 0; i < SeatComponent->PlayerSlots.Num(); i++)
    {
        if (SeatComponent->PlayerSlots[i].Player == Pawn)
        {
            SeatIndex = i;
            break;
        }
    }

    for (auto& WeaponDefinition : SeatWeaponComponent->WeaponSeatDefinitions)
    {
        if (WeaponDefinition.SeatIndex == SeatIndex)
        {
            Weapon = WeaponDefinition.VehicleWeapon;
            break;
        }
    }

    if (Weapon)
    {
        for (int i = 0; i < ReplicatedEntries.Num(); i++)
        {
            if (ReplicatedEntries[i].ItemDefinition == Weapon)
            {
                Remove(WorldInventory, ReplicatedEntries[i].ItemGuid);
                break;
            }
        }
    }

    *Ret = callOGWithRet(Pawn, Stack.CurrentNativeFunction, ServerOnExitVehicle, VehicleExitData);

    if (!Weapon)
        return;

    if (auto LastItem = Pawn->PreviousWeapon)
    {
        PlayerController->ServerExecuteInventoryItem(LastItem->ItemEntryGuid);
        PlayerController->ClientEquipItem(LastItem->ItemEntryGuid, true);
        return;
    }

    for (int i = 0; i < ReplicatedEntries.Num(); i++)
    {
        if (ReplicatedEntries[i].ItemDefinition->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
        {
            PlayerController->ServerExecuteInventoryItem(ReplicatedEntries[i].ItemGuid);
            PlayerController->ClientEquipItem(ReplicatedEntries[i].ItemGuid, true);
            break;
        }
    }
}

static void EmoteStopped(UObject* Context, FFrame& Stack)
{
    UFortItemDefinition* MontageItemDef;

    Stack.StepCompiledIn(&MontageItemDef);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;

    if (Pawn->LastReplicatedEmoteExecuted == MontageItemDef)
    {
        Pawn->LastReplicatedEmoteExecuted = nullptr;
        Pawn->OnRep_LastReplicatedEmoteExecuted(MontageItemDef);
    }

    callOG(Pawn, Stack.CurrentNativeFunction, EmoteStopped, MontageItemDef);
}

static void EndSkydiving(AFortPlayerPawnAthena* Pawn)
{
    EndSkydivingOG(Pawn);

    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;

    if (!PlayerController || !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass()))
        return;

    if (Pawn->bIsSkydiving)
        SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::Land, 1, true);

    if (Pawn->bIsSkydivingFromBus)
        SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::Visit, 1, true);
}

static void SetDBNOBeingRevivedTag(AActor* AbilitySystemActor, bool bBeingRevived)
{
    static auto DBNOBeingRevivedTag = FGameplayTag{ FName(L"Gameplay.Status.DBNOBeingRevived") };

    FGameplayTagContainer DBNOBeingRevivedTags{};
    DBNOBeingRevivedTags.GameplayTags.Add(DBNOBeingRevivedTag);

    if (bBeingRevived)
        UAbilitySystemBlueprintLibrary::AddLooseGameplayTags(AbilitySystemActor, DBNOBeingRevivedTags, true);
    else
        UAbilitySystemBlueprintLibrary::RemoveLooseGameplayTags(AbilitySystemActor, DBNOBeingRevivedTags, true);

    DBNOBeingRevivedTags.GameplayTags.Free();
}

static void SetDBNOBeingRevivedCue(AActor* AbilitySystemActor, bool bBeingRevived, float ReviveTime = 0.f, AActor* RevivingPawn = nullptr)
{
    static auto BeingRevivedCueTag = FGameplayTag{ FName(L"GameplayCue.Athena.Player.BeingRevivedFromDBNO") };

    FGameplayCueParameters CueParameters{};
    CueParameters.RawMagnitude = ReviveTime;
    CueParameters.Instigator = RevivingPawn;
    CueParameters.EffectCauser = RevivingPawn;
    CueParameters.GameplayEffectLevel = 1;
    CueParameters.AbilityLevel = 1;

    if (bBeingRevived)
        UGameplayCueFunctionLibrary::AddGameplayCueOnActor(AbilitySystemActor, BeingRevivedCueTag, CueParameters);
    else
        UGameplayCueFunctionLibrary::RemoveGameplayCueOnActor(AbilitySystemActor, BeingRevivedCueTag, CueParameters);
}

static void BeginDBNOBeingRevived(AFortPlayerPawnAthena* Pawn, AActor* RevivingPawn)
{
    for (auto& RevivePawn : DBNORevivePawns)
        if (RevivePawn.first.Get() == Pawn)
            return;

    if (!Pawn->PlayerState)
        return;

    SetDBNOBeingRevivedTag(Pawn->PlayerState, true);
    SetDBNOBeingRevivedCue(Pawn->PlayerState, true, Pawn->GetReviveFromDBNOTime(), RevivingPawn);
    DBNORevivePawns.push_back({ Pawn, Pawn->PlayerState });
}

static void UpdateDBNORevivingActors(AFortPlayerPawnAthena* Pawn)
{
    std::vector<AFortPlayerPawnAthena*> RevivingPawns;

    if (Pawn->IsDBNO())
    {
        TArray<AFortPlayerControllerAthena*> PlayerControllers;
        GetAll<AFortPlayerControllerAthena>(PlayerControllers);

        for (auto PlayerController : PlayerControllers)
        {
            auto InteractionComponent = PlayerController ? (UFortControllerComponent_Interaction*)PlayerController->GetComponentByClass(UFortControllerComponent_Interaction::StaticClass()) : nullptr;
            auto RevivingPawn = InteractionComponent && InteractionComponent->LongInteractValidateInfo.Target == Pawn ? (AFortPlayerPawnAthena*)PlayerController->MyFortPawn : nullptr;

            if (RevivingPawn && RevivingPawn != Pawn && !RevivingPawn->IsDBNO())
                RevivingPawns.push_back(RevivingPawn);
        }

        PlayerControllers.Free();
    }

    if (Pawn->DBNORevivingActorsCount != (uint8)RevivingPawns.size())
    {
        Pawn->DBNORevivingActorsCount = (uint8)RevivingPawns.size();
        Pawn->ForceNetUpdate();
    }

    if (auto MultiInteractComponent = (UFortActorComponent_MultiInteract*)Pawn->GetComponentByClass(UFortActorComponent_MultiInteract::StaticClass()))
    {
        auto& InteractingPlayers = MultiInteractComponent->InteractingPlayers;

        for (int i = InteractingPlayers.Num() - 1; i >= 0; i--)
        {
            auto InteractingPlayer = InteractingPlayers[i];

            if (std::none_of(RevivingPawns.begin(), RevivingPawns.end(), [&](AFortPlayerPawnAthena* RevivingPawn) { return RevivingPawn->PlayerState == InteractingPlayer; }))
                MultiInteractComponent->RemoveInteractingActor(InteractingPlayer);
        }

        for (auto RevivingPawn : RevivingPawns)
            MultiInteractComponent->AddInteractingActor(RevivingPawn);
    }

    if (Pawn->ServerWorldTimeRevivalTime >= 0.f)
        return;

    if (!RevivingPawns.empty())
    {
        BeginDBNOBeingRevived(Pawn, RevivingPawns[0]);
        return;
    }

    for (size_t i = 0; i < DBNORevivePawns.size(); i++)
    {
        if (DBNORevivePawns[i].first.Get() != Pawn)
            continue;

        if (auto AbilitySystemActor = DBNORevivePawns[i].second.Get())
        {
            SetDBNOBeingRevivedCue(AbilitySystemActor, false);
            SetDBNOBeingRevivedTag(AbilitySystemActor, false);
        }

        DBNORevivePawns.erase(DBNORevivePawns.begin() + i);
        return;
    }
}

static void FinishDBNORevive(AFortPlayerPawnAthena* Pawn)
{
    Pawn->ServerWorldTimeRevivalTime = -1.f;
    Pawn->ForceNetUpdate();

    auto PlayerState = Pawn->PlayerState && Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;

    if (!Pawn->IsDBNO() && Pawn->Controller && PlayerState && PlayerState->AbilitySystemComponent)
    {
        static auto DBNOTag = FGameplayTag{ FName(L"Gameplay.Action.Player.DBNO") };
        static auto DBNOAthenaTag = FGameplayTag{ FName(L"Gameplay.Action.Player.DBNOAthena") };

        FGameplayTagContainer DBNOTags{};
        DBNOTags.GameplayTags.Add(DBNOTag);
        DBNOTags.GameplayTags.Add(DBNOAthenaTag);

        PlayerState->AbilitySystemComponent->BP_CancelAbilitiesWithTags(DBNOTags);

        DBNOTags.GameplayTags.Free();
    }

    UpdateDBNORevivingActors(Pawn);
}

static FGameplayTagContainer GetOwnedGameplayTags(AActor* Actor)
{
    FGameplayTagContainer OwnedTags{};

    if (auto Interface = Actor ? Actor->GetInterface(IGameplayTagAssetInterface::StaticClass()) : nullptr)
        ((void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2])(Interface, &OwnedTags);

    return OwnedTags;
}

static void ReviveFromDBNO(AFortPlayerPawnAthena* Pawn, AActor* EventInstigator, bool bForceRevive)
{
    auto PlayerState = Pawn->PlayerState && Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;
    auto AbilitySystemComponent = PlayerState ? PlayerState->AbilitySystemComponent : nullptr;

    if (!Pawn->IsDBNO() || !AbilitySystemComponent)
        return;

    auto ReviverController = EventInstigator && EventInstigator->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)EventInstigator : nullptr;
    auto ReviverPawn = ReviverController ? ReviverController->MyFortPawn : nullptr;
    auto PlayerController = Pawn->Controller && Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)Pawn->Controller : nullptr;
    auto GameMode = (AFortGameModeZone*)UWorld::GetWorld()->AuthorityGameMode;
    auto ReviveGameplayEffect = GameMode ? GameMode->GetTeammateReviveGameplayEffectOverride() : TSubclassOf<UGameplayEffect>();

    if (!ReviveGameplayEffect)
        ReviveGameplayEffect = Pawn->TeammateReviveGameplayEffect;

    if (ReviveGameplayEffect)
    {
        auto SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(ReviveGameplayEffect, 1.f, FGameplayEffectContextHandle{});
        auto ReviveSpecHandle = UAbilitySystemBlueprintLibrary::AssignSetByCallerMagnitude(SpecHandle, FName(L"ReviveHeal"), Pawn->SetByCallerReviveHealth.Evaluate() - Pawn->GetHealth());

        AbilitySystemComponent->BP_ApplyGameplayEffectSpecToSelf(ReviveSpecHandle);

        ReviveSpecHandle.Release();
        SpecHandle.Release();
    }

    ((void (*)(AFortPlayerPawnAthena*, bool))Pawn->VTable[0x199])(Pawn, false);

    static auto UpdateDBNOInteractCollision = (void (*)(AFortPlayerPawnAthena*))(ImageBase + 0x19751A4);
    UpdateDBNOInteractCollision(Pawn);

    memset((PBYTE)&PlayerState->DeathInfo, 0, sizeof(FDeathInfo));
    PlayerState->OnRep_DeathInfo();
    PlayerState->ForceNetUpdate();

    if (bForceRevive)
    {
        FinishDBNORevive(Pawn);

        if (PlayerController)
            PlayerController->ClientOnPawnRevived((AController*)EventInstigator);

        return;
    }

    static auto RevivedTag = FGameplayTag{ FName(L"Event.RevivedAthena") };

    FGameplayEventData EventData{};
    EventData.EventTag = RevivedTag;
    EventData.Instigator = ReviverPawn;
    EventData.Target = Pawn;

    UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Pawn, RevivedTag, EventData);

    BeginDBNOBeingRevived(Pawn, ReviverPawn);

    for (auto& RevivePawn : DBNORevivePawns)
    {
        if (RevivePawn.first.Get() == Pawn)
        {
            if (auto AbilitySystemActor = RevivePawn.second.Get())
                SetDBNOBeingRevivedCue(AbilitySystemActor, false);

            break;
        }
    }

    Pawn->ServerWorldTimeRevivalTime = (float)UGameplayStatics::GetTimeSeconds(Pawn) + Pawn->ConvertFromDBNOTime;
    Pawn->ForceNetUpdate();

    if (PlayerController)
        PlayerController->ClientOnPawnRevived((AController*)EventInstigator);

    if (ReviverController)
    {
        auto TargetTags = GetOwnedGameplayTags(Pawn);
        SendStatEvent(ReviverController->GetQuestManager(ESubGame::Athena), ReviverController, EFortQuestObjectiveStatEvent::RevivePlayer, 1, false, Pawn, TargetTags);
        TargetTags.GameplayTags.Free();
        TargetTags.ParentTags.Free();
    }

    if (PlayerController)
    {
        auto TargetTags = GetOwnedGameplayTags(ReviverPawn);
        SendStatEvent(PlayerController->GetQuestManager(ESubGame::Athena), PlayerController, EFortQuestObjectiveStatEvent::RevivedFromDBNO, 1, false, ReviverPawn, TargetTags);
        TargetTags.GameplayTags.Free();
        TargetTags.ParentTags.Free();
    }
}

static void ServerReviveFromDBNO(AFortPlayerPawnAthena* Pawn, AActor* EventInstigator)
{
    ReviveFromDBNO(Pawn, EventInstigator, false);
}

static void ServerReviveFromDBNO(UObject* Context, FFrame& Stack)
{
    AFortPlayerControllerAthena* EventInstigator;

    Stack.StepCompiledIn(&EventInstigator);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;

    if (Pawn->Controller && !Pawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()))
        ReviveFromDBNO(Pawn, EventInstigator, false);
}

static void ForceReviveFromDBNO(UObject* Context, FFrame& Stack)
{
    AActor* EventInstigator;

    Stack.StepCompiledIn(&EventInstigator);
    Stack.IncrementCode();

    ReviveFromDBNO((AFortPlayerPawnAthena*)Context, EventInstigator, true);
}

static void PlayGroupEmote(UObject* Context, FFrame& Stack)
{
    UObject* EmoteAsset;

    Stack.StepCompiledIn(&EmoteAsset);
    Stack.IncrementCode();

    auto Pawn = (AFortPlayerPawnAthena*)Context;
    auto DanceAsset = EmoteAsset && EmoteAsset->IsA(UAthenaDanceItemDefinition::StaticClass()) ? (UAthenaDanceItemDefinition*)EmoteAsset : nullptr;
    auto PlayerState = Pawn->PlayerState && Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;
    auto AbilitySystemComponent = PlayerState ? PlayerState->AbilitySystemComponent : nullptr;

    if (!DanceAsset || !AbilitySystemComponent)
        return;

    static auto EmoteAbilityClass = FindObject<UClass>(L"/Game/Abilities/Emotes/GAB_Emote_Generic.GAB_Emote_Generic_C");
    auto CustomAbility = DanceAsset->CustomDanceAbility.Get();
    auto AbilityClass = CustomAbility ? CustomAbility : EmoteAbilityClass;

    if (!AbilityClass)
        return;

    Pawn->bMovingEmote = DanceAsset->bMovingEmote;
    Pawn->EmoteWalkSpeed = DanceAsset->WalkForwardSpeed;
    Pawn->bMovingEmoteForwardOnly = DanceAsset->bMoveForwardOnly;
    Pawn->bMovingEmoteFollowingOnly = DanceAsset->bMoveFollowingOnly;

    static auto ConstructAbilitySpec = (void (*)(FGameplayAbilitySpec*, const UObject*, int, int, UObject*))(ImageBase + 0x7B33CD8);
    static auto GiveAbilityAndActivateOnce = (void (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle*, FGameplayAbilitySpec*, void*))(ImageBase + 0x7AF6ED8);

    auto Spec = (FGameplayAbilitySpec*)calloc(1, sizeof(FGameplayAbilitySpec));
    FGameplayAbilitySpecHandle Handle;

    ConstructAbilitySpec(Spec, AbilityClass->ClassDefaultObject, 1, -1, EmoteAsset);
    GiveAbilityAndActivateOnce(AbilitySystemComponent, &Handle, Spec, nullptr);

    free(Spec);

    auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
    Pawn->LastReplicatedEmoteExecuted = (UFortItemDefinition*)EmoteAsset;
    Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);
}

static void ServerNotifyStartLongUse(void* InteractInterface, AFortPlayerPawnAthena* InteractingPawn)
{
    auto Pawn = (AFortPlayerPawnAthena*)(__int64(InteractInterface) - InteractInterfaceOffset);

    if (Pawn->IsDBNO())
        UpdateDBNORevivingActors(Pawn);
}

static void ServerNotifyEndLongUse(void* InteractInterface, AFortPlayerPawnAthena* InteractingPawn, bool bUseCompleted)
{
    auto Pawn = (AFortPlayerPawnAthena*)(__int64(InteractInterface) - InteractInterfaceOffset);

    if (Pawn->DBNORevivingActorsCount > 0)
        UpdateDBNORevivingActors(Pawn);
}

static uint8* SetNum(FMinimalGameplayCueReplicationProxy* Proxy, uint32 InlineOffset, uint32 NumOffset, int32 ElementSize, int32 NewNum)
{
    auto Base = (uint8*)Proxy;
    auto& SecondaryData = *(uint8**)(Base + NumOffset - 8);
    auto& ArrayNum = *(int32*)(Base + NumOffset);
    auto& ArrayMax = *(int32*)(Base + NumOffset + 4);

    if (NewNum > ArrayMax)
    {
        SecondaryData = (uint8*)FMemory::Realloc(SecondaryData, uint64(NewNum) * ElementSize);
        ArrayMax = NewNum;
    }

    ArrayNum = NewNum;

    return SecondaryData ? SecondaryData : Base + InlineOffset;
}

static bool PreReplication(FMinimalGameplayCueReplicationProxy* Proxy, FActiveGameplayCueContainer& SourceContainer)
{
    auto& LastSourceArrayReplicationKey = *(int32*)(uint64(Proxy) + 0x2C8);

    if (LastSourceArrayReplicationKey == SourceContainer.ArrayReplicationKey)
        return false;

    LastSourceArrayReplicationKey = SourceContainer.ArrayReplicationKey;

    auto& GameplayCues = SourceContainer.GameplayCues;
    auto ReplicatedTags = (FGameplayTag*)SetNum(Proxy, 0x40, 0x88, sizeof(FGameplayTag), GameplayCues.Num());
    auto ReplicatedLocations = (FVector*)SetNum(Proxy, 0x90, 0x218, sizeof(FVector), GameplayCues.Num());

    for (int32 i = 0; i < GameplayCues.Num(); i++)
    {
        auto& Parameters = GameplayCues[i].Parameters;

        ReplicatedTags[i] = GameplayCues[i].GameplayCueTag;
        ReplicatedLocations[i] = Parameters.bReplicateLocationWhenUsingMinimalRepProxy ? (FVector)Parameters.Location : FVector();
    }

    return true;
}

static void TriggerAbilityFromAnimation(AFortPlayerPawnAthena* Pawn, UObject* Animation, uint32 ApplicationTag, uint32 MontageSectionIfBlocked, bool bSetNextSectionIfBlocked, uint32 MontageSectionIfTargetInvalid)
{
    TriggerAbilityFromAnimationOG(Pawn, Animation, ApplicationTag, MontageSectionIfBlocked, bSetNextSectionIfBlocked, MontageSectionIfTargetInvalid);

    if (!Animation || Pawn->Role != ENetRole::ROLE_Authority || Pawn->IsLocallyControlled() || Pawn->IsBotControlled())
        return;

    auto AbilitySystemComponent = Pawn->AbilitySystemComponent;
    UFortGameplayAbility* Ability = nullptr;

    for (int i = 0; AbilitySystemComponent && !Ability && i < AbilitySystemComponent->ActivatableAbilities.Items.Num(); i++)
    {
        auto& Spec = AbilitySystemComponent->ActivatableAbilities.Items[i];

        for (auto Instances : { &Spec.ReplicatedInstances, &Spec.NonReplicatedInstances })
        {
            for (auto Instance : *Instances)
            {
                if (!Ability && Instance && Instance->IsA(UFortGameplayAbility::StaticClass()) && Instance->CurrentMontage == Animation)
                    Ability = (UFortGameplayAbility*)Instance;
            }
        }
    }

    if (!Ability || Ability->NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::LocalPredicted)
        return;

    for (auto Task : Ability->ActiveTasks)
    {
        if (Task && Task->IsA(UFortAbilityTask_PlayMontageWaitTarget::StaticClass()) && *(EFortAbilityTargetDataPolicy*)(uint64(Task) + 0x100) == EFortAbilityTargetDataPolicy::SimulateOnServer)
        {
            ((void (*)(UFortGameplayAbility*, UObject*, uint32))Ability->VTable[0xAA])(Ability, AbilitySystemComponent, ApplicationTag);
            return;
        }
    }
}

static void StartCarryingPlayer(AFortPlayerPawnAthena* Pawn, AFortPlayerPawnAthena* InDBNOHoistee, EFortDBNOCarryEvent CarryEvent)
{
    Pawn->DBNOHoistee = InDBNOHoistee;
    Pawn->OnRep_DBNOHoistee();

    auto PreviousHoisterData = InDBNOHoistee->DBNOHoisterData;

    InDBNOHoistee->DBNOHoisterData.DBNOHoister = Pawn;
    InDBNOHoistee->DBNOHoisterData.DBNOCarryEvent = CarryEvent;
    InDBNOHoistee->OnRep_DBNOHoisterData(PreviousHoisterData);

    InDBNOHoistee->ForceNetUpdate();
    Pawn->ForceNetUpdate();
}

static void ServerHoistDBNOPlayer(AFortPlayerPawnAthena* Pawn, AFortPlayerPawnAthena* InDBNOHoistee)
{
    if (InDBNOHoistee && ((bool (*)(AFortPlayerPawnAthena*, AActor*))InDBNOHoistee->VTable[0x2B9])(InDBNOHoistee, Pawn->Controller))
        StartCarryingPlayer(Pawn, InDBNOHoistee, EFortDBNOCarryEvent::PickedUp);
}

static void ServerInterrogateDBNOPlayer(AFortPlayerPawnAthena* Pawn, AFortPlayerPawnAthena* InDBNOHoistee)
{
    if (InDBNOHoistee && InDBNOHoistee->CanInterrogateFromDBNO(Pawn->Controller))
        StartCarryingPlayer(Pawn, InDBNOHoistee, EFortDBNOCarryEvent::Interrogating);
}

static void SetIsInsideSafeZone(AFortPlayerPawnAthena* Pawn, bool bNewValue)
{
    if (Pawn->bIsInsideSafeZone == bNewValue)
        return;

    Pawn->bIsInsideSafeZone = bNewValue;
    Pawn->OnRep_IsInsideSafeZone();
}

static void UpdateOutsideSafeZone(AFortPlayerPawnAthena* Pawn)
{
    SetIsInsideSafeZone(Pawn, !Pawn->bIsInAnyStorm);
}

static bool ServerOnAttemptInteract(void* InteractInterface, uint8* Request)
{
    auto Result = ServerOnAttemptInteractOG(InteractInterface, Request);

    if (Request[1] != 0 || Request[0] != 1 || !((bool (*)(void*, uint8*))(*(void***)InteractInterface)[0x6])(InteractInterface, Request))
        return Result;

    static auto GetValidInteractionRequestController = (AActor* (*)(uint8*))(ImageBase + 0x98851A4);
    static auto GetNPCConversationComponentForInteraction = (bool (*)(AFortPlayerPawnAthena*, AActor*, UObject**))(ImageBase + 0x9F58698);
    static auto GetInteractionRequestController = (AActor* (*)(uint8*))(ImageBase + 0x988316C);

    auto Pawn = (AFortPlayerPawnAthena*)(__int64(InteractInterface) - InteractInterfaceOffset);
    auto Controller = GetValidInteractionRequestController(Request);
    UObject* ConversationComponent = nullptr;

    if (!Controller || !GetNPCConversationComponentForInteraction(Pawn, Controller, &ConversationComponent) || !ConversationComponent)
        return Result;

    if (auto Instigator = GetInteractionRequestController(Request))
    {
        auto NPCConversationComponent = (UFortNonPlayerConversationParticipantComponent*)ConversationComponent;
        StartConversation(NPCConversationComponent, NPCConversationComponent->ConversationEntryTag, Instigator, Pawn);
    }

    return Result;
}

static void SetupOwningPawn(UFortHeldObjectComponent* HeldObjectComponent, AFortPlayerPawn* Pawn)
{
    auto HeldObject = HeldObjectComponent->GetOwner();

    if (!HeldObject)
        return;

    HeldObject->FlushNetDormancy();

    if (Pawn)
        HeldObjectComponent->HeldObjectState = EHeldObjectState::Held;

    auto PreviousPawn = HeldObjectComponent->OwningPawn;

    HeldObjectComponent->PreviousOwningPawn = !Pawn ? HeldObjectComponent->OwningPawn : nullptr;
    HeldObjectComponent->OwningPawn = Pawn;

    if (Pawn)
    {
        Pawn->HeldObject = HeldObject;
        ((AFortPlayerControllerAthena*)Pawn->Controller)->bHoldingObject = true;
    }

    HeldObject->ForceNetUpdate();
    HeldObjectComponent->OnHeldObjectOwningPawnChanged.Process();

    if (Pawn)
        Pawn->OnHeldObjectPickedUp.Process(HeldObject);
    else
        PreviousPawn->OnHeldObjectDropped.Process(HeldObject);
}

static void PickupHeldObject(UObject* Context, FFrame& Stack)
{
    AFortPlayerPawn* PlayerPawn;

    Stack.StepCompiledIn(&PlayerPawn);
    Stack.IncrementCode();

    auto HeldObjectComponent = (UFortHeldObjectComponent*)Context;
    auto PlayerController = (AFortPlayerControllerAthena*)PlayerPawn->Controller;

    SetupOwningPawn(HeldObjectComponent, PlayerPawn);

    if (!HeldObjectComponent->GrantedWeaponItem.Get())
    {
        auto ItemDefinition = HeldObjectComponent->EquippedWeaponItemDefinition.Get();
        auto Item = GiveItem((AFortInventory*)PlayerController->WorldInventory.ObjectPointer, ItemDefinition, 1, 99999);

        if (!Item)
            return;

        auto Weapon = PlayerPawn->EquipWeaponDefinition(ItemDefinition, Item->ItemEntry.ItemGuid, Item->ItemEntry.TrackerGuid, false);

        if (!Weapon)
            return;

        PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);

        HeldObjectComponent->GrantedWeapon = Weapon;
        HeldObjectComponent->GrantedWeaponItem = Item;
    }

    HeldObjectComponent->OnHeldObjectPickedUp.Process();
}

static void PlaceHeldObject(UObject* Context, FFrame& Stack)
{
    Stack.IncrementCode();
}

static void ThrowHeldObject(UObject* Context, FFrame& Stack)
{
    FVector DetachLocation;
    FRotator ThrowDirection;

    Stack.StepCompiledIn(&DetachLocation);
    Stack.StepCompiledIn(&ThrowDirection);
    Stack.IncrementCode();
}

static void DropHeldObject(UObject* Context, FFrame& Stack)
{
    Stack.IncrementCode();

    auto HeldObjectComponent = (UFortHeldObjectComponent*)Context;
    auto OwningPawn = HeldObjectComponent->OwningPawn;

    if (!OwningPawn)
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)OwningPawn->Controller;

    HeldObjectComponent->HeldObjectState = EHeldObjectState::Dropped;

    for (auto& Entry : ((AFortInventory*)PlayerController->WorldInventory.ObjectPointer)->Inventory.ReplicatedEntries)
    {
        if (Entry.ItemGuid != OwningPawn->PreviousWeapon->ItemEntryGuid)
            continue;

        auto Weapon = OwningPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Entry.ItemDefinition, Entry.ItemGuid, Entry.TrackerGuid, false);
        PlayerController->ClientEquipItem(Weapon->ItemEntryGuid, true);
        break;
    }

    SetupOwningPawn(HeldObjectComponent, nullptr);
    HeldObjectComponent->OnHeldObjectDropped.Process();
}

static void* (*InternalConvertPawnOG)(UFortPawnComponent_Convert*, AFortPlayerPawnAthena*, UFortPawnComponent_Convert*);

static void* InternalConvertPawn(UFortPawnComponent_Convert* ConverterComponent, AFortPlayerPawnAthena* ConvertedPawn, UFortPawnComponent_Convert* ConvertedComponent)
{
    if (ConverterComponent && ConvertedComponent && ConvertedPawn && ConvertedPawn->IsA(AFortPlayerPawnAthena::StaticClass()) && ConvertedPawn->IsDBNO())
    {
        auto ConvertedType = ConvertedComponent->ConvertedType;
        auto MaxConverted = ConvertedType != EConvertedType::NPC ? ConverterComponent->GetMaxConvertedCreature() : ConverterComponent->GetMaxConvertedNPC();

        if (ConvertedType != EConvertedType::Unknown && MaxConverted > 0)
        {
            ConvertedPawn->ForceReviveFromDBNO(nullptr);
            ConvertedPawn->SetHealth(ConvertedPawn->GetMaxHealth());
            ConvertedPawn->UnCrouch(false);
        }
    }

    return InternalConvertPawnOG(ConverterComponent, ConvertedPawn, ConvertedComponent);
}

static void TeleportPlayerPawn(UObject* Context, FFrame& Stack, bool* Ret)
{
    UObject* WorldContextObject;
    AFortPlayerPawn* PlayerPawn;
    FVector DestLocation;
    FRotator DestRotation;
    bool bIgnoreCollision;
    bool bIgnoreSupplementalKillVolumeSweep;

    Stack.StepCompiledIn(&WorldContextObject);
    Stack.StepCompiledIn(&PlayerPawn);
    Stack.StepCompiledIn(&DestLocation);
    Stack.StepCompiledIn(&DestRotation);
    Stack.StepCompiledIn(&bIgnoreCollision);
    Stack.StepCompiledIn(&bIgnoreSupplementalKillVolumeSweep);
    Stack.IncrementCode();

    if (auto Vehicle = PlayerPawn->GetVehicleActor())
        Vehicle->K2_TeleportTo(DestLocation, DestRotation);

    PlayerPawn->K2_TeleportTo(DestLocation, DestRotation);
    *Ret = true;
}

void AFortPlayerPawn__Init()
{
    Hook(ImageBase + 0x94EA094, InternalConvertPawn, InternalConvertPawnOG);
}

void AFortPlayerPawn__PostLoad()
{
    auto PawnClass = AFortPlayerPawnAthena::StaticClass();
    auto DefaultPawn = AFortPlayerPawnAthena::GetDefaultObj();
    auto InteractInterface = DefaultPawn->GetInterface(IFortInteractInterface::StaticClass());
    auto InteractVft = *(void***)InteractInterface;

    InteractInterfaceOffset = __int64(InteractInterface) - __int64(DefaultPawn);

    ExecHook(PawnClass->FindFunction("ServerHandlePickupInfo"), ServerHandlePickupInfo);
    Hook(ImageBase + 0x2F38E28, FinishedTargetSpline, FinishedTargetSplineOG);
    ExecHook(PawnClass->FindFunction("OnCapsuleBeginOverlap"), OnCapsuleBeginOverlap, OnCapsuleBeginOverlapOG);
    ExecHook(PawnClass->FindFunction("ServerSendZiplineState"), ServerSendZiplineState);
    ExecHook(PawnClass->FindFunction("MovingEmoteStopped"), MovingEmoteStopped);
    ExecHook(PawnClass->FindFunction("ServerOnExitVehicle"), ServerOnExitVehicle, ServerOnExitVehicleOG);
    ExecHook(PawnClass->FindFunction("EmoteStopped"), EmoteStopped, EmoteStoppedOG);
    Hook<AFortPlayerPawnAthena>(uint32(0x2C7), EndSkydiving, EndSkydivingOG);
    Hook<AFortPlayerPawnAthena>(uint32(0x23A), (void (*)(AFortPlayerPawnAthena*, AActor*))ServerReviveFromDBNO);
    ExecHook(PawnClass->FindFunction("ForceReviveFromDBNO"), ForceReviveFromDBNO);
    ExecHook(PawnClass->FindFunction("PlayGroupEmote"), PlayGroupEmote);
    Hook<AFortPlayerPawnAthena>(uint32(0x23E), ServerHoistDBNOPlayer);
    Hook<AFortPlayerPawnAthena>(uint32(0x23D), ServerInterrogateDBNOPlayer);
    VirtualSwap(InteractVft, 0x10, ServerNotifyEndLongUse);
    VirtualSwap(InteractVft, 0x11, ServerNotifyStartLongUse);

    ServerOnAttemptInteractOG = decltype(ServerOnAttemptInteractOG)(InteractVft[0x15]);
    VirtualSwap(InteractVft, 0x15, ServerOnAttemptInteract);

    ExecHook(PawnClass->FindFunction("ServerReviveFromDBNO"), (void (*)(UObject*, FFrame&))ServerReviveFromDBNO);
    Hook<AFortPlayerPawnAthena>(uint32(0x1F9), TriggerAbilityFromAnimation, TriggerAbilityFromAnimationOG);
    Hook<AFortPlayerPawnAthena>(uint32(0x271), SetIsInsideSafeZone);
    Hook<AFortPlayerPawnAthena>(uint32(0x272), UpdateOutsideSafeZone);

    auto HeldObjectComponentClass = UFortHeldObjectComponent::StaticClass();

    ExecHook(HeldObjectComponentClass->FindFunction("PickupHeldObject"), PickupHeldObject);
    ExecHook(HeldObjectComponentClass->FindFunction("DropHeldObject"), DropHeldObject);
    ExecHook(HeldObjectComponentClass->FindFunction("PlaceHeldObject"), PlaceHeldObject);
    ExecHook(HeldObjectComponentClass->FindFunction("ThrowHeldObject"), ThrowHeldObject);
    ExecHook(UFortMissionLibrary::StaticClass()->FindFunction("TeleportPlayerPawn"), TeleportPlayerPawn);
}

void AFortPlayerPawn__Tick()
{
    if (!DBNORevivePawns.empty())
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        auto RevivePawns = DBNORevivePawns;

        for (auto& RevivePawn : RevivePawns)
        {
            auto Pawn = RevivePawn.first.Get();

            if (!Pawn)
            {
                if (auto AbilitySystemActor = RevivePawn.second.Get())
                {
                    SetDBNOBeingRevivedCue(AbilitySystemActor, false);
                    SetDBNOBeingRevivedTag(AbilitySystemActor, false);
                }
            }
            else if (Pawn->ServerWorldTimeRevivalTime < 0.f)
                UpdateDBNORevivingActors(Pawn);
            else if (Time >= Pawn->ServerWorldTimeRevivalTime)
                FinishDBNORevive(Pawn);
        }

        std::erase_if(DBNORevivePawns, [](auto& RevivePawn) { return !RevivePawn.first.Get(); });
    }

    static auto SimulatedProxyActiveGameplayCuesName = FName(L"SimulatedProxyActiveGameplayCues");
    static auto SimulatedProxyMinimalReplicationGameplayCuesName = FName(L"SimulatedProxyMinimalReplicationGameplayCues");

    TArray<AFortPlayerPawnAthena*> Pawns;
    GetAll<AFortPlayerPawnAthena>(Pawns);

    for (auto Pawn : Pawns)
    {
        auto PlayerState = Pawn && Pawn->PlayerState && Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;
        auto AbilitySystemComponent = PlayerState ? PlayerState->AbilitySystemComponent : nullptr;

        if (!AbilitySystemComponent)
            continue;

        if (PreReplication(&Pawn->SimulatedProxyActiveGameplayCues, AbilitySystemComponent->ActiveGameplayCues))
            UNetPushModelHelpers::MarkPropertyDirty(Pawn, SimulatedProxyActiveGameplayCuesName);

        if (PreReplication(&Pawn->SimulatedProxyMinimalReplicationGameplayCues, AbilitySystemComponent->MinimalReplicationGameplayCues))
            UNetPushModelHelpers::MarkPropertyDirty(Pawn, SimulatedProxyMinimalReplicationGameplayCuesName);
    }

    Pawns.Free();
}
