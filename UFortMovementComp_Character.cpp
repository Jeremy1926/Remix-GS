#include "pch.h"
#include "Shared.h"

INIT_POSTLOAD(UFortMovementComp_Character);

struct FCharacterNetworkMoveData
{
    enum class ENetworkMoveType : uint8
    {
        NewMove,
        PendingMove,
        OldMove
    };

    void* Vft;
    ENetworkMoveType NetworkMoveType;
    float TimeStamp;
};

static void (*ServerMove_PerformMovementOG)(UCharacterMovementComponent*, const FCharacterNetworkMoveData*);

static void ServerMove_PerformMovement(UCharacterMovementComponent* CharacterMovement, const FCharacterNetworkMoveData* MoveData)
{
    auto Movement = (UFortMovementComp_Character*)CharacterMovement;
    auto Pawn = Movement && MoveData && Movement->IsA(UFortMovementComp_Character::StaticClass()) ? Movement->PawnOwner : nullptr;

    if (!Pawn)
        return ServerMove_PerformMovementOG(CharacterMovement, MoveData);

    static auto GetMovementModeExtensionLogic = (UObject** (*)(AActor*, UObject**, uint32))(ImageBase + 0x2CD640C);
    static auto MakeMovementModeExtensionContext = (FMMERuntimeContext* (*)(FMMERuntimeContext*, UObject*))(ImageBase + 0x18E5684);
    static auto PushMovementModeExtension = (void (*)(UObject*, FMMERuntimeContext*))(ImageBase + 0x33FBC4C);
    static auto GetPredictionData_Server_Character = (uint8* (*)(UCharacterMovementComponent*))(ImageBase + 0x6C50E6C);

    auto& Received = Movement->ReceivedPackMovementModeExtensionRuntimeData;
    auto& Pushed = Movement->PushedMovementModeExtensionRuntimeData;
    auto& Pending = *(TArray<uint64>*)(uint64(&Movement->SavedMoveRuntimeDataBackup) + sizeof(TArray<uint64>) * 2);
    auto CurrentClientTimeStamp = *(float*)(GetPredictionData_Server_Character(CharacterMovement) + 0x90);
    auto bNewestMove = MoveData->NetworkMoveType != FCharacterNetworkMoveData::ENetworkMoveType::OldMove && (MoveData->TimeStamp > CurrentClientTimeStamp || CurrentClientTimeStamp - MoveData->TimeStamp > 100.f);

    for (auto Runtime : Pushed)
    {
        if (!Runtime)
            continue;

        UFortMovementMode_BaseExtRuntimeData* Source = nullptr;

        for (auto Candidate : Received)
        {
            if (Candidate && Candidate->LogicID.MovementMode_ExtCRC_ID == Runtime->LogicID.MovementMode_ExtCRC_ID && Candidate->RuntimeID == Runtime->RuntimeID)
                Source = Candidate;
        }

        if (Source)
        {
            Runtime->bForceManualActivationNetworked = Source->bForceManualActivationNetworked;
            Runtime->bForceManualEndNetworked = Source->bForceManualEndNetworked;
        }
        else if (bNewestMove)
            Runtime->bForceManualEnd = true;
    }

    for (auto PendingExtension : Pending)
    {
        auto LogicID = uint32(PendingExtension);
        auto RuntimeID = uint8(PendingExtension >> 32);
        int32 SourceIndex = -1;

        for (int i = 0; i < Received.Num(); i++)
        {
            if (Received[i] && Received[i]->LogicID.MovementMode_ExtCRC_ID == LogicID && Received[i]->RuntimeID == RuntimeID)
                SourceIndex = i;
        }

        UObject* Logic = nullptr;
        GetMovementModeExtensionLogic(Pawn, &Logic, LogicID);

        if (SourceIndex == -1 || !Logic || !((bool (*)(UObject*))Logic->VTable[0x7C])(Logic))
            continue;

        auto Runtime = Received[SourceIndex];
        Received[SourceIndex] = nullptr;

        Runtime->bForceManualActivation = false;
        Runtime->bForceManualEnd = false;
        Pushed.Add(Runtime);

        FMMERuntimeContext Context;
        MakeMovementModeExtensionContext(&Context, Runtime);
        PushMovementModeExtension(Logic, &Context);
    }

    Pending.Clear();

    return ServerMove_PerformMovementOG(CharacterMovement, MoveData);
}

void UFortMovementComp_Character__PostLoad()
{
    Hook(ImageBase + 0x6C5C5A0, ServerMove_PerformMovement, ServerMove_PerformMovementOG);
}
