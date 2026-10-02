#pragma once
#include "pch.h"

inline void (*VendWobble__FinishedFuncOG)(UObject*, FFrame&, void*);

void ConsumeResource(AFortPlayerControllerAthena* PlayerController, const UFortItemDefinition* ResourceDefinition, int32 Amount);
void VendWobble__FinishedFunc(UObject* Context, FFrame& Stack, void* Ret);
