#include "pch.h"
#include "Shared.h"

INIT_MODULE(UFortEngine);

static float GetMaxTickRate(UEngine* Engine, float DeltaTime, bool bAllowFrameRateSmoothing)
{
    return 30.f;
}

static uint32_t CheckCheckpointHeartBeat()
{
    return -1;
}

void UFortEngine__Init()
{
    Hook(ImageBase + 0x16E4324, GetMaxTickRate);
    Hook(ImageBase + 0x50D7900, CheckCheckpointHeartBeat);

    *(float*)(ImageBase + 0x12F168B0) = 0.0f;
}
