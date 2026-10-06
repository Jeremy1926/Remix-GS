#pragma once
#include "pch.h"

inline std::unordered_map<int32, UClass*> BuildingClassMap;

struct FNativeDelegate
{
    uint8* Instance;
    int32 Size;
};

void SetActorTimer(AActor* Actor, void* Callback, uint64& Handle, float Delay);
void PauseWarmup(EGameModePauseWarmupMaskId MaskId);
void UnPauseWarmup(EGameModePauseWarmupMaskId MaskId);
