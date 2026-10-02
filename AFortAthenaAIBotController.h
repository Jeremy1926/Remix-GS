#pragma once
#include "pch.h"

void CreateSubsystems(UAthenaAISystem* AISystem, UObject* GameMode);
bool BotFinishedTargetSpline(AActor* Pickup, AActor* Controller, bool (*Original)(void*));
