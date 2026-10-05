#include "pch.h"
#include "Shared.h"
#include "UAthenaAIServiceLoot.h"

INIT_MODULE(UAthenaAIServiceLoot);

static void (*BeginPlayOG)(ABuildingContainer*);

static UAthenaAIServiceLoot* GetStartedServiceLoot()
{
    auto World = UWorld::GetWorld();
    auto Service = World ? UAthenaAIBlueprintLibrary::GetAIServiceLoot(World) : nullptr;

    return Service && (*(uint8*)(uint64(Service) + 0x70) & 1) ? Service : nullptr;
}

void RegisterPickupLoot(AFortPickup* Pickup)
{
    static auto RegisterPickupLootInOctree = (void (*)(UAthenaAIServiceLoot*, AFortPickup*))(ImageBase + 0xA6B3744);

    if (auto Service = GetStartedServiceLoot())
        RegisterPickupLootInOctree(Service, Pickup);
}

static void BeginPlay(ABuildingContainer* Container)
{
    static auto RegisterChestLoot = (void (*)(UAthenaAIServiceLoot*, ABuildingContainer*, uint8))(ImageBase + 0xA6B3324);

    BeginPlayOG(Container);

    if (auto Service = GetStartedServiceLoot())
        RegisterChestLoot(Service, Container, ((bool (*)(ABuildingContainer*))Container->VTable[0x217])(Container));
}

void UAthenaAIServiceLoot__Init()
{
    Hook<ABuildingContainer>(uint32(0x74), BeginPlay, BeginPlayOG);
}
