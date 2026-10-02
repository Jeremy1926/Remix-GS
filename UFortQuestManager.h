#pragma once
#include "pch.h"

void SendStatEvent(UFortQuestManager* QuestManager, AActor* PlayerController, EFortQuestObjectiveStatEvent StatEvent, int32 Count, bool bAllowQueueStatEvent, UObject* TargetObject = nullptr,
                   FGameplayTagContainer TargetTags = FGameplayTagContainer(), FGameplayTagContainer AdditionalSourceTags = FGameplayTagContainer());
