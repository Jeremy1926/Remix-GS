#pragma once
#include "pch.h"

void SetNPCUniqueIDTag(const UObject* Component, const FGameplayTag& Tag);
bool CanStartConversationWith(const UFortNonPlayerConversationParticipantComponent* Component, AActor* PlayerController);
UConversationInstance* StartConversation(const UFortNonPlayerConversationParticipantComponent* Component, const FGameplayTag& InConversationEntryTag, AActor* Instigator, AActor* Target, const UClass* ConversationInstanceClass = nullptr, const UConversationDatabase* Graph = nullptr);
