#include "pch.h"
#include "Shared.h"
#include "AFortWeapon.h"
#include "AFortInventory.h"

INIT_POSTLOAD(AFortWeapon);

static void ServerNotifyPawnHit(UObject* Context, FFrame& Stack, void* Ret)
{
    FHitResult Hit{};
    FVector ProjectileOriginPosition;
    float ProjectileStartTimestamp;
    TArray<uint8> ProjectileContext;
    FVector LocalSpaceImpactPoint;
    FVector LocalSpaceImpactNormal;
    bool bWasTargetingWhenProjectileFired;
    UObject* HitEntity;
    Stack.StepCompiledIn(&Hit);
    Stack.StepCompiledIn(&ProjectileOriginPosition);
    Stack.StepCompiledIn(&ProjectileStartTimestamp);
    Stack.StepCompiledIn(&ProjectileContext);
    Stack.StepCompiledIn(&LocalSpaceImpactPoint);
    Stack.StepCompiledIn(&LocalSpaceImpactNormal);
    Stack.StepCompiledIn(&bWasTargetingWhenProjectileFired);
    Stack.StepCompiledIn(&HitEntity);
    Stack.IncrementCode();

    ProjectileContext.Free();

    auto Weapon = (AFortWeaponRanged*)Context;

    if (!Hit.HitObjectHandle.ReferenceObject.Get())
        return;

    auto Pawn = Weapon->Owner && Weapon->Owner->IsA(AFortPlayerPawnAthena::StaticClass()) ? (AFortPlayerPawnAthena*)Weapon->Owner : nullptr;
    if (!Pawn || !Pawn->PlayerState)
        return;

    auto PlayerState = Pawn->PlayerState->IsA(AFortPlayerStateAthena::StaticClass()) ? (AFortPlayerStateAthena*)Pawn->PlayerState : nullptr;
    if (!PlayerState || !PlayerState->AbilitySystemComponent)
        return;

    int32 Level = 1;
    UGameplayAbility* Ability = nullptr;

    for (auto& Spec : PlayerState->AbilitySystemComponent->ActivatableAbilities.Items)
    {
        if (Spec.Handle.Handle != Weapon->PrimaryAbilitySpecHandle.Handle)
            continue;

        Level = Spec.Level;

        for (auto& Instance : Spec.NonReplicatedInstances)
        {
            if (Instance)
            {
                Ability = Instance;
                break;
            }
        }

        if (!Ability)
        {
            for (auto& Instance : Spec.ReplicatedInstances)
            {
                if (Instance)
                {
                    Ability = Instance;
                    break;
                }
            }
        }

        if (!Ability)
            Ability = Spec.ability;

        break;
    }

    if (Ability)
    {
        static auto HitApplicationTag = FGameplayTag{ FName(L"Gameplay.Application.Hit") };
        auto TargetData = UAbilitySystemBlueprintLibrary::AbilityTargetDataFromHitResult(Hit);
        ((UFortGameplayAbility*)Ability)->ApplyGameplayEffectContainer(TargetData, HitApplicationTag, Level);
    }
}

static void ServerSetScopedThirdPersonTargeting(UObject* Context, FFrame& Stack, void* Ret)
{
    uint8 bNewUseScopedThirdPersonTargeting;
    Stack.StepCompiledIn(&bNewUseScopedThirdPersonTargeting);
    Stack.IncrementCode();

    auto Weapon = (AFortWeaponRanged*)Context;

    if (!Weapon->OpticDefinition || !Weapon->OpticDefinition->bUsesFirstPersonAiming)
        return;

    static auto bUseScopedThirdPersonTargetingName = FName(L"bUseScopedThirdPersonTargeting");
    Weapon->bUseScopedThirdPersonTargeting = bNewUseScopedThirdPersonTargeting != 0;
    UNetPushModelHelpers::MarkPropertyDirty(Weapon, bUseScopedThirdPersonTargetingName);
    Weapon->SetFirstPersonTargeting(Weapon->CanUseScopedCameraViaOptic());
}

static void ServerSpawnDeco(UObject* Context, FFrame& Stack, void* Ret)
{
    FVector Location;
    FRotator Rotation;
    ABuildingSMActor* AttachedActor;
    EBuildingAttachmentType InBuildingAttachmentType;
    Stack.StepCompiledIn(&Location);
    Stack.StepCompiledIn(&Rotation);
    Stack.StepCompiledIn(&AttachedActor);
    Stack.StepCompiledIn(&InBuildingAttachmentType);
    Stack.IncrementCode();
    auto DecoTool = (AFortDecoTool*)Context;

    auto Pawn = (AFortPlayerPawnAthena*)DecoTool->Owner;
    if (!Pawn)
        return;
    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
    if (!PlayerController)
        return;

    auto ItemDefinition = (UFortDecoItemDefinition*)DecoTool->ItemDefinition;

    if (DecoTool->IsA(AFortDecoTool_ContextTrap::StaticClass()))
    {
        auto ContextTrapItemDefinition = ((AFortDecoTool_ContextTrap*)DecoTool)->ContextTrapItemDefinition;

        switch ((int)InBuildingAttachmentType)
        {
        case 0:
        case 6:
            ItemDefinition = ContextTrapItemDefinition->FloorTrap;
            break;
        case 7:
        case 2:
            ItemDefinition = ContextTrapItemDefinition->CeilingTrap;
            break;
        case 1:
            ItemDefinition = ContextTrapItemDefinition->WallTrap;
            break;
        case 8:
            ItemDefinition = ContextTrapItemDefinition->StairTrap;
            break;
        }
    }

    if (!ItemDefinition)
        return;

    auto ShouldAllowServerSpawnDeco = (bool (*)(AFortDecoTool*, FVector&, FRotator&, ABuildingSMActor*, EBuildingAttachmentType))DecoTool->VTable[0x1A3];

    if (!ShouldAllowServerSpawnDeco(DecoTool, Location, Rotation, AttachedActor, InBuildingAttachmentType))
        return;

    auto SpawnDeco = (ABuildingSMActor* (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&, FVector&, FRotator&, ABuildingSMActor*, EBuildingAttachmentType, int))DecoTool->VTable[0x198];

    TSubclassOf<ABuildingSMActor> DecoClass = ItemDefinition->BlueprintClass.Get();
    SpawnDeco(DecoTool, DecoClass, Location, Rotation, AttachedActor, InBuildingAttachmentType, 0);

    auto WorldInventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    UFortWorldItem* Item = nullptr;
    FFortItemEntry* ItemEntry = nullptr;

    for (auto& ItemInstance : WorldInventory->Inventory.ItemInstances)
    {
        if (ItemInstance->ItemEntry.ItemDefinition == DecoTool->ItemDefinition)
        {
            Item = ItemInstance;
            break;
        }
    }

    for (auto& ReplicatedEntry : WorldInventory->Inventory.ReplicatedEntries)
    {
        if (ReplicatedEntry.ItemDefinition == DecoTool->ItemDefinition)
        {
            ItemEntry = &ReplicatedEntry;
            break;
        }
    }

    if (!ItemEntry)
        return;

    ItemEntry->Count--;
    if (ItemEntry->Count <= 0)
        Remove(WorldInventory, ItemEntry->ItemGuid);
    else
    {
        Item->ItemEntry.Count = ItemEntry->Count;
        UpdateEntry(WorldInventory, *ItemEntry);
        Item->ItemEntry.bIsDirty = true;
    }
}

static void ServerCreateBuildingAndSpawnDeco(UObject* Context, FFrame& Stack, void* Ret)
{
    FVector BuildingLocation;
    EFortCardinalDirection BuildingDirection;
    FVector Location;
    FRotator Rotation;
    EBuildingAttachmentType InBuildingAttachmentType;
    bool bSpawnDecoOnExtraPiece;
    FVector BuildingExtraPieceLocation;
    Stack.StepCompiledIn(&BuildingLocation);
    Stack.StepCompiledIn(&BuildingDirection);
    Stack.StepCompiledIn(&Location);
    Stack.StepCompiledIn(&Rotation);
    Stack.StepCompiledIn(&InBuildingAttachmentType);
    Stack.StepCompiledIn(&bSpawnDecoOnExtraPiece);
    Stack.StepCompiledIn(&BuildingExtraPieceLocation);
    Stack.IncrementCode();

    FRotator BuildingRotation{};
    BuildingRotation.Yaw = (uint8)BuildingDirection == 0 ? -90.f : ((uint8)BuildingDirection == 2 ? 90.f : ((uint8)BuildingDirection == 3 ? 180.f : 0.f));

    auto Tool = (AFortDecoTool*)Context;

    auto Pawn = (AFortPlayerPawnAthena*)Tool->Owner;
    if (!Pawn)
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
    if (!PlayerController)
        return;

    auto ItemDefinition = (UFortDecoItemDefinition*)Tool->ItemDefinition;

    if (Tool->IsA(AFortDecoTool_ContextTrap::StaticClass()))
    {
        auto ContextTrapItemDefinition = ((AFortDecoTool_ContextTrap*)Tool)->ContextTrapItemDefinition;

        switch ((int)InBuildingAttachmentType)
        {
        case 0:
        case 6:
            ItemDefinition = ContextTrapItemDefinition->FloorTrap;
            break;
        case 7:
        case 2:
            ItemDefinition = ContextTrapItemDefinition->CeilingTrap;
            break;
        case 1:
            ItemDefinition = ContextTrapItemDefinition->WallTrap;
            break;
        case 8:
            ItemDefinition = ContextTrapItemDefinition->StairTrap;
            break;
        }
    }

    if (!ItemDefinition)
        return;

    auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
    UClass* BuildingClass = nullptr;
    EFortResourceType ResourceType = PlayerController->CurrentResourceType;

    if (ItemDefinition->AutoCreateAttachmentBuildingResourceType != EFortResourceType::None)
        ResourceType = ItemDefinition->AutoCreateAttachmentBuildingResourceType;

    auto BuildingType = (uint8)InBuildingAttachmentType <= 7 && ((0xC5 >> (uint8)InBuildingAttachmentType) & 1) ? 1 : ((uint8)InBuildingAttachmentType == 1 ? 0 : 12);

    static auto GetSparseClassData = (FBuildingSMActorClassData* (*)(UClass*, uint8))(ImageBase + 0x21E0B6C);

    for (auto& ShapePtr : ItemDefinition->AutoCreateAttachmentBuildingShapes)
    {
        auto Shape = ShapePtr.Get();

        for (auto& Class : GameState->AllPlayerBuildableClasses)
        {
            auto Default = (ABuildingSMActor*)((UClass*)Class)->ClassDefaultObject;
            auto EditModePatternData = GetSparseClassData(Default->Class, 1)->EditModePatternData;

            if (Default->ResourceType == ResourceType && (uint8)Default->BuildingType == BuildingType && EditModePatternData == Shape)
            {
                BuildingClass = Class;
                break;
            }
        }

        if (BuildingClass)
            break;
    }

    if (!BuildingClass)
        return;

    FBuildingClassData BuildingClassData{};
    BuildingClassData.BuildingClass = BuildingClass;
    BuildingClassData.PreviousBuildingLevel = -1;
    BuildingClassData.UpgradeLevel = 0;

    static auto CanAffordToPlaceBuildableClass = (bool (*)(AFortPlayerControllerAthena*, FBuildingClassData))(ImageBase + 0x9FF8A14);

    if (!CanAffordToPlaceBuildableClass(PlayerController, BuildingClassData))
        return;

    static auto CantBuild = (__int64 (*)(UWorld*, TSubclassOf<ABuildingActor>&, FVector, FRotator, bool, TArray<ABuildingSMActor*>*, char*))(ImageBase + 0x987C5E0);

    TArray<ABuildingSMActor*> RemoveBuildings;
    char bCantBuildReason;

    if (CantBuild(UWorld::GetWorld(), BuildingClassData.BuildingClass, BuildingLocation, BuildingRotation, false, &RemoveBuildings, &bCantBuildReason))
        return;

    for (auto& RemoveBuilding : RemoveBuildings)
        RemoveBuilding->K2_DestroyActor();
    RemoveBuildings.Free();

    auto Building = SpawnActorUnfinished<ABuildingSMActor>(BuildingClass, BuildingLocation, BuildingRotation, PlayerController);

    if (!Building)
        return;

    Building->InitializeKismetSpawnedBuildingActor(Building, PlayerController, true, nullptr, false);
    FinishSpawnActor(Building, BuildingLocation, BuildingRotation);

    Building->bPlayerPlaced = true;
    Building->Team = (EFortTeam)((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex;
    Building->TeamIndex = (uint8)Building->Team;

    if (!PlayerController->bBuildFree)
    {
        static auto PayBuildableClassPlacementCost = (int (*)(AFortPlayerControllerAthena*, FBuildingClassData))(ImageBase + 0xA0279B8);

        PayBuildableClassPlacementCost(PlayerController, BuildingClassData);
    }

    Tool->ServerSpawnDeco(Location, Rotation, Building, InBuildingAttachmentType);
}

UFortWeaponModSetData* GetRandomWeaponModSetData(const UObject* WorldContext, const UFortWeaponItemDefinition* WeaponItemDefinition)
{
    if (!WorldContext || !WeaponItemDefinition)
        return nullptr;

    auto WeaponModManager = (UFortWeaponModManager*)USubsystemBlueprintLibrary::GetWorldSubsystem((UObject*)WorldContext, UFortWeaponModManager::StaticClass());
    auto AutoApplyModData = WeaponModManager ? WeaponModManager->AutoApplyModDataAsset : nullptr;

    if (!AutoApplyModData || !AutoApplyModData->ModSetDataTable)
        return nullptr;

    auto Interface = WeaponItemDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass());

    if (!Interface)
        return nullptr;

    FGameplayTagContainer OwnedTags{};
    auto GetOwnedGameplayTags = (void (*)(void*, FGameplayTagContainer*))(*(void***)Interface)[0x2];
    GetOwnedGameplayTags(Interface, &OwnedTags);

    std::vector<FFortWeaponModSetRow*> BucketEntries;
    float TotalWeight = 0.f;

    for (auto& Pair : AutoApplyModData->ModSetDataTable->RowMap)
    {
        auto ModSetRow = (FFortWeaponModSetRow*)Pair.Value();

        if (!ModSetRow || !ModSetRow->Set || ModSetRow->RandomWeight <= 0.f)
            continue;

        for (auto& Tag : OwnedTags.GameplayTags)
        {
            if (Tag.TagName.ComparisonIndex == ModSetRow->BucketTag.TagName.ComparisonIndex)
            {
                BucketEntries.push_back(ModSetRow);
                TotalWeight += ModSetRow->RandomWeight;
                break;
            }
        }
    }

    OwnedTags.GameplayTags.Free();
    OwnedTags.ParentTags.Free();

    if (BucketEntries.empty())
        return nullptr;

    auto RandomNumber = ((float)rand() / 32767.f) * TotalWeight;

    for (auto& BucketEntry : BucketEntries)
    {
        if (RandomNumber <= BucketEntry->RandomWeight)
            return BucketEntry->Set;

        RandomNumber -= BucketEntry->RandomWeight;
    }

    return BucketEntries.back()->Set;
}

void ApplyModSetToPickup(const UFortWeaponModSetData* ModSet, AFortPickup* WeaponPickup)
{
    if (!ModSet || !WeaponPickup)
        return;

    auto& PickupEntry = WeaponPickup->MultiItemPickupEntries.Num() > 0 ? WeaponPickup->MultiItemPickupEntries[0] : WeaponPickup->PrimaryPickupItemEntry;
    auto WeaponDefinition = PickupEntry.ItemDefinition && PickupEntry.ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()) ? (UFortWeaponItemDefinition*)PickupEntry.ItemDefinition : nullptr;

    if (!WeaponDefinition)
        return;

    const TArray<FFortModSetModEntry>* Mods = nullptr;

    switch (WeaponDefinition->Rarity)
    {
    case EFortRarity::Common:
        Mods = &ModSet->CommonRarityMods;
        break;
    case EFortRarity::Uncommon:
        Mods = &ModSet->UncommonRarityMods;
        break;
    case EFortRarity::Rare:
        Mods = &ModSet->RareRarityMods;
        break;
    case EFortRarity::Epic:
        Mods = &ModSet->EpicRarityMods;
        break;
    case EFortRarity::Legendary:
        Mods = &ModSet->LegendaryRarityMods;
        break;
    }

    if (!Mods)
        return;

    bool bApplied = false;

    for (auto& ModEntry : *Mods)
    {
        if (ModEntry.ModDefinition && UFortWeaponModFunctionLibrary::ApplyWeaponModToPickup(WeaponPickup, ModEntry.ModDefinition))
            bApplied = true;
    }

    if (bApplied)
    {
        static auto ModSetStateType = FGameplayTag{ FName(L"ItemStateValue.ConstObject.ModSet") };
        SetStateValueConstObject(PickupEntry, ModSetStateType, ModSet);
    }
}

void AFortWeapon__PostLoad()
{
    ExecHook(AFortWeaponRanged::StaticClass()->FindFunction("ServerNotifyPawnHit"), ServerNotifyPawnHit);
    ExecHook(AFortWeaponRanged::StaticClass()->FindFunction("ServerSetScopedThirdPersonTargeting"), ServerSetScopedThirdPersonTargeting);
    ExecHook(AFortDecoTool::StaticClass()->FindFunction("ServerSpawnDeco"), ServerSpawnDeco);
    ExecHook(AFortDecoTool::StaticClass()->FindFunction("ServerCreateBuildingAndSpawnDeco"), ServerCreateBuildingAndSpawnDeco);
}
