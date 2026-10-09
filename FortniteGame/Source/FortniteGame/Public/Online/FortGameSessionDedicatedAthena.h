#pragma once
#include "pch.h"

struct FOnlineSessionSettings
{
    uint8 Pad_0[0x8];
    int32 NumPublicConnections;
    int32 NumPrivateConnections;
    bool bShouldAdvertise;
    bool bAllowJoinInProgress;
    bool bIsLANMatch;
    bool bIsDedicated;
    bool bUsesStats;
    bool bAllowInvites;
    uint8 Pad_16[0xC2];
};

FSharedRef GetSessionInterface();
FOnlineSessionSettings* GetSessionSettings(const FSharedRef& SessionInterface, FName SessionName);
void RegisterServer(AGameSession* GameSession);
bool HasGameSession();
void OnDestroySessionComplete(AFortGameSessionDedicatedAthena* GameSession, FName SessionName, bool bWasSuccessful);
void CleanupDedicatedServerSession(AGameSession* GameSession, void* OnComplete);
