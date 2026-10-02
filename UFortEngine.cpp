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

static void* (*SendRequestNowOG)(UMcpProfileGroup*, void*, int);
static void* SendRequestNow(UMcpProfileGroup* McpProfileGroup, void* Request, int Context)
{
    return SendRequestNowOG(McpProfileGroup, Request, 3);
}

void UFortEngine__Init()
{
    Hook(ImageBase + 0x16E4324, GetMaxTickRate);
    Hook(ImageBase + 0x50D7900, CheckCheckpointHeartBeat);
    Hook(ImageBase + 0x7C419C4, SendRequestNow, SendRequestNowOG);
    Hook<AFortTeamMemberPedestal>(uint32(0x74), AActor::GetDefaultObj()->VTable[0x74]);

    *(float*)(ImageBase + 0x12F168B0) = 0.0f;
}
