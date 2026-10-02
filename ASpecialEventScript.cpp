#include "pch.h"
#include "Shared.h"
INIT_MODULE(ASpecialEventScript);

static void (*ActivatePhaseOG)(ASpecialEventScript*, int32, float);
static void ActivatePhase(ASpecialEventScript* SpecialEventScript, int32 IndexToActivate, float SequenceTimeOffset)
{
    auto DataLayerManager = UWorld::GetWorld()->GetDataLayerManager();

    if (SpecialEventScript->ReplicatedActivePhaseIndex >= 0)
    {
        auto& OldPhaseInfo = SpecialEventScript->PhaseInfoArray[SpecialEventScript->ReplicatedActivePhaseIndex];

        for (auto& DataLayerEntry : OldPhaseInfo.DataLayers)
            DataLayerManager->SetDataLayerRuntimeState(DataLayerEntry.DataLayerAsset, EDataLayerRuntimeState::Unloaded, DataLayerEntry.bIsRecursive);
    }

    auto& PhaseInfo = SpecialEventScript->PhaseInfoArray[IndexToActivate];

    for (auto& DataLayerEntry : PhaseInfo.DataLayers)
        DataLayerManager->SetDataLayerRuntimeState(DataLayerEntry.DataLayerAsset, EDataLayerRuntimeState::Activated, DataLayerEntry.bIsRecursive);

    SpecialEventScript->ReplicatedActivePhaseIndex = IndexToActivate;

    ActivatePhaseOG(SpecialEventScript, IndexToActivate, SequenceTimeOffset);
}

void ASpecialEventScript__Init()
{
    Hook(ImageBase + 0xC16D1EC, ActivatePhase, ActivatePhaseOG);
}
