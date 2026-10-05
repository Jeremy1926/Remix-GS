#pragma once
#include "pch.h"

inline std::unordered_map<int32, UClass*> BuildingClassMap;

void PauseWarmup(EGameModePauseWarmupMaskId MaskId);
void UnPauseWarmup(EGameModePauseWarmupMaskId MaskId);
