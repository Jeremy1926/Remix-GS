#include "pch.h"
#include "Shared.h"
#include "SDK/SDK/FortniteAIServer_classes.hpp"

INIT_MODULE(UFortAthenaBTService_PickUpLoot);

static void (*OnBecomeRelevantOG)(UFortAthenaBTService_PickUpLoot*, UBehaviorTreeComponent*, uint8*);

static uint8 OnPickupInteractionExecutionStatusChanged(UFortAthenaBTService_PickUpLoot* Service, UBlackboardComponent* Blackboard, uint16, UBehaviorTreeComponent* OwnerComp)
{
    static auto GetExecutionStatus = (uint8 (*)(UBlackboardComponent*, uint16))(ImageBase + 0xA654FDC);
    static auto GetLootActor = (AActor* (*)(UFortAthenaBTService_PickUpLoot*, UBehaviorTreeComponent*))(ImageBase + 0xA766E84);
    static auto ConstructInteractionType = (FInteractionType* (*)(FInteractionType*, AFortPlayerPawn*, ETInteractionType))(ImageBase + 0x9876D48);
    static auto FlyToPlayer = (void (*)(AFortPickup*, AFortPlayerPawn*))(ImageBase + 0x9ABA508);

    auto Controller = OwnerComp ? (AFortAthenaAIBotController*)OwnerComp->AIOwner : nullptr;

    if (!Controller || !Controller->IsA(AFortAthenaAIBotController::StaticClass()))
        return 0;

    auto Pawn = Controller->PlayerBotPawn;

    if (GetExecutionStatus(Blackboard, *(uint16*)(uint64(Service) + 0x88)) != 2 || !Pawn)
        return 1;

    auto Loot = GetLootActor(Service, OwnerComp);

    if (!Loot)
        return 1;

    if (Loot->IsA(ABuildingActor::StaticClass()))
    {
        auto InteractInterface = Loot->GetInterface(IFortInteractInterface::StaticClass());
        FInteractionType InteractionType;

        if (!InteractInterface)
            return 1;

        ConstructInteractionType(&InteractionType, Pawn, ETInteractionType::IT_LongPress);
        ((bool (*)(void*, FInteractionType*))(*(void***)InteractInterface)[0x15])(InteractInterface, &InteractionType);
    }
    else if (Loot->IsA(AFortPickup::StaticClass()))
        FlyToPlayer((AFortPickup*)Loot, Pawn);

    return 1;
}

static void OnBecomeRelevant(UFortAthenaBTService_PickUpLoot* Service, UBehaviorTreeComponent* OwnerComp, uint8* NodeMemory)
{
    static auto CreateObserverDelegate = (void* (*)(void*, UFortAthenaBTService_PickUpLoot*, void*, UBehaviorTreeComponent**))(ImageBase + 0xA75DF4C);
    static auto RegisterObserver = (void* (*)(UBlackboardComponent*, uint64*, uint16, UObject*, void*))(ImageBase + 0x75A6B54);

    OnBecomeRelevantOG(Service, OwnerComp, NodeMemory);

    auto Blackboard = OwnerComp->BlackboardComp;

    if (!Blackboard)
        return;

    struct { void* Function; uint64 Adjustment; } ObserverMethod{ (void*)OnPickupInteractionExecutionStatusChanged, 0 };
    uint8 ObserverDelegate[0x10];
    uint64 ObserverHandle = 0;

    RegisterObserver(Blackboard, &ObserverHandle, *(uint16*)(uint64(Service) + 0x88), Service, CreateObserverDelegate(ObserverDelegate, Service, &ObserverMethod, &OwnerComp));
}

void UFortAthenaBTService_PickUpLoot__Init()
{
    Hook<UFortAthenaBTService_PickUpLoot>(uint32(0x63), OnBecomeRelevant, OnBecomeRelevantOG);
}
