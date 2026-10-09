#include "pch.h"
#include "Shared.h"
#include "Engine/Source/Runtime/Engine/Public/TimerManager.h"

void ClearTimer(AActor* Actor, uint64& Handle)
{
    static auto GetWorldTimerManager = (void* (*)(AActor*))(ImageBase + 0x15C52AC);
    static auto FindTimer = (void* (*)(void*, uint64*))(ImageBase + 0x15C33A8);
    static auto InternalClearTimer = (void (*)(void*, uint64))(ImageBase + 0x15C4BA4);

    auto TimerManager = GetWorldTimerManager(Actor);

    if (FindTimer(TimerManager, &Handle))
        InternalClearTimer(TimerManager, Handle);

    Handle = 0;
}

void SetActorTimer(AActor* Actor, void* Callback, uint64& Handle, float Delay)
{
    static auto BindTimerDelegate = (void (*)(FNativeDelegate*, UObject*, void*))(ImageBase + 0x9816384);
    static auto GetWorldTimerManager = (void* (*)(AActor*))(ImageBase + 0x15C52AC);
    static auto MakeUnifiedTimerDelegate = (void* (*)(uint8*, FNativeDelegate*))(ImageBase + 0x15C4A14);
    static auto InternalSetTimer = (void (*)(void*, uint64*, uint8*, float, const void*))(ImageBase + 0x1E0C500);
    static auto DestroyDelegate = (void (*)(FNativeDelegate*))(ImageBase + 0x1C1D0C0);

    FNativeDelegate TimerDelegate{};
    uint8 UnifiedTimerDelegate[0x48];
    struct { uint8 bLoop; uint8 bMaxOncePerFrame; float FirstDelay; } TimerParameters{ 0, 0, -1.f };

    BindTimerDelegate(&TimerDelegate, Actor, Callback);
    MakeUnifiedTimerDelegate(UnifiedTimerDelegate, &TimerDelegate);
    InternalSetTimer(GetWorldTimerManager(Actor), &Handle, UnifiedTimerDelegate, Delay, &TimerParameters);
    DestroyDelegate((FNativeDelegate*)UnifiedTimerDelegate);
    DestroyDelegate(&TimerDelegate);
}
