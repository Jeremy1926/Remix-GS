#include "pch.h"
#include "Shared.h"
#include "AFortAthenaVehicle.h"
#include "AFortInventory.h"

INIT_MODULE(AFortAthenaVehicle);
INIT_POSTLOAD(AFortAthenaVehicle);

static void ServerMove(UObject* Context, FFrame& Stack, void* Ret)
{
    FReplicatedPhysicsPawnState InState{};
    Stack.StepCompiledIn(&InState);
    Stack.IncrementCode();

    auto Pawn = (AFortPhysicsPawn*)Context;
    auto Rotation = InState.Rotation;

    Rotation.X -= 0.3f;
    Rotation.Y /= -0.75f;
    Rotation.Z += 0.15f;
    Rotation.W /= 1.1f;

    auto RootComponent = Pawn->RootComponent && Pawn->RootComponent->IsA(UPrimitiveComponent::StaticClass()) ? (UPrimitiveComponent*)Pawn->RootComponent : nullptr;

    if (RootComponent)
    {
        FTransform Transform{};
        Transform.Rotation = Rotation;
        Transform.Translation = InState.Translation;
        Transform.Scale3D = FVector(1, 1, 1);

        RootComponent->K2_SetWorldTransform(Transform, false, nullptr, true);
        RootComponent->SetPhysicsLinearVelocity(InState.LinearVelocity, false, FName());
        RootComponent->SetPhysicsAngularVelocityInRadians(InState.AngularVelocity, false, FName());
    }
}

static void ServerUpdateTowhook(UObject* Context, FFrame& Stack, void* Ret)
{
    FVector_NetQuantizeNormal InNetTowhookAimDir;
    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();

    if (Context->IsA(AFortOctopusVehicle::StaticClass()))
    {
        auto Vehicle = (AFortOctopusVehicle*)Context;

        Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
        Vehicle->OnRep_NetTowhookAimDir();
    }
    else
    {
        auto Vehicle = (AFortSpaghettiVehicle*)Context;

        Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
        Vehicle->OnRep_NetTowhookAimDir();
    }
}

static void (*OnRep_ReplicatedAttachedInfoOG)(AFortOctopusTowhookAttachableProjectile*);
static void OnRep_ReplicatedAttachedInfo(AFortOctopusTowhookAttachableProjectile* Projectile)
{
    OnRep_ReplicatedAttachedInfoOG(Projectile);

    auto OwningVehicle = Projectile->OwningVehicle;

    if (!OwningVehicle)
        return;

    auto Component = (UPrimitiveComponent*)Projectile->AttachedInfo.Hit.Component.Get();

    static auto CanGrappleToComponent = (bool (*)(AFortOctopusVehicle*, UActorComponent*))(ImageBase + 0x8FFDAFC);

    if (!CanGrappleToComponent(OwningVehicle, Component))
        return;

    OwningVehicle->ReplicatedAttachState.Component = Component;
    (FVector&)OwningVehicle->ReplicatedAttachState.LocalLocation = UKismetMathLibrary::InverseTransformLocation(Component->K2_GetComponentToWorld(), Projectile->AttachedInfo.Hit.Location);
    (FVector&)OwningVehicle->ReplicatedAttachState.LocalNormal = UKismetMathLibrary::InverseTransformDirection(Component->K2_GetComponentToWorld(), Projectile->AttachedInfo.Hit.Normal);
    OwningVehicle->OnRep_ReplicatedAttachState();
}

static void ApplyVehicleMod(void* VehicleInterface, const FGameplayTag* ModTag, AActor* EventInstigator)
{
    static auto GetModConfig = (UFortVehicleModConfig* (*)(void*, const FGameplayTag*))(ImageBase + 0x99B1BE0);
    static auto ApplyToVehicleInternal = (void (*)(UFortVehicleModConfig*, void*))(ImageBase + 0xA3E0400);

    auto Vehicle = (AFortAthenaVehicle*)(__int64(VehicleInterface) - 0x520);

    if (!ModTag || Vehicle->Role != ENetRole::ROLE_Authority)
        return;

    auto CanApplyMod = (bool (*)(void*, const FGameplayTag*))(*(void***)VehicleInterface)[0x39];

    if (!CanApplyMod(VehicleInterface, ModTag))
        return;

    auto ModConfig = GetModConfig(VehicleInterface, ModTag);

    if (!ModConfig)
        return;

    static auto AppliedVehicleModTagsName = FName(L"AppliedVehicleModTags");

    UBlueprintGameplayTagLibrary::AddGameplayTag(Vehicle->AppliedVehicleModTags, *ModTag);
    UNetPushModelHelpers::MarkPropertyDirty(Vehicle, AppliedVehicleModTagsName);

    ApplyToVehicleInternal(ModConfig, VehicleInterface);
}

static void RemoveVehicleMod(void* VehicleInterface, const FGameplayTag* ModTag)
{
    static auto GetModConfig = (UFortVehicleModConfig* (*)(void*, const FGameplayTag*))(ImageBase + 0x99B1BE0);
    static auto RemoveFromVehicleInternal = (void (*)(UFortVehicleModConfig*, void*))(ImageBase + 0xA3F2168);

    auto Vehicle = (AFortAthenaVehicle*)(__int64(VehicleInterface) - 0x520);

    if (!ModTag || Vehicle->Role != ENetRole::ROLE_Authority)
        return;

    if (auto ModConfig = GetModConfig(VehicleInterface, ModTag))
        RemoveFromVehicleInternal(ModConfig, VehicleInterface);

    static auto AppliedVehicleModTagsName = FName(L"AppliedVehicleModTags");

    UBlueprintGameplayTagLibrary::RemoveGameplayTag(Vehicle->AppliedVehicleModTags, *ModTag);
    UNetPushModelHelpers::MarkPropertyDirty(Vehicle, AppliedVehicleModTagsName);
}

static void PatchVehicleInterfaces()
{
    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (!Object || !Object->IsDefaultObject() || !Object->IsA(AFortAthenaVehicle::StaticClass()))
            continue;

        auto VehicleInterface = (uint8*)Object->GetInterface(IFortVehicleInterface::StaticClass());

        if (VehicleInterface != (uint8*)Object + 0x520)
            continue;

        auto InterfaceVft = *(void***)VehicleInterface;

        if (*(uint8*)InterfaceVft[0x3B] == 0xC3 || *(uint8*)InterfaceVft[0x3B] == 0xC2)
            Patch<void*>(uint64(&InterfaceVft[0x3B]), (void*)RemoveVehicleMod);

        if (*(uint8*)InterfaceVft[0x3A] == 0xC3 || *(uint8*)InterfaceVft[0x3A] == 0xC2)
            Patch<void*>(uint64(&InterfaceVft[0x3A]), (void*)ApplyVehicleMod);
    }
}

static bool (*ShouldSelfSpawnVehicleOG)(AFortAthenaVehicleSpawner*);
static bool ShouldSelfSpawnVehicle(AFortAthenaVehicleSpawner* Spawner)
{
    if (ShouldSelfSpawnVehicleOG(Spawner))
        return true;

    auto World = UWorld::GetWorld();
    auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;

    if ((!GameState || !GameState->IsA(AFortGameStateAthena::StaticClass()) || !GameState->bPlaylistDataIsLoaded) &&
        std::find(MissedSelfSpawns.begin(), MissedSelfSpawns.end(), Spawner) == MissedSelfSpawns.end())
        MissedSelfSpawns.push_back(Spawner);

    return false;
}

void ReplayMissedSelfSpawns()
{
    if (MissedSelfSpawns.empty())
        return;

    TArray<AFortAthenaVehicleSpawner*> Spawners;
    GetAll<AFortAthenaVehicleSpawner>(Spawners);

    for (auto& Spawner : Spawners)
    {
        if (std::find(MissedSelfSpawns.begin(), MissedSelfSpawns.end(), Spawner) == MissedSelfSpawns.end())
            continue;

        if (ShouldSelfSpawnVehicleOG(Spawner))
            Spawner->SpawnVehicle();
    }

    MissedSelfSpawns.clear();
    Spawners.Free();
}

static void (*EquipVehicleWeaponOG)(UFortVehicleSeatWeaponComponent*, AFortPlayerPawn*, FWeaponSeatDefinition*, int32);
static void EquipVehicleWeapon(UFortVehicleSeatWeaponComponent* Component, AFortPlayerPawn* FortPawn, FWeaponSeatDefinition* WeaponSeatDefinition, int32 ItemLevel)
{
    if (FortPawn && WeaponSeatDefinition)
    {
        auto PlayerController = FortPawn->Controller && FortPawn->Controller->IsA(AFortPlayerControllerAthena::StaticClass()) ? (AFortPlayerControllerAthena*)FortPawn->Controller : nullptr;
        auto VehicleWeapon = WeaponSeatDefinition->VehicleWeaponOverride ? WeaponSeatDefinition->VehicleWeaponOverride : WeaponSeatDefinition->VehicleWeapon;
        auto WorldInventory = PlayerController ? (AFortInventory*)PlayerController->WorldInventory.ObjectPointer : nullptr;

        if (WorldInventory && PlayerController->MyFortPawn && VehicleWeapon)
        {
            auto Stats = GetStats(VehicleWeapon);
            auto VehicleItem = GiveItem(WorldInventory, VehicleWeapon, 1, Stats ? Stats->ClipSize : 0);
            auto Weapon = VehicleItem ? PlayerController->MyFortPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)VehicleItem->ItemEntry.ItemDefinition, VehicleItem->ItemEntry.ItemGuid, VehicleItem->ItemEntry.TrackerGuid, false) : nullptr;

            if (Weapon)
            {
                Weapon->ForceNetUpdate();

                *(TWeakObjectPtr<UFortWorldItem>*)(__int64(WeaponSeatDefinition) + 0x34) = TWeakObjectPtr<UFortWorldItem>(VehicleItem);
                *(TWeakObjectPtr<AFortWeapon>*)(__int64(WeaponSeatDefinition) + 0x3C) = TWeakObjectPtr<AFortWeapon>(Weapon);

                Component->CachedWeapon = TWeakObjectPtr<AFortWeaponRanged>((AFortWeaponRanged*)Weapon);
                Component->CachedWeaponDef = (UFortWeaponRangedItemDefinition*)Weapon->WeaponData;
                Component->bWeaponEquipped = true;

                auto Vehicle = Component->GetVehicle();
                FMountedWeaponInfoRepped RepWeaponInfo{};

                RepWeaponInfo.HostVehicleCached.ObjectPointer = Vehicle;
                RepWeaponInfo.HostVehicleCached.InterfacePointer = Vehicle->GetInterface(IFortVehicleInterface::StaticClass());
                RepWeaponInfo.HostVehicleSeatIndexCached = WeaponSeatDefinition->SeatIndex;

                ((AFortWeaponRangedForVehicle*)Weapon)->MountedWeaponInfoRepped = RepWeaponInfo;
                ((AFortWeaponRangedForVehicle*)Weapon)->OnRep_MountedWeaponInfoRepped();

                WeaponSeatDefinition->LastEquippedVehicleWeapon = VehicleWeapon;
            }
        }
    }

    return EquipVehicleWeaponOG(Component, FortPawn, WeaponSeatDefinition, ItemLevel);
}

void AFortAthenaVehicle__Init()
{
    ExecHook(AFortPhysicsPawn::StaticClass()->FindFunction("ServerMove"), ServerMove);
    ExecHook(AFortPhysicsPawn::StaticClass()->FindFunction("ServerMoveReliable"), ServerMove);
    ExecHook(AFortOctopusVehicle::StaticClass()->FindFunction("ServerUpdateTowhook"), ServerUpdateTowhook);
    ExecHook(AFortSpaghettiVehicle::StaticClass()->FindFunction("ServerUpdateTowhook"), ServerUpdateTowhook);
    Hook<AFortOctopusTowhookAttachableProjectile>(uint32(0x103), OnRep_ReplicatedAttachedInfo, OnRep_ReplicatedAttachedInfoOG);

    PatchVehicleInterfaces();

    Hook(ImageBase + 0x8F737D4, ShouldSelfSpawnVehicle, ShouldSelfSpawnVehicleOG);
}

void AFortAthenaVehicle__PostLoad()
{
    PatchVehicleInterfaces();

    Hook(ImageBase + 0xA3E7224, EquipVehicleWeapon, EquipVehicleWeaponOG);
}
