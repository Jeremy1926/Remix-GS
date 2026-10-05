#include "pch.h"
#include "Shared.h"
#include "UAthenaAIServiceLoot.h"

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

void RegisterChestLoot(ABuildingContainer* Container)
{
    static auto RegisterChestLootInOctree = (void (*)(UAthenaAIServiceLoot*, ABuildingContainer*, uint8))(ImageBase + 0xA6B3324);

    if (auto Service = GetStartedServiceLoot())
        RegisterChestLootInOctree(Service, Container, ((bool (*)(ABuildingContainer*))Container->VTable[0x217])(Container));
}
