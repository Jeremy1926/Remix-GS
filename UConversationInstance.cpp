#include "pch.h"
#include "Shared.h"
#include "UConversationInstance.h"
#include "AFortPickup.h"
#include "AFortInventory.h"
#include "ABuildingActor.h"
#include "AFortWeapon.h"

INIT_MODULE(UConversationInstance);

struct FServiceEntry
{
    int32 ServiceTag;
    int32 AdditionalRequirementTag;
    bool bShowAsLockedIfUnavailable;
};

struct FChosenSaleItem
{
    UFortItemDefinition* ItemDefinition;
    int32 StackSize;
    const FNPCSaleInventoryRow* Row;
};

static TArray<FClientConversationOptionEntry>& GetCurrentUserChoices(UConversationInstance* Conversation)
{
    return *(TArray<FClientConversationOptionEntry>*)(uint64(Conversation) + 0x40);
}

static FConversationBranchPoint& GetCurrentBranchPoint(UConversationInstance* Conversation)
{
    return *(FConversationBranchPoint*)(uint64(Conversation) + 0xE8);
}

static FConversationNodeHandle& GetCurrentNodeHandle(UConversationInstance* Conversation)
{
    return GetCurrentBranchPoint(Conversation).ClientChoice.ChoiceReference.NodeReference;
}

static EConversationRequirementResult CheckRequirements(UConversationTaskNode* Node, const FConversationContext* Context);
static void GenerateNextChoices(UConversationTaskNode* Node, TArray<FConversationBranchPoint>& BranchBuilder, const FConversationContext* Context);
static void ExecuteTaskNodeWithSideEffects(UConversationTaskNode* Node, const FConversationContext* Context, FConversationTaskResult* OutResult);
static UConversationParticipantComponent* FindFor(AActor* Actor);
static void ServerNotifyConversationStarted(UConversationParticipantComponent* Component, UConversationInstance* Conversation, const FGameplayTag& AsParticipant);
static void ServerNotifyConversationEnded(UConversationParticipantComponent* Component, UConversationInstance* Conversation, const FConversationParticipants& PreservedParticipants);
static void SendClientConversationMessage(UConversationParticipantComponent* Component, const FConversationContext* Context, const FClientConversationMessagePayload& Payload);
static void SendClientUpdatedChoices(UConversationParticipantComponent* Component, const FConversationContext* Context, bool bForcedRefresh);
static void SendClientRefreshedTaskChoiceData(UConversationParticipantComponent* Component, const FConversationNodeHandle& Handle, const FConversationContext* Context);

static UConversationRegistry* GetFromWorld(UObject* World)
{
    static UObject* CachedWorld = nullptr;
    static UConversationRegistry* CachedRegistry = nullptr;

    if (!World)
        return nullptr;

    if (CachedWorld == World && CachedRegistry)
        return CachedRegistry;

    CachedWorld = World;
    CachedRegistry = nullptr;

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (Object && !Object->IsDefaultObject() && Object->Outer == World && Object->IsA(UConversationRegistry::StaticClass()))
        {
            CachedRegistry = (UConversationRegistry*)Object;
            break;
        }
    }

    return CachedRegistry;
}

static UConversationNode* GetRuntimeNodeFromGUID(UConversationRegistry* Registry, const FGuid& NodeGUID, UConversationDatabase* Graph)
{
    static auto TryGetRuntimeNodeFromGUID = (UConversationNode* (*)(UConversationRegistry*, const FGuid*, UConversationDatabase*))(ImageBase + 0x8785FB8);

    if ((NodeGUID.A | NodeGUID.B | NodeGUID.C | NodeGUID.D) == 0)
        return nullptr;

    return TryGetRuntimeNodeFromGUID(Registry, &NodeGUID, Graph);
}

static void AppendOutputLinkGUIDs(UConversationRegistry* Registry, TArray<FGuid>& Result, const FGuid& SourceGUID, UConversationDatabase* Graph)
{
    auto SourceNode = GetRuntimeNodeFromGUID(Registry, SourceGUID, Graph);

    if (!SourceNode || !SourceNode->IsA(UConversationNodeWithLinks::StaticClass()))
        return;

    auto& OutputConnections = ((UConversationNodeWithLinks*)SourceNode)->OutputConnections;

    for (int32 i = 0; i < OutputConnections.Num(); i++)
        Result.Add(OutputConnections.Data[i]);
}

static void GetOutputLinkGUIDs(UConversationRegistry* Registry, TArray<FGuid>& Result, UConversationDatabase* Graph, const FGameplayTag& EntryPoint)
{
    static auto TryGetRuntimeNodeFromGUID = (UConversationNode* (*)(UConversationRegistry*, const FGuid*, UConversationDatabase*))(ImageBase + 0x8785FB8);

    if (!Graph)
    {
        FGuid NoGuid{};
        TryGetRuntimeNodeFromGUID(Registry, &NoGuid, nullptr);

        auto& EntryTagToEntryList = *(TMap<FGameplayTag, TArray<FGuid>>*)(uint64(Registry) + 0x150);

        for (auto& Entry : EntryTagToEntryList)
        {
            if (Entry.Key().TagName != EntryPoint.TagName)
                continue;

            for (int32 i = 0; i < Entry.Value().Num(); i++)
                AppendOutputLinkGUIDs(Registry, Result, Entry.Value().Data[i], nullptr);

            break;
        }

        return;
    }

    auto& EntryTags = Graph->EntryTags;

    for (int32 i = 0; i < EntryTags.Num(); i++)
    {
        auto& EntryList = EntryTags.Data[i];

        if (EntryList.EntryTag.TagName != EntryPoint.TagName)
            continue;

        for (int32 j = 0; j < EntryList.DestinationList.Num(); j++)
            AppendOutputLinkGUIDs(Registry, Result, EntryList.DestinationList.Data[j], Graph);

        break;
    }
}

static FGameplayTag& GetStartingEntryGameplayTag(UConversationInstance* Conversation)
{
    return *(FGameplayTag*)(uint64(Conversation) + 0x68);
}

static FConversationBranchPoint& GetStartingBranchPoint(UConversationInstance* Conversation)
{
    return *(FConversationBranchPoint*)(uint64(Conversation) + 0x70);
}

struct FCheckpoint
{
    FConversationBranchPoint ClientBranchPoint;
    TArray<FConversationChoiceReference> ScopeStack;
};

static TArray<FCheckpoint>& GetClientBranchPoints(UConversationInstance* Conversation)
{
    return *(TArray<FCheckpoint>*)(uint64(Conversation) + 0x160);
}

static TArray<FConversationBranchPoint>& GetCurrentBranchPoints(UConversationInstance* Conversation)
{
    return *(TArray<FConversationBranchPoint>*)(uint64(Conversation) + 0x170);
}

static TArray<FConversationChoiceReference>& GetScopeStack(UConversationInstance* Conversation)
{
    return *(TArray<FConversationChoiceReference>*)(uint64(Conversation) + 0x180);
}

static bool& GetbConversationStarted(UConversationInstance* Conversation)
{
    return *(bool*)(uint64(Conversation) + 0x198);
}

static void ModifyCurrentConversationNode(UConversationInstance* Conversation, const FConversationBranchPoint& NewBranchPoint);
static void ProcessCurrentConversationNode(UConversationInstance* Conversation);

static UObject* GetWorld(UConversationInstance* Conversation)
{
    for (auto Object = Conversation->Outer; Object; Object = Object->Outer)
        if (Object->IsA(UWorld::StaticClass()))
            return Object;

    return nullptr;
}

static std::vector<AActor*> GetParticipantActors(UConversationInstance* Conversation)
{
    std::vector<AActor*> Actors;

    for (int32 i = 0; i < Conversation->Participants.List.Num(); i++)
        Actors.push_back(Conversation->Participants.List.Data[i].Actor);

    return Actors;
}

static FConversationContext* CreateServerContext(UConversationInstance* Conversation, UConversationTaskNode* TaskBeingConsidered = nullptr)
{
    return FConversationContext::CreateServerContext(Conversation, TaskBeingConsidered, GetFromWorld(GetWorld(Conversation)));
}

static UConversationNode* ResolveNode(UConversationInstance* Conversation, const FConversationNodeHandle& Handle)
{
    auto Registry = GetFromWorld(GetWorld(Conversation));

    return Registry ? GetRuntimeNodeFromGUID(Registry, Handle.NodeGUID, Conversation->ActiveConversationGraph) : nullptr;
}

static void ServerRemoveParticipant(UConversationInstance* Conversation, const FGameplayTag& ParticipantID, const FConversationParticipants& PreservedParticipants)
{
    auto& List = Conversation->Participants.List;

    for (int32 i = 0; i < List.Num(); i++)
    {
        auto& Entry = List.Data[i];

        if (Entry.ParticipantID.TagName != ParticipantID.TagName)
            continue;

        auto OldParticipant = FindFor(Entry.Actor);

        if (OldParticipant && GetbConversationStarted(Conversation))
            ServerNotifyConversationEnded(OldParticipant, Conversation, PreservedParticipants);

        List.Remove(i);
        break;
    }
}

static void ServerAssignParticipant(UConversationInstance* Conversation, const FGameplayTag& ParticipantID, AActor* ParticipantActor)
{
    if (ParticipantID.TagName.ComparisonIndex == 0 || !ParticipantActor)
        return;

    ServerRemoveParticipant(Conversation, ParticipantID, Conversation->Participants);

    FConversationParticipantEntry NewEntry{};
    NewEntry.ParticipantID = ParticipantID;
    NewEntry.Actor = ParticipantActor;
    Conversation->Participants.List.Add(NewEntry);

    if (GetbConversationStarted(Conversation))
        if (auto ParticipantComponent = FindFor(ParticipantActor))
            ServerNotifyConversationStarted(ParticipantComponent, Conversation, ParticipantID);
}

static int32 ChooseBranchIndex(UConversationInstance* Conversation, int32 NumBranchIndices)
{
    if (NumBranchIndices <= 0)
        return 0;

    auto& Seed = *(uint32*)(uint64(Conversation) + 0x194);
    Seed = (Seed * 196314165U) + 907633515U;

    uint32 FractionBits = 0x3F800000U | (Seed >> 9);
    float Fraction = *(float*)&FractionBits - 1.0f;

    return int32(Fraction * float(NumBranchIndices));
}

static void PopClientBranchPoint(UConversationInstance* Conversation)
{
    auto& ClientBranchPoints = GetClientBranchPoints(Conversation);

    if (ClientBranchPoints.Num() <= 0)
        return;

    auto Index = ClientBranchPoints.Num() - 1;

    GetClientBranchPoints(Conversation).Data[Index].ClientBranchPoint.Destroy();
    FConversationChoiceReference::DestroyArray(GetClientBranchPoints(Conversation).Data[Index].ScopeStack);
    ClientBranchPoints.NumElements--;
}

static void ResetConversationProgress(UConversationInstance* Conversation)
{
    GetStartingEntryGameplayTag(Conversation).TagName.ComparisonIndex = 0;
    GetStartingBranchPoint(Conversation).Reset();
    GetCurrentBranchPoint(Conversation).Reset();

    auto& CurrentBranchPoints = GetCurrentBranchPoints(Conversation);

    for (int32 i = 0; i < CurrentBranchPoints.Num(); i++)
        CurrentBranchPoints.Data[i].Destroy();

    CurrentBranchPoints.NumElements = 0;

    while (GetClientBranchPoints(Conversation).Num() > 0)
        PopClientBranchPoint(Conversation);

    auto& CurrentUserChoices = GetCurrentUserChoices(Conversation);

    for (int32 i = 0; i < CurrentUserChoices.Num(); i++)
        CurrentUserChoices.Data[i].Destroy();

    CurrentUserChoices.NumElements = 0;
}

static void DetermineBranches(UConversationInstance* Conversation, TArray<FGuid>& EnabledPaths, const TArray<FGuid>& SourceList, EConversationRequirementResult MaximumRequirementResult)
{
    auto Context = CreateServerContext(Conversation);
    auto Registry = GetFromWorld(GetWorld(Conversation));

    for (int32 i = 0; i < SourceList.Num(); i++)
    {
        auto TestNode = Registry ? GetRuntimeNodeFromGUID(Registry, SourceList.Data[i], Conversation->ActiveConversationGraph) : nullptr;

        if (TestNode && TestNode->IsA(UConversationTaskNode::StaticClass()) && CheckRequirements((UConversationTaskNode*)TestNode, Context) <= MaximumRequirementResult)
            EnabledPaths.Add(SourceList.Data[i]);
    }

    Context->Free();
}

static void ServerAbortConversation(UConversationInstance* Conversation)
{
    if (GetbConversationStarted(Conversation))
    {
        ((void (*)(UConversationInstance*))Conversation->VTable[0x5A])(Conversation);

        auto ParticipantsCopy = FConversationParticipants::Allocate();
        ParticipantsCopy->CopyFrom(Conversation->Participants);

        for (int32 i = 0; i < ParticipantsCopy->List.Num(); i++)
            ServerRemoveParticipant(Conversation, ParticipantsCopy->List.Data[i].ParticipantID, *ParticipantsCopy);

        ParticipantsCopy->Free();
    }

    ResetConversationProgress(Conversation);
    GetbConversationStarted(Conversation) = false;
}

static void ServerStartConversation(UConversationInstance* Conversation, const FGameplayTag& EntryPoint, UConversationDatabase* Graph)
{
    ResetConversationProgress(Conversation);
    GetStartingEntryGameplayTag(Conversation) = EntryPoint;
    Conversation->ActiveConversationGraph = Graph;

    auto Registry = GetFromWorld(GetWorld(Conversation));
    TArray<FGuid> PotentialStartingPoints;

    if (Registry)
        GetOutputLinkGUIDs(Registry, PotentialStartingPoints, Graph, EntryPoint);

    if (PotentialStartingPoints.Num() == 0)
    {
        ServerAbortConversation(Conversation);
        return;
    }

    TArray<FGuid> LegalStartingPoints;
    DetermineBranches(Conversation, LegalStartingPoints, PotentialStartingPoints, EConversationRequirementResult::FailedButVisible);
    PotentialStartingPoints.Free();

    if (LegalStartingPoints.Num() == 0)
    {
        ServerAbortConversation(Conversation);
        return;
    }

    auto StartingGuid = LegalStartingPoints.Data[ChooseBranchIndex(Conversation, LegalStartingPoints.Num())];

    GetStartingBranchPoint(Conversation).Reset();
    GetStartingBranchPoint(Conversation).ClientChoice.ChoiceReference.NodeReference.NodeGUID = StartingGuid;
    GetCurrentBranchPoint(Conversation).Reset();
    GetCurrentBranchPoint(Conversation).ClientChoice.ChoiceReference.NodeReference.NodeGUID = StartingGuid;
    LegalStartingPoints.Free();

    auto& List = Conversation->Participants.List;

    for (int32 i = 0; i < List.Num(); i++)
        if (auto ParticipantComponent = FindFor(List.Data[i].Actor))
            ServerNotifyConversationStarted(ParticipantComponent, Conversation, List.Data[i].ParticipantID);

    GetbConversationStarted(Conversation) = true;

    LARGE_INTEGER Cycles;
    QueryPerformanceCounter(&Cycles);

    *(int32*)(uint64(Conversation) + 0x190) = int32(Cycles.LowPart);
    *(uint32*)(uint64(Conversation) + 0x194) = uint32(Cycles.LowPart);

    ((void (*)(UConversationInstance*))Conversation->VTable[0x59])(Conversation);

    ProcessCurrentConversationNode(Conversation);
}

static FConversationBranchPoint* FindBranchPointFromClientChoice(UConversationInstance* Conversation, const FConversationChoiceReference& InChoice)
{
    auto& CurrentBranchPoints = GetCurrentBranchPoints(Conversation);

    for (int32 i = 0; i < CurrentBranchPoints.Num(); i++)
    {
        auto& BranchPoint = CurrentBranchPoints.Data[i];

        if (BranchPoint.ClientChoice.ChoiceType != EConversationChoiceType::ServerOnly && BranchPoint.ClientChoice.ChoiceReference.IsIdenticalTo(InChoice))
            return &BranchPoint;
    }

    return nullptr;
}

static void ModifyCurrentConversationNode(UConversationInstance* Conversation, const FConversationChoiceReference& NewChoice)
{
    auto BranchPoint = FConversationBranchPoint::Allocate();
    BranchPoint->ClientChoice.ChoiceReference.CopyFrom(NewChoice);

    ModifyCurrentConversationNode(Conversation, *BranchPoint);
    BranchPoint->Free();
}

static void ServerAdvanceConversation(UConversationInstance* Conversation, const FAdvanceConversationRequest& InChoicePicked)
{
    if (!GetbConversationStarted(Conversation) || !GetCurrentBranchPoint(Conversation).ClientChoice.ChoiceReference.IsValid())
        return;

    TArray<FConversationBranchPoint> CandidateDestinations;
    auto ServerContext = CreateServerContext(Conversation);
    auto& Choice = InChoicePicked.Choice;

    if (Choice.NodeReference.IsValid() || Choice.NodeParameters.Num() > 0)
    {
        auto BranchPoint = FindBranchPointFromClientChoice(Conversation, Choice);

        if (!BranchPoint)
        {
            ServerContext->Free();

            auto Context = CreateServerContext(Conversation);

            for (auto Actor : GetParticipantActors(Conversation))
                if (auto ParticipantComponent = FindFor(Actor))
                    SendClientUpdatedChoices(ParticipantComponent, Context, true);

            Context->Free();
            return;
        }

        CandidateDestinations.Reserve(1);
        memset(CandidateDestinations.Data, 0, sizeof(FConversationBranchPoint));
        CandidateDestinations.Data[0].CopyFrom(*BranchPoint);
        CandidateDestinations.NumElements = 1;

        auto TaskNode = ResolveNode(Conversation, CandidateDestinations.Data[0].ClientChoice.ChoiceReference.NodeReference);

        if (TaskNode && TaskNode->IsA(UConversationTaskNode::StaticClass()))
        {
            auto& SubNodes = ((UConversationTaskNode*)TaskNode)->SubNodes;

            for (int32 i = 0; i < SubNodes.Num(); i++)
            {
                auto SubNode = SubNodes.Data[i];

                if (SubNode && SubNode->IsA(UConversationChoiceNode::StaticClass()))
                {
                    ((void (*)(UConversationSubNode*, const FConversationContext*, const FClientConversationOptionEntry*))SubNode->VTable[0x5E])(SubNode, ServerContext, &CandidateDestinations.Data[0].ClientChoice);
                    break;
                }
            }
        }
    }
    else
    {
        auto& ScopeStack = GetScopeStack(Conversation);

        if (GetCurrentBranchPoints(Conversation).Num() == 0 && ScopeStack.Num() > 0)
        {
            auto ReturnChoice = FConversationChoiceReference::Allocate();
            ReturnChoice->CopyFrom(ScopeStack.Data[ScopeStack.Num() - 1]);

            ServerContext->Free();
            ModifyCurrentConversationNode(Conversation, *ReturnChoice);
            ReturnChoice->Free();
            return;
        }

        auto& CurrentBranchPoints = GetCurrentBranchPoints(Conversation);

        for (int32 i = 0; i < CurrentBranchPoints.Num(); i++)
        {
            auto& BranchPoint = CurrentBranchPoints.Data[i];

            if (!BranchPoint.ClientChoice.IsChoiceAvailable())
                continue;

            CandidateDestinations.Reserve(1);
            memset(&CandidateDestinations.Data[CandidateDestinations.Num()], 0, sizeof(FConversationBranchPoint));
            CandidateDestinations.Data[CandidateDestinations.Num()].CopyFrom(BranchPoint);
            CandidateDestinations.NumElements++;
        }
    }

    TArray<FConversationBranchPoint> ValidDestinations;
    auto RequirementContext = CreateServerContext(Conversation);

    for (int32 i = 0; i < CandidateDestinations.Num(); i++)
    {
        auto& BranchPoint = CandidateDestinations.Data[i];
        auto TaskNode = ResolveNode(Conversation, BranchPoint.ClientChoice.ChoiceReference.NodeReference);

        if (TaskNode && TaskNode->IsA(UConversationTaskNode::StaticClass()))
        {
            auto Node = (UConversationTaskNode*)TaskNode;

            if (!Node->bIgnoreRequirementsWhileAdvancingConversations && CheckRequirements(Node, RequirementContext) != EConversationRequirementResult::Passed)
                continue;
        }

        ValidDestinations.Reserve(1);
        memset(&ValidDestinations.Data[ValidDestinations.Num()], 0, sizeof(FConversationBranchPoint));
        ValidDestinations.Data[ValidDestinations.Num()].CopyFrom(BranchPoint);
        ValidDestinations.NumElements++;
    }

    RequirementContext->Free();
    ServerContext->Free();
    FConversationBranchPoint::DestroyArray(CandidateDestinations);

    if (ValidDestinations.Num() == 0)
        ServerAbortConversation(Conversation);
    else
        ModifyCurrentConversationNode(Conversation, ValidDestinations.Data[ChooseBranchIndex(Conversation, ValidDestinations.Num())]);

    FConversationBranchPoint::DestroyArray(ValidDestinations);
}

static void PushClientBranchPoint(UConversationInstance* Conversation)
{
    auto& ClientBranchPoints = GetClientBranchPoints(Conversation);

    ClientBranchPoints.Reserve(1);

    auto Index = ClientBranchPoints.Num();
    memset(&ClientBranchPoints.Data[Index], 0, sizeof(FCheckpoint));
    ClientBranchPoints.NumElements++;

    GetClientBranchPoints(Conversation).Data[Index].ClientBranchPoint.CopyFrom(GetCurrentBranchPoint(Conversation));
    FConversationChoiceReference::CopyArray(GetClientBranchPoints(Conversation).Data[Index].ScopeStack, GetScopeStack(Conversation));
}

static void RestoreCheckpoint(UConversationInstance* Conversation, int32 Index)
{
    auto BranchPoint = FConversationBranchPoint::Allocate();
    BranchPoint->CopyFrom(GetClientBranchPoints(Conversation).Data[Index].ClientBranchPoint);

    auto& ScopeStack = GetScopeStack(Conversation);
    FConversationChoiceReference::DestroyArray(ScopeStack);
    FConversationChoiceReference::CopyArray(ScopeStack, GetClientBranchPoints(Conversation).Data[Index].ScopeStack);

    ModifyCurrentConversationNode(Conversation, *BranchPoint);
    BranchPoint->Free();
}

static void ModifyCurrentConversationNode(UConversationInstance* Conversation, const FConversationBranchPoint& NewBranchPoint)
{
    auto NewBranchPointCopy = FConversationBranchPoint::Allocate();
    NewBranchPointCopy->CopyFrom(NewBranchPoint);

    auto& CurrentBranchPoint = GetCurrentBranchPoint(Conversation);
    CurrentBranchPoint.Reset();
    CurrentBranchPoint.CopyFrom(*NewBranchPointCopy);

    auto& ScopeStack = GetScopeStack(Conversation);
    auto& ReturnScopeStack = NewBranchPointCopy->ReturnScopeStack;

    for (int32 i = 0; i < ReturnScopeStack.Num(); i++)
    {
        ScopeStack.Reserve(1);
        memset(&ScopeStack.Data[ScopeStack.Num()], 0, sizeof(FConversationChoiceReference));
        ScopeStack.Data[ScopeStack.Num()].NodeReference.NodeGUID = ReturnScopeStack.Data[i].NodeGUID;
        ScopeStack.NumElements++;
    }

    NewBranchPointCopy->Free();

    ProcessCurrentConversationNode(Conversation);
}

static void AddUserChoice(UConversationInstance* Conversation, const FClientConversationOptionEntry& Choice)
{
    auto& CurrentUserChoices = GetCurrentUserChoices(Conversation);

    CurrentUserChoices.Reserve(1);
    memset(&CurrentUserChoices.Data[CurrentUserChoices.Num()], 0, sizeof(FClientConversationOptionEntry));
    CurrentUserChoices.Data[CurrentUserChoices.Num()].CopyFrom(Choice);
    CurrentUserChoices.NumElements++;
}

static void AddDefaultUserChoice(UConversationInstance* Conversation)
{
    auto DefaultChoice = FClientConversationOptionEntry::Allocate();
    auto NameSpace = FString(L"ConversationInstance");
    auto Key = FString(L"ConversationInstance_DefaultText");
    auto SourceString = FString(L"Continue");
    FText OutText{};

    if (UKismetTextLibrary::FindTextInLocalizationTable(NameSpace, Key, &OutText, SourceString))
        DefaultChoice->ChoiceText = OutText;
    else
    {
        FText::Destroy(&OutText);
        DefaultChoice->ChoiceText = UKismetTextLibrary::Conv_StringToText(SourceString);
    }

    NameSpace.Free();
    Key.Free();
    SourceString.Free();

    DefaultChoice->ChoiceType = EConversationChoiceType::UserChoiceAvailable;
    AddUserChoice(Conversation, *DefaultChoice);
    DefaultChoice->Free();
}

static void UpdateNextChoices(UConversationInstance* Conversation, const FConversationContext* Context)
{
    TArray<FConversationBranchPoint> AllChoices;
    auto TaskNode = ResolveNode(Conversation, GetCurrentNodeHandle(Conversation));

    if (TaskNode && TaskNode->IsA(UConversationTaskNode::StaticClass()))
    {
        auto ChoiceContext = Context->CreateChildContext((UConversationTaskNode*)TaskNode);
        GenerateNextChoices((UConversationTaskNode*)TaskNode, AllChoices, ChoiceContext);
        ChoiceContext->Free();
    }

    auto& CurrentUserChoices = GetCurrentUserChoices(Conversation);
    auto& CurrentBranchPoints = GetCurrentBranchPoints(Conversation);

    for (int32 i = 0; i < CurrentUserChoices.Num(); i++)
        CurrentUserChoices.Data[i].Destroy();

    CurrentUserChoices.NumElements = 0;
    FConversationBranchPoint::DestroyArray(CurrentBranchPoints);

    CurrentBranchPoints = AllChoices;

    for (int32 i = 0; i < CurrentBranchPoints.Num(); i++)
        if (CurrentBranchPoints.Data[i].ClientChoice.ChoiceType != EConversationChoiceType::ServerOnly)
            AddUserChoice(Conversation, CurrentBranchPoints.Data[i].ClientChoice);

    if ((CurrentBranchPoints.Num() > 0 || GetScopeStack(Conversation).Num() > 0) && CurrentUserChoices.Num() == 0)
        AddDefaultUserChoice(Conversation);
}

static void ServerRefreshConversationChoices(UConversationInstance* Conversation)
{
    auto Context = CreateServerContext(Conversation);

    UpdateNextChoices(Conversation, Context);

    for (auto Actor : GetParticipantActors(Conversation))
        if (auto ParticipantComponent = FindFor(Actor))
            SendClientUpdatedChoices(ParticipantComponent, Context, false);

    Context->Free();
}

static void ServerRefreshTaskChoiceData(UConversationInstance* Conversation, const FConversationNodeHandle& Handle)
{
    auto Context = CreateServerContext(Conversation);

    UpdateNextChoices(Conversation, Context);

    for (auto Actor : GetParticipantActors(Conversation))
        if (auto ParticipantComponent = FindFor(Actor))
            SendClientRefreshedTaskChoiceData(ParticipantComponent, Handle, Context);

    Context->Free();
}

static void ProcessCurrentConversationNode(UConversationInstance* Conversation)
{
    auto AnonContext = CreateServerContext(Conversation);
    auto CurrentNode = ResolveNode(Conversation, GetCurrentNodeHandle(Conversation));

    if (!CurrentNode || !CurrentNode->IsA(UConversationTaskNode::StaticClass()))
    {
        AnonContext->Free();
        ServerAbortConversation(Conversation);
        return;
    }

    auto TaskNode = (UConversationTaskNode*)CurrentNode;
    auto Context = AnonContext->CreateChildContext(TaskNode);
    auto TaskResult = FConversationTaskResult::Allocate();

    AnonContext->Free();
    ExecuteTaskNodeWithSideEffects(TaskNode, Context, TaskResult);

    auto& ScopeStack = GetScopeStack(Conversation);

    if (ScopeStack.Num() > 0 && ScopeStack.Data[ScopeStack.Num() - 1].NodeReference.NodeGUID == TaskNode->Compiled_NodeGUID)
    {
        ScopeStack.Data[ScopeStack.Num() - 1].Destroy();
        ScopeStack.NumElements--;
    }

    UpdateNextChoices(Conversation, Context);

    switch (TaskResult->Type)
    {
    case EConversationTaskResultType::AbortConversation:
        ServerAbortConversation(Conversation);
        break;
    case EConversationTaskResultType::AdvanceConversation:
    {
        auto AnyRequest = FAdvanceConversationRequest::Allocate();
        ServerAdvanceConversation(Conversation, *AnyRequest);
        AnyRequest->Free();
        break;
    }
    case EConversationTaskResultType::AdvanceConversationWithChoice:
        ModifyCurrentConversationNode(Conversation, TaskResult->AdvanceToChoice.Choice);
        break;
    case EConversationTaskResultType::PauseConversationAndSendClientChoices:
    {
        auto LastMessage = FClientConversationMessagePayload::Allocate();

        LastMessage->Message.CopyFrom(TaskResult->Message);
        FClientConversationOptionEntry::CopyArray(LastMessage->Options, GetCurrentUserChoices(Conversation));
        LastMessage->CurrentNode.NodeGUID = GetCurrentNodeHandle(Conversation).NodeGUID;
        LastMessage->Participants.CopyFrom(Conversation->Participants);

        PushClientBranchPoint(Conversation);

        for (int32 i = 0; i < LastMessage->Participants.List.Num(); i++)
            if (auto ParticipantComponent = FindFor(LastMessage->Participants.List.Data[i].Actor))
                SendClientConversationMessage(ParticipantComponent, Context, *LastMessage);

        LastMessage->Free();
        break;
    }
    case EConversationTaskResultType::ReturnToLastClientChoice:
        if (GetClientBranchPoints(Conversation).Num() > 1)
        {
            PopClientBranchPoint(Conversation);
            RestoreCheckpoint(Conversation, GetClientBranchPoints(Conversation).Num() - 1);
        }
        break;
    case EConversationTaskResultType::ReturnToCurrentClientChoice:
        if (GetClientBranchPoints(Conversation).Num() > 0)
        {
            auto Index = GetClientBranchPoints(Conversation).Num() - 1;
            auto BranchPoint = FConversationBranchPoint::Allocate();
            BranchPoint->CopyFrom(GetClientBranchPoints(Conversation).Data[Index].ClientBranchPoint);

            TArray<FConversationChoiceReference> CheckpointScopeStack;
            FConversationChoiceReference::CopyArray(CheckpointScopeStack, GetClientBranchPoints(Conversation).Data[Index].ScopeStack);

            PopClientBranchPoint(Conversation);

            FConversationChoiceReference::DestroyArray(ScopeStack);
            ScopeStack = CheckpointScopeStack;

            ModifyCurrentConversationNode(Conversation, *BranchPoint);
            BranchPoint->Free();
        }
        break;
    case EConversationTaskResultType::ReturnToConversationStart:
    {
        auto EntryStartPointGameplayTagCache = GetStartingEntryGameplayTag(Conversation);
        auto StartingBranchPointCache = FConversationBranchPoint::Allocate();
        StartingBranchPointCache->CopyFrom(GetStartingBranchPoint(Conversation));

        ResetConversationProgress(Conversation);

        GetStartingEntryGameplayTag(Conversation) = EntryStartPointGameplayTagCache;
        GetStartingBranchPoint(Conversation).CopyFrom(*StartingBranchPointCache);

        ModifyCurrentConversationNode(Conversation, *StartingBranchPointCache);
        StartingBranchPointCache->Free();
        break;
    }
    default:
        break;
    }

    TaskResult->Free();
    Context->Free();
}

static UConversationInstance* StartConversation(const FGameplayTag& ConversationEntryTag, AActor* Instigator, const FGameplayTag& InstigatorTag, AActor* Target, const FGameplayTag& TargetTag, UClass* ConversationInstanceClass, UConversationDatabase* Graph = nullptr)
{
    if (!Instigator || !Target)
        return nullptr;

    UObject* World = nullptr;

    for (auto Outer = (UObject*)Instigator; Outer && !World; Outer = Outer->Outer)
        if (Outer->IsA(UWorld::StaticClass()))
            World = Outer;

    if (!World)
        return nullptr;

    auto InstanceClass = ConversationInstanceClass;

    if (!InstanceClass)
    {
        InstanceClass = UConversationSettings::GetDefaultObj()->ConversationInstanceClass.Get();

        if (!InstanceClass)
            InstanceClass = UConversationInstance::StaticClass();
    }

    auto ConversationInstance = (UConversationInstance*)UGameplayStatics::SpawnObject(InstanceClass, World);

    if (!ConversationInstance)
        return nullptr;

    if (TargetTag.TagName.ComparisonIndex != 0)
        ServerAssignParticipant(ConversationInstance, TargetTag, Target);

    if (InstigatorTag.TagName.ComparisonIndex != 0)
        ServerAssignParticipant(ConversationInstance, InstigatorTag, Instigator);

    ServerStartConversation(ConversationInstance, ConversationEntryTag, Graph);

    return ConversationInstance;
}

static void StartConversation(UObject* Context, FFrame& Stack, UConversationInstance** Ret)
{
    FGameplayTag ConversationEntryTag{};
    AActor* Instigator = nullptr;
    FGameplayTag InstigatorTag{};
    AActor* Target = nullptr;
    FGameplayTag TargetTag{};
    UClass* ConversationInstanceClass = nullptr;

    Stack.StepCompiledIn(&ConversationEntryTag);
    Stack.StepCompiledIn(&Instigator);
    Stack.StepCompiledIn(&InstigatorTag);
    Stack.StepCompiledIn(&Target);
    Stack.StepCompiledIn(&TargetTag);
    Stack.StepCompiledIn(&ConversationInstanceClass);
    Stack.IncrementCode();

    *Ret = StartConversation(ConversationEntryTag, Instigator, InstigatorTag, Target, TargetTag, ConversationInstanceClass);
}

static void StartConversationFromGraph(UObject* Context, FFrame& Stack, UConversationInstance** Ret)
{
    FGameplayTag ConversationEntryTag{};
    AActor* Instigator = nullptr;
    FGameplayTag InstigatorTag{};
    AActor* Target = nullptr;
    FGameplayTag TargetTag{};
    UConversationDatabase* Graph = nullptr;

    Stack.StepCompiledIn(&ConversationEntryTag);
    Stack.StepCompiledIn(&Instigator);
    Stack.StepCompiledIn(&InstigatorTag);
    Stack.StepCompiledIn(&Target);
    Stack.StepCompiledIn(&TargetTag);
    Stack.StepCompiledIn(&Graph);
    Stack.IncrementCode();

    *Ret = Graph ? StartConversation(ConversationEntryTag, Instigator, InstigatorTag, Target, TargetTag, nullptr, Graph) : nullptr;
}

static UConversationParticipantComponent* FindFor(AActor* Actor)
{
    return Actor ? (UConversationParticipantComponent*)Actor->GetComponentByClass(UConversationParticipantComponent::StaticClass()) : nullptr;
}

static bool HasAuthority(UConversationParticipantComponent* Component)
{
    auto Owner = Component->GetOwner();

    return Owner && Owner->Role == ENetRole::ROLE_Authority;
}

static void UpdateConversationsActive(UConversationParticipantComponent* Component, int32 NewConversationsActive)
{
    Component->ConversationsActive = NewConversationsActive;
    UNetPushModelHelpers::MarkPropertyDirty(Component, FName(L"ConversationsActive"));
}

static void ServerNotifyConversationStarted(UConversationParticipantComponent* Component, UConversationInstance* Conversation, const FGameplayTag& AsParticipant)
{
    if (!HasAuthority(Component) || !Conversation)
        return;

    Component->Auth_CurrentConversation = Conversation;
    Component->Auth_Conversations.Add(Conversation);
    Component->ClientUpdateParticipants(Conversation->Participants);

    auto OldConversationsActive = Component->ConversationsActive;
    UpdateConversationsActive(Component, OldConversationsActive + 1);

    if (OldConversationsActive == 0)
        Component->OnRep_ConversationsActive(OldConversationsActive);

    Component->ClientStartConversation(Conversation->Participants);
    Component->ClientUpdateConversations(Component->ConversationsActive);
}

static void ServerNotifyConversationEnded(UConversationParticipantComponent* Component, UConversationInstance* Conversation, const FConversationParticipants& PreservedParticipants)
{
    if (!HasAuthority(Component) || !Conversation)
        return;

    auto& Conversations = Component->Auth_Conversations;

    for (int32 i = 0; i < Conversations.Num(); i++)
    {
        if (Conversations.Data[i] != Conversation)
            continue;

        if (Component->Auth_CurrentConversation == Conversation)
            Component->Auth_CurrentConversation = nullptr;

        Conversations.Remove(i);

        auto OldConversationsActive = Component->ConversationsActive;
        UpdateConversationsActive(Component, OldConversationsActive - 1);

        Component->ClientExitConversation(PreservedParticipants);
        Component->ClientUpdateConversations(Component->ConversationsActive);

        if (Component->ConversationsActive == 0)
            Component->OnRep_ConversationsActive(OldConversationsActive);

        break;
    }
}

static void ServerNotifyExecuteTaskAndSideEffects(UConversationParticipantComponent* Component, const FConversationNodeHandle& Handle, UConversationDatabase* Graph)
{
    if (HasAuthority(Component))
        Component->ClientExecuteTaskAndSideEffects(Handle, Graph);
}

static void ServerForAllConversationsRefreshChoices(UConversationParticipantComponent* Component, UConversationInstance* IgnoreConversation)
{
    if (!HasAuthority(Component))
        return;

    auto Conversations = Component->Auth_Conversations;

    for (int32 i = 0; i < Conversations.Num(); i++)
        if (Conversations.Data[i] != IgnoreConversation)
            ServerRefreshConversationChoices(Conversations.Data[i]);
}

static void ServerForAllConversationsRefreshTaskChoiceData(UConversationParticipantComponent* Component, const FConversationNodeHandle& Handle, UConversationInstance* IgnoreConversation)
{
    if (!HasAuthority(Component))
        return;

    auto Conversations = Component->Auth_Conversations;

    for (int32 i = 0; i < Conversations.Num(); i++)
        if (Conversations.Data[i] != IgnoreConversation)
            ServerRefreshTaskChoiceData(Conversations.Data[i], Handle);
}

static void ServerAbortAllConversations(UConversationParticipantComponent* Component)
{
    std::vector<UConversationInstance*> Conversations;

    for (int32 i = 0; i < Component->Auth_Conversations.Num(); i++)
        Conversations.push_back(Component->Auth_Conversations.Data[i]);

    for (auto Conversation : Conversations)
        ServerAbortConversation(Conversation);
}

static void SendClientConversationMessage(UConversationParticipantComponent* Component, const FConversationContext* Context, const FClientConversationMessagePayload& Payload)
{
    auto& LastMessage = Component->LastMessage;

    LastMessage.Destroy();
    memset(&LastMessage, 0, sizeof(FClientConversationMessagePayload));
    LastMessage.CopyFrom(Payload);
    Component->ClientUpdateConversation(LastMessage);
}

static void SendClientUpdatedChoices(UConversationParticipantComponent* Component, const FConversationContext* Context, bool bForcedRefresh)
{
    auto Conversation = Context->ActiveConversation;

    if (!Conversation)
        return;

    auto& NewOptions = GetCurrentUserChoices(Conversation);
    auto& LastMessage = Component->LastMessage;

    if (!bForcedRefresh && FClientConversationOptionEntry::AreArraysIdentical(NewOptions, LastMessage.Options))
        return;

    FClientConversationOptionEntry::DestroyArray(LastMessage.Options);
    FClientConversationOptionEntry::CopyArray(LastMessage.Options, NewOptions);
    Component->ClientUpdateConversation(LastMessage);
}

static void SendClientRefreshedTaskChoiceData(UConversationParticipantComponent* Component, const FConversationNodeHandle& Handle, const FConversationContext* Context)
{
    auto Conversation = Context->ActiveConversation;

    if (!Conversation)
        return;

    auto& CurrentOptions = GetCurrentUserChoices(Conversation);
    auto& LastOptions = Component->LastMessage.Options;

    for (int32 i = 0; i < CurrentOptions.Num(); i++)
    {
        auto& CurrentOption = CurrentOptions.Data[i];

        if (!CurrentOption.ChoiceReference.NodeReference.IsIdenticalTo(Handle))
            continue;

        for (int32 j = 0; j < LastOptions.Num(); j++)
        {
            auto& LastOption = LastOptions.Data[j];

            if (!LastOption.ChoiceReference.NodeReference.IsIdenticalTo(Handle))
                continue;

            if (FConversationNodeParameterPair::AreArraysIdentical(LastOption.ExtraData, CurrentOption.ExtraData) &&
                FConversationNodeParameterPair::AreArraysIdentical(LastOption.ChoiceReference.NodeParameters, CurrentOption.ChoiceReference.NodeParameters))
                continue;

            FConversationNodeParameterPair::DestroyArray(LastOption.ExtraData);
            FConversationNodeParameterPair::CopyArray(LastOption.ExtraData, CurrentOption.ExtraData);
            FConversationNodeParameterPair::DestroyArray(LastOption.ChoiceReference.NodeParameters);
            FConversationNodeParameterPair::CopyArray(LastOption.ChoiceReference.NodeParameters, CurrentOption.ChoiceReference.NodeParameters);
            Component->ClientUpdateConversationTaskChoiceData(Handle, LastOption);
            break;
        }
    }
}

static void ServerAdvanceConversation(UObject* Context, FFrame& Stack)
{
    auto InChoicePicked = FAdvanceConversationRequest::Allocate();
    Stack.StepCompiledIn(InChoicePicked);
    Stack.IncrementCode();

    auto Component = (UConversationParticipantComponent*)Context;

    if (HasAuthority(Component) && Component->Auth_CurrentConversation)
        ServerAdvanceConversation(Component->Auth_CurrentConversation, *InChoicePicked);

    InChoicePicked->Free();
}

struct FServiceState
{
    bool bInitialized = false;
    std::vector<FServiceEntry> Services;
    std::vector<const FNPCSaleInventoryRow*> Sales;
    std::unordered_map<uint64, FChosenSaleItem> ChosenItems;
};

static std::unordered_map<uint64, int32> NPCUniqueIDTags;
static std::unordered_map<uint64, FServiceState> ServiceStates;

static uint64 GetObjectKey(const UObject* Object)
{
    FWeakObjectPtr WeakObject(Object);

    return (uint64(uint32(WeakObject.ObjectIndex)) << 32) | uint32(WeakObject.ObjectSerialNumber);
}

void SetNPCUniqueIDTag(const UObject* Component, const FGameplayTag& Tag)
{
    if (Component && Tag.TagName.ComparisonIndex != 0)
        NPCUniqueIDTags[GetObjectKey(Component)] = Tag.TagName.ComparisonIndex;
}

static FGameplayTag GetServiceProviderTag(const UFortNonPlayerConversationParticipantComponent* Component)
{
    return Component->ServiceProviderIDTag.TagName.ComparisonIndex != 0 ? Component->ServiceProviderIDTag : Component->ConversationEntryTag;
}

static UFortNonPlayerConversationParticipantComponent* FindNPCParticipantComponent(const FConversationParticipants& Participants, const AActor* Ignore)
{
    for (int32 i = 0; i < Participants.List.Num(); i++)
    {
        auto Actor = Participants.List.Data[i].Actor;

        if (!Actor || Actor == Ignore)
            continue;

        if (auto Component = Actor->GetComponentByClass(UFortNonPlayerConversationParticipantComponent::StaticClass()))
            return (UFortNonPlayerConversationParticipantComponent*)Component;
    }

    return nullptr;
}

static UDataTable* ResolveTable(UDataTable* DefaultTable, const FGameplayTag& OverrideTag)
{
    if (OverrideTag.TagName.ComparisonIndex == 0)
        return DefaultTable;

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Season = (UAthenaSeasonItemDefinition*)UObject::GObjects->GetByIndex(i);

        if (!Season || Season->IsDefaultObject() || !Season->IsA(UAthenaSeasonItemDefinition::StaticClass()) || Season->SeasonNumber != 32)
            continue;

        for (auto& SeasonDataTable : Season->SeasonDataTables)
            if (SeasonDataTable.Key().TagName == OverrideTag.TagName)
                return SeasonDataTable.Value();

        break;
    }

    return DefaultTable;
}

static bool DoesTagMatch(const FGameplayTag& Tag, const FGameplayTag& TagToCheck, bool bExactMatch)
{
    if (Tag.TagName.ComparisonIndex == 0 || TagToCheck.TagName.ComparisonIndex == 0)
        return false;

    if (Tag.TagName == TagToCheck.TagName)
        return true;

    if (bExactMatch)
        return false;

    auto TagString = Tag.TagName.ToString();
    auto CheckString = TagToCheck.TagName.ToString();

    return TagString.size() > CheckString.size() && TagString.compare(0, CheckString.size(), CheckString) == 0 && TagString[CheckString.size()] == '.';
}

static FServiceState& GetServiceState(const UFortNonPlayerConversationParticipantComponent* Component)
{
    auto& State = ServiceStates[GetObjectKey(Component)];

    if (State.bInitialized)
        return State;

    State.bInitialized = true;

    auto Found = NPCUniqueIDTags.find(GetObjectKey(Component));
    auto NPCUniqueIDTag = GetServiceProviderTag(Component);

    if (Found != NPCUniqueIDTags.end())
        NPCUniqueIDTag.TagName.ComparisonIndex = Found->second;

    if (auto ServicesTable = ResolveTable(Component->Services, Component->ServicesOverride))
    {
        std::vector<std::pair<int32, FServiceEntry>> Candidates;

        for (auto& Row : ServicesTable->RowMap)
        {
            auto ServiceRow = (FNPCDynamicServiceRow*)Row.Value();

            if (!ServiceRow || ServiceRow->NPC.TagName != NPCUniqueIDTag.TagName)
                continue;

            auto Chance = ServiceRow->Chance;

            if (Chance <= 0.f || (Chance < 1.f && float(rand()) / float(RAND_MAX) >= Chance))
                continue;

            Candidates.push_back({ ServiceRow->Priority, { ServiceRow->ServiceTag.TagName.ComparisonIndex, ServiceRow->AdditionalRequirementTag.TagName.ComparisonIndex, ServiceRow->bShowAsLockedIfUnavailable } });
        }

        std::stable_sort(Candidates.begin(), Candidates.end(), [](auto& Left, auto& Right) { return Left.first > Right.first; });

        for (auto& [Priority, Entry] : Candidates)
        {
            if (Component->MaxServices > 0 && int32(State.Services.size()) >= Component->MaxServices)
                break;

            State.Services.push_back(Entry);
        }
    }

    if (auto SalesTable = ResolveTable(Component->SalesInventory, Component->SalesInventoryOverride))
    {
        for (auto& Row : SalesTable->RowMap)
        {
            auto SaleRow = (const FNPCSaleInventoryRow*)Row.Value();

            if (SaleRow && SaleRow->NPC.TagName == NPCUniqueIDTag.TagName)
                State.Sales.push_back(SaleRow);
        }

        std::stable_sort(State.Sales.begin(), State.Sales.end(), [](auto Left, auto Right) { return Left->Priority > Right->Priority; });
    }

    return State;
}

static const FServiceEntry* FindSupportedService(const UFortNonPlayerConversationParticipantComponent* Component, const FGameplayTag& ServiceTag, bool bExactMatch)
{
    for (auto& Service : GetServiceState(Component).Services)
    {
        FGameplayTag SupportedTag{};
        SupportedTag.TagName.ComparisonIndex = Service.ServiceTag;

        if (DoesTagMatch(SupportedTag, ServiceTag, bExactMatch))
            return &Service;
    }

    return nullptr;
}

static const FChosenSaleItem* GetSaleItem(const UFortNonPlayerConversationParticipantComponent* Component, int32 SellSlot, const FGameplayTag& FilterCategoryTag)
{
    auto& State = GetServiceState(Component);
    std::vector<const FNPCSaleInventoryRow*> FilteredSales;

    for (auto Sale : State.Sales)
        if (FilterCategoryTag.TagName.ComparisonIndex == 0 || Sale->OptionalFilterCategoryTag.TagName == FilterCategoryTag.TagName)
            FilteredSales.push_back(Sale);

    if (SellSlot < 1 || SellSlot > int32(FilteredSales.size()))
        return nullptr;

    auto Key = (uint64(uint32(FilterCategoryTag.TagName.ComparisonIndex)) << 32) | uint32(SellSlot);
    auto Found = State.ChosenItems.find(Key);

    if (Found != State.ChosenItems.end())
        return Found->second.ItemDefinition ? &Found->second : nullptr;

    auto Row = FilteredSales[SellSlot - 1];
    FChosenSaleItem ChosenItem{ nullptr, 0, Row };
    TArray<FFortItemEntry*> LootDrops{};

    if (Row->LootLevel >= 0)
        ChooseLootForContainer(LootDrops, Row->LootTier, -1, Row->LootLevel);
    else
        ChooseLootForContainer(LootDrops, Row->LootTier);

    for (auto& LootDrop : LootDrops)
    {
        auto ItemDefinition = (UFortItemDefinition*)LootDrop->ItemDefinition;

        if (ItemDefinition && (!ChosenItem.ItemDefinition || (!IsPrimaryQuickbar(ChosenItem.ItemDefinition) && IsPrimaryQuickbar(ItemDefinition))))
        {
            ChosenItem.ItemDefinition = ItemDefinition;
            ChosenItem.StackSize = LootDrop->Count > 0 ? LootDrop->Count : 1;
        }

        free(LootDrop);
    }

    LootDrops.Free();

    auto& Stored = State.ChosenItems[Key] = ChosenItem;

    return Stored.ItemDefinition ? &Stored : nullptr;
}

static float GetDiscountMultiplier(const UFortNonPlayerConversationParticipantComponent* Component)
{
    if (!Component->DiscountTable)
        return 1.f;

    for (auto& Row : Component->DiscountTable->RowMap)
    {
        auto DiscountRow = (FNPCDiscountServicesRow*)Row.Value();

        if (DiscountRow && DiscountRow->NPCConversationTag.TagName == Component->ConversationEntryTag.TagName)
            return DiscountRow->DiscountMultiplier;
    }

    return 1.f;
}

bool CanStartConversationWith(const UFortNonPlayerConversationParticipantComponent* Component, AActor* PlayerController)
{
    return ((bool (*)(const UFortNonPlayerConversationParticipantComponent*, AActor*))Component->VTable[0xA7])(Component, PlayerController);
}

UConversationInstance* StartConversation(const UFortNonPlayerConversationParticipantComponent* Component, const FGameplayTag& InConversationEntryTag, AActor* Instigator, AActor* Target, const UClass* ConversationInstanceClass, const UConversationDatabase* Graph)
{
    return StartConversation(InConversationEntryTag, Instigator, Component->InteractorParticipantTag, Target, Component->SelfParticipantTag, (UClass*)ConversationInstanceClass, (UConversationDatabase*)Graph);
}

static void StartConversation(UObject* Context, FFrame& Stack)
{
    FGameplayTag InConversationEntryTag{};
    AActor* Instigator = nullptr;
    AActor* Target = nullptr;

    Stack.StepCompiledIn(&InConversationEntryTag);
    Stack.StepCompiledIn(&Instigator);
    Stack.StepCompiledIn(&Target);
    Stack.IncrementCode();

    StartConversation((UFortNonPlayerConversationParticipantComponent*)Context, InConversationEntryTag, Instigator, Target);
}

static void StartConversationFromGraph(UObject* Context, FFrame& Stack)
{
    FGameplayTag InConversationEntryTag{};
    AActor* Instigator = nullptr;
    AActor* Target = nullptr;
    UConversationDatabase* Conversation = nullptr;

    Stack.StepCompiledIn(&InConversationEntryTag);
    Stack.StepCompiledIn(&Instigator);
    Stack.StepCompiledIn(&Target);
    Stack.StepCompiledIn(&Conversation);
    Stack.IncrementCode();

    if (Conversation)
        StartConversation((UFortNonPlayerConversationParticipantComponent*)Context, InConversationEntryTag, Instigator, Target, nullptr, Conversation);
}

static void RequestServerAbortConversation(UObject* Context, FFrame& Stack)
{
    Stack.IncrementCode();

    auto Component = (UFortPlayerConversationComponent*)Context;

    if (HasAuthority(Component))
        ServerAbortAllConversations(Component);
}

static void RequestServerAbortConversationWithParticipant(UObject* Context, FFrame& Stack)
{
    UConversationParticipantComponent* Participant = nullptr;

    Stack.StepCompiledIn(&Participant);
    Stack.IncrementCode();

    auto Component = (UFortPlayerConversationComponent*)Context;
    auto ParticipantActor = Participant ? Participant->GetOwner() : nullptr;

    if (!HasAuthority(Component) || !ParticipantActor)
        return;

    std::vector<UConversationInstance*> ConversationsToAbort;

    for (int32 i = 0; i < Component->Auth_Conversations.Num(); i++)
    {
        auto Conversation = Component->Auth_Conversations.Data[i];

        for (auto Actor : GetParticipantActors(Conversation))
        {
            if (Actor == ParticipantActor)
            {
                ConversationsToAbort.push_back(Conversation);
                break;
            }
        }
    }

    for (auto Conversation : ConversationsToAbort)
        ServerAbortConversation(Conversation);
}

static void (*GatherStaticExtraDataBase)(UConversationTaskNode*, const FConversationContext*, TArray<FConversationNodeParameterPair>*);

static std::unordered_map<uint64, int32> Purchases;

static EConversationRequirementResult CheckRequirements(UConversationTaskNode* Node, const FConversationContext* Context)
{
    static auto CheckRequirements = (EConversationRequirementResult (*)(UConversationTaskNode*, const FConversationContext*))(ImageBase + 0x8782B38);

    return CheckRequirements(Node, Context);
}

static void GenerateNextChoices(UConversationTaskNode* Node, TArray<FConversationBranchPoint>& BranchBuilder, const FConversationContext* Context)
{
    ((void (*)(UConversationTaskNode*, TArray<FConversationBranchPoint>*, const FConversationContext*))Node->VTable[0x62])(Node, &BranchBuilder, Context);
}

static void GenerateChoicesForDestinations(TArray<FConversationBranchPoint>& BranchBuilder, const FConversationContext* Context, const TArray<FGuid>& CandidateDestinations)
{
    static auto GenerateChoicesForDestinations = (void (*)(TArray<FConversationBranchPoint>*, const FConversationContext*, const TArray<FGuid>*))(ImageBase + 0x8784508);

    GenerateChoicesForDestinations(&BranchBuilder, Context, &CandidateDestinations);
}

static void ExecuteTaskNodeWithSideEffects(UConversationTaskNode* Node, const FConversationContext* Context, FConversationTaskResult* OutResult)
{
    auto Conversation = Context->ActiveConversation;
    auto PreviousEvalWorldContextObj = Node->EvalWorldContextObj;

    Node->EvalWorldContextObj = Conversation ? GetWorld(Conversation) : nullptr;
    memset(OutResult, 0, sizeof(FConversationTaskResult));

    if (Context->bServer_PRIVATE)
    {
        *OutResult = Node->ExecuteTaskNode(*Context);

        if (OutResult->CanConversationContinue())
        {
            for (int32 i = 0; i < Node->SubNodes.Num(); i++)
            {
                auto SideEffectNode = (UConversationSideEffectNode*)Node->SubNodes.Data[i];

                if (!SideEffectNode || !SideEffectNode->IsA(UConversationSideEffectNode::StaticClass()))
                    continue;

                auto PreviousSideEffectWorldContextObj = SideEffectNode->EvalWorldContextObj;

                SideEffectNode->EvalWorldContextObj = Conversation ? GetWorld(Conversation) : nullptr;
                SideEffectNode->ServerCauseSideEffect(*Context);
                SideEffectNode->EvalWorldContextObj = PreviousSideEffectWorldContextObj;
            }

            if (Conversation)
                for (auto Actor : GetParticipantActors(Conversation))
                    if (auto Component = FindFor(Actor))
                        ServerNotifyExecuteTaskAndSideEffects(Component, GetCurrentNodeHandle(Conversation), Conversation->ActiveConversationGraph);
        }
    }

    Node->EvalWorldContextObj = PreviousEvalWorldContextObj;
}

static void AddChoice(TArray<FConversationBranchPoint>& BranchBuilder, const FConversationContext* Context, const FClientConversationOptionEntry& Choice)
{
    BranchBuilder.Reserve(1);

    auto& BranchPoint = BranchBuilder.Data[BranchBuilder.Num()];

    memset(&BranchPoint, 0, sizeof(FConversationBranchPoint));
    FConversationNodeHandle::CopyArray(BranchPoint.ReturnScopeStack, Context->ReturnScopeStack);
    BranchPoint.ClientChoice.CopyFrom(Choice);
    BranchBuilder.NumElements++;
}

static void GatherChoices(UConversationLinkNode* Node, TArray<FConversationBranchPoint>* BranchBuilder, const FConversationContext* Context)
{
    auto Conversation = Context->ActiveConversation;
    auto Registry = Context->ConversationRegistry;

    if (!Conversation || !Registry)
        return;

    TArray<FGuid> PotentialStartingPoints;
    GetOutputLinkGUIDs(Registry, PotentialStartingPoints, nullptr, Node->RemoteEntryTag);

    if (PotentialStartingPoints.Num() > 0)
    {
        TArray<FGuid> LegalStartingPoints;
        DetermineBranches(Conversation, LegalStartingPoints, PotentialStartingPoints, EConversationRequirementResult::FailedButVisible);

        auto ReturnScopeContext = Context->CreateReturnScopeContext(Node->Compiled_NodeGUID);
        GenerateChoicesForDestinations(*BranchBuilder, ReturnScopeContext, LegalStartingPoints);
        ReturnScopeContext->Free();
        LegalStartingPoints.Free();
    }

    PotentialStartingPoints.Free();
}

static AActor* GetParticipantActor(const FConversationContext* Context, const FGameplayTag& ParticipantID)
{
    auto Conversation = Context ? Context->ActiveConversation : nullptr;

    if (!Conversation)
        return nullptr;

    for (auto& Entry : Conversation->Participants.List)
        if (Entry.ParticipantID.TagName == ParticipantID.TagName)
            return Entry.Actor;

    return nullptr;
}

static void ResolveParticipant(AActor* Participant, AActor*& OutActor, AController*& OutController)
{
    OutActor = Participant;
    OutController = nullptr;

    if (!Participant)
        return;

    if (Participant->IsA(AController::StaticClass()))
    {
        OutController = (AController*)Participant;
        OutActor = OutController->Pawn;
    }
    else if (Participant->IsA(APawn::StaticClass()))
        OutController = ((APawn*)Participant)->Controller;
}

static bool IsControllerRequirementMet(UFortControllerRequirement* Requirement, const FConversationContext* Context, const FGameplayTag& ParticipantID)
{
    auto Conversation = Context ? Context->ActiveConversation : nullptr;

    if (!Requirement || !Conversation)
        return false;

    AActor* Subject = nullptr;
    AActor* Other = nullptr;

    for (auto& Entry : Conversation->Participants.List)
    {
        if (Entry.ParticipantID.TagName == ParticipantID.TagName)
            Subject = Entry.Actor;
        else if (!Other)
            Other = Entry.Actor;
    }

    FControllerRequirementTestContext RequestContext{};

    ResolveParticipant(Subject, RequestContext.TestSubjectActor, RequestContext.TestSubjectController);
    ResolveParticipant(Other, RequestContext.OtherActor, RequestContext.OtherController);

    return Requirement->IsRequirementMet(RequestContext);
}

static EConversationRequirementResult IsRequirementSatisfied(UFortConversationRequirement_ControllerRequirement* Node, const FConversationContext* Context)
{
    return IsControllerRequirementMet(Node->Requirement, Context, Node->ParticipantID) ? EConversationRequirementResult::Passed : EConversationRequirementResult::FailedAndHidden;
}

static EConversationRequirementResult IsRequirementSatisfied(UFortConversationRequirement_HasService* Node, const FConversationContext* Context)
{
    auto Conversation = Context ? Context->ActiveConversation : nullptr;
    auto NPCComponent = Conversation ? FindNPCParticipantComponent(Conversation->Participants, nullptr) : nullptr;

    if (!NPCComponent || !FindSupportedService(NPCComponent, Node->ServiceTag, Node->bRequireExactMatch))
        return EConversationRequirementResult::FailedAndHidden;

    return EConversationRequirementResult::Passed;
}

static FString ObjectPathToString(const UObject* Object)
{
    return Object ? UKismetSystemLibrary::GetPathName(Object) : FString();
}

static FString IntToString(int32 Value)
{
    return FString(std::to_wstring(Value).c_str());
}

static FString TagContainerToString(const FGameplayTagContainer& Tags)
{
    std::wstring Result = L"(GameplayTags=(";

    for (int32 i = 0; i < Tags.GameplayTags.Num(); i++)
    {
        if (i > 0)
            Result += L",";

        auto TagName = Tags.GameplayTags.Data[i].TagName.ToString();
        Result += L"(TagName=\"" + std::wstring(TagName.begin(), TagName.end()) + L"\")";
    }

    Result += L"))";

    return FString(Result.c_str());
}

static void AddExtraData(TArray<FConversationNodeParameterPair>& ExtraData, const wchar_t* Key, const FString& Value)
{
    ExtraData.Reserve(1);

    auto& Pair = ExtraData.Data[ExtraData.Num()];

    memset(&Pair, 0, sizeof(FConversationNodeParameterPair));
    Pair.Name = FString(Key);
    Pair.Value = FConversationNodeParameterPair::CopyString(Value);
    ExtraData.NumElements++;
}

static void AddExtraData(TArray<FConversationNodeParameterPair>& ExtraData, const wchar_t* Key, const wchar_t* Value)
{
    auto ValueString = FString(Value);

    AddExtraData(ExtraData, Key, ValueString);
    ValueString.Free();
}

static void (*FillChoiceOG)(UFortConversationChoiceWithPreviewNode*, const FConversationContext*, FClientConversationOptionEntry*);

static void FillChoice(UFortConversationChoiceWithPreviewNode* Node, const FConversationContext* Context, FClientConversationOptionEntry* ChoiceEntry)
{
    FillChoiceOG(Node, Context, ChoiceEntry);

    auto Parent = Node->ParentNode;

    if (!Parent || !Parent->IsA(UConversationTaskNode::StaticClass()) || !Context || !Context->ActiveConversation || !Context->ConversationRegistry)
        return;

    for (auto& Connection : ((UConversationNodeWithLinks*)Parent)->OutputConnections)
    {
        auto Child = (UFortConversationTaskNode*)GetRuntimeNodeFromGUID(Context->ConversationRegistry, Connection, Context->ActiveConversation->ActiveConversationGraph);

        if (!Child || !Child->IsA(UFortConversationTaskNode::StaticClass()))
            continue;

        if (!Node->OptionalChildChoiceTagFilter.TagName.IsNone() && !Child->NodeTags.HasTag(Node->OptionalChildChoiceTagFilter))
            continue;

        auto ChildContext = Context->CreateChildContext(Child);

        if (CheckRequirements(Child, ChildContext) == EConversationRequirementResult::FailedAndHidden)
        {
            ChildContext->Free();
            continue;
        }

        TArray<FConversationBranchPoint> ChildChoices;
        ((void (*)(UConversationTaskNode*, TArray<FConversationBranchPoint>*, const FConversationContext*))Child->VTable[0x63])(Child, &ChildChoices, ChildContext);

        auto bPreviewed = false;

        if (ChildChoices.Num() > 0)
        {
            auto& ChildOption = ChildChoices.Data[0].ClientChoice;

            if (((bool (*)(UFortConversationTaskNode*, const FConversationContext*, const FClientConversationOptionEntry*))Child->VTable[0x68])(Child, ChildContext, &ChildOption))
            {
                auto PreviewTags = TagContainerToString(Child->NodeTags);

                AddExtraData(ChoiceEntry->ExtraData, L"preview_tags", PreviewTags);
                ((void (*)(UFortConversationTaskNode*, const FConversationContext*, const FClientConversationOptionEntry*, TArray<FConversationNodeParameterPair>*))Child->VTable[0x69])(Child, ChildContext, &ChildOption, &ChoiceEntry->ExtraData);

                PreviewTags.Free();
                bPreviewed = true;
            }
        }

        FConversationBranchPoint::DestroyArray(ChildChoices);
        ChildContext->Free();

        if (bPreviewed)
            break;
    }
}

static void GatherStaticExtraData(UFortConversationTaskNode* Node, const FConversationContext* Context, TArray<FConversationNodeParameterPair>* InOutExtraData)
{
    GatherStaticExtraDataBase(Node, Context, InOutExtraData);

    if (Node->NodeType.Data && Node->NodeType.Num() > 1)
        AddExtraData(*InOutExtraData, L"Type", Node->NodeType);

    auto& BriefWidgetClassOverride = Node->BriefWidgetClassOverride.ObjectID.AssetPath;

    if (BriefWidgetClassOverride.PackageName.ComparisonIndex != 0)
    {
        auto PackageName = BriefWidgetClassOverride.PackageName.GetRawString();
        auto FullPath = std::wstring(PackageName.begin(), PackageName.end());

        if (BriefWidgetClassOverride.AssetName.ComparisonIndex != 0)
        {
            auto AssetName = BriefWidgetClassOverride.AssetName.GetRawString();
            FullPath += L"." + std::wstring(AssetName.begin(), AssetName.end());
        }

        auto& SubPathString = Node->BriefWidgetClassOverride.ObjectID.SubPathString;

        if (SubPathString.Num() > 1)
            FullPath += L":" + SubPathString.ToWString();

        auto BriefWidgetClassPath = FString(FullPath.c_str());

        if (BriefWidgetClassPath.Num() > 1)
            AddExtraData(*InOutExtraData, L"BriefWidgetClassPath", BriefWidgetClassPath);

        BriefWidgetClassPath.Free();
    }

    if (Node->IsA(UFortConversationTaskNode_Back::StaticClass()))
        AddExtraData(*InOutExtraData, L"IsBackTaskNode", L"1");
}

static int32 GetPrice(UFortConversationTaskNode_Service* Node, const FName& PricingKey, float Level)
{
    if (PricingKey.ComparisonIndex == 0)
        return -1;

    for (int32 i = 0; i < Node->PricingTables.Num(); i++)
    {
        auto CurveTable = Node->PricingTables.Data[i];

        if (!CurveTable)
            continue;

        bool bHasRow = false;

        for (auto& Row : GetRowMap(CurveTable))
        {
            if (Row.Key() == PricingKey)
            {
                bHasRow = true;
                break;
            }
        }

        if (!bHasRow)
            continue;

        FScalableFloat Price{};
        Price.Value = 1.f;
        Price.Curve.CurveTable = CurveTable;
        Price.Curve.RowName = PricingKey;

        return int32(Price.Evaluate(Level));
    }

    return -1;
}

static AFortPlayerControllerAthena* GetPlayerController(const FConversationContext* Context)
{
    auto Conversation = Context ? Context->ActiveConversation : nullptr;

    if (!Conversation)
        return nullptr;

    for (int32 i = 0; i < Conversation->Participants.List.Num(); i++)
    {
        auto Actor = Conversation->Participants.List.Data[i].Actor;

        if (Actor && Actor->IsA(APlayerController::StaticClass()))
            return (AFortPlayerControllerAthena*)Actor;
    }

    return nullptr;
}

static UFortNonPlayerConversationParticipantComponent* GetNPCComponent(const FConversationContext* Context)
{
    auto Conversation = Context ? Context->ActiveConversation : nullptr;

    return Conversation ? FindNPCParticipantComponent(Conversation->Participants, nullptr) : nullptr;
}

static int32 GetCurrencyCount(AFortPlayerControllerAthena* PlayerController, const UFortItemDefinition* Currency)
{
    auto Inventory = PlayerController ? (AFortInventory*)PlayerController->WorldInventory.ObjectPointer : nullptr;

    if (!Inventory || !Currency)
        return 0;

    int32 Count = 0;

    for (int32 i = 0; i < Inventory->Inventory.ReplicatedEntries.Num(); i++)
        if (Inventory->Inventory.ReplicatedEntries.Data[i].ItemDefinition == Currency)
            Count += Inventory->Inventory.ReplicatedEntries.Data[i].Count;

    return Count;
}

static uint64 GetPurchaseKey(UFortConversationTaskNode_SellItem* Node, const UObject* NPCComponent, const AActor* PlayerController)
{
    return GetObjectKey(NPCComponent) ^ (GetObjectKey(PlayerController) * 0x9E3779B97F4A7C15ull) ^ (uint64(uint32(Node->SellSlot)) << 56) ^ uint64(uint32(Node->OptionalFilterCategoryTag.TagName.ComparisonIndex));
}

static int32 GetItemCost(UFortConversationTaskNode_SellItem* Node, const UFortNonPlayerConversationParticipantComponent* NPCComponent, const UFortItemDefinition* ItemDefinition, bool& bOutDiscounted)
{
    bOutDiscounted = false;

    auto ItemPath = ObjectPathToString(ItemDefinition);
    auto Cost = ItemPath.Num() > 1 ? GetPrice(Node, FName(ItemPath.CStr()), float(ItemDefinition->Rarity) + 1.f) : -1;
    ItemPath.Free();

    if (Cost < 0)
        return -1;

    if (Node->bPriceDiscountsEnabled && NPCComponent)
    {
        auto Multiplier = GetDiscountMultiplier(NPCComponent);

        if (Multiplier < 1.f)
        {
            Cost = int32(float(Cost) * Multiplier);
            bOutDiscounted = true;
        }
    }

    return Cost;
}

static int32 GetRemainingStock(UFortConversationTaskNode_SellItem* Node, const UObject* NPCComponent, const AActor* PlayerController)
{
    auto Found = Purchases.find(GetPurchaseKey(Node, NPCComponent, PlayerController));
    auto Purchased = Found == Purchases.end() ? 0 : Found->second;

    return Purchased >= 1 ? 0 : 1 - Purchased;
}

static EConversationRequirementResult IsRequirementSatisfied(UFortConversationTaskNode_SellItem* Node, const FConversationContext* Context)
{
    auto NPCComponent = GetNPCComponent(Context);

    return NPCComponent && GetSaleItem(NPCComponent, Node->SellSlot, Node->OptionalFilterCategoryTag) ? EConversationRequirementResult::Passed : EConversationRequirementResult::FailedAndHidden;
}

static void GatherDynamicChoices(UFortConversationTaskNode_SellItem* Node, TArray<FConversationBranchPoint>* BranchBuilder, const FConversationContext* Context)
{
    auto NPCComponent = GetNPCComponent(Context);
    auto PlayerController = GetPlayerController(Context);
    auto SaleItem = NPCComponent ? GetSaleItem(NPCComponent, Node->SellSlot, Node->OptionalFilterCategoryTag) : nullptr;

    if (!SaleItem || !PlayerController)
        return;

    auto Choice = FClientConversationOptionEntry::Allocate();
    bool bDiscounted = false;
    auto Cost = GetItemCost(Node, NPCComponent, SaleItem->ItemDefinition, bDiscounted);

    if (Cost < 0)
        Cost = 0;

    auto Remaining = GetRemainingStock(Node, NPCComponent, PlayerController);
    auto bCanAfford = GetCurrencyCount(PlayerController, Node->ResourceCurrency) >= Cost;

    if (Remaining > 0)
    {
        TArray<FFormatArgumentData> InArgs;
        FFormatArgumentData Argument{};

        Argument.ArgumentName = FString(L"0");
        Argument.ArgumentValueType = EFormatArgumentType::Text;
        Argument.ArgumentValue = SaleItem->ItemDefinition->ItemName;
        InArgs.Add(Argument);

        Choice->ChoiceText = UKismetTextLibrary::Format(Node->BuyTextFormat, InArgs);

        InArgs.Data[0].ArgumentName.Free();
        InArgs.Free();
    }
    else
        FText::CopyTo(&Choice->ChoiceText, &Node->SoldOutTextFormat);

    for (int32 i = 0; i < Node->NodeTags.GameplayTags.Num(); i++)
        Choice->ChoiceTags.GameplayTags.Add(Node->NodeTags.GameplayTags.Data[i]);

    for (int32 i = 0; i < Node->NodeTags.ParentTags.Num(); i++)
        Choice->ChoiceTags.ParentTags.Add(Node->NodeTags.ParentTags.Data[i]);

    Choice->ChoiceType = Remaining > 0 && bCanAfford ? EConversationChoiceType::UserChoiceAvailable : EConversationChoiceType::UserChoiceUnavailable;
    Choice->ChoiceReference.NodeReference.NodeGUID = Node->Compiled_NodeGUID;

    auto& ExtraData = Choice->ExtraData;
    GatherStaticExtraData(Node, Context, &ExtraData);

    auto ItemPath = ObjectPathToString(SaleItem->ItemDefinition);
    auto CurrencyPath = ObjectPathToString(Node->ResourceCurrency);
    auto StackSize = IntToString(SaleItem->StackSize);
    auto RemainingString = IntToString(Remaining);
    auto CostString = IntToString(Cost);

    AddExtraData(ExtraData, L"item", ItemPath);
    AddExtraData(ExtraData, L"stacksize", StackSize);
    AddExtraData(ExtraData, L"remaining", RemainingString);
    AddExtraData(ExtraData, L"cost", CostString);
    AddExtraData(ExtraData, L"currency", CurrencyPath);
    AddExtraData(ExtraData, L"discountprice", bDiscounted ? L"1" : L"0");
    AddExtraData(ExtraData, L"locked", L"0");
    AddExtraData(ExtraData, L"unlock_level", L"0");

    if (Remaining <= 0)
        AddExtraData(ExtraData, L"cannotpurchasereason", L"1");
    else if (!bCanAfford)
        AddExtraData(ExtraData, L"cannotpurchasereason", L"0");

    ItemPath.Free();
    CurrencyPath.Free();
    StackSize.Free();
    RemainingString.Free();
    CostString.Free();

    AddChoice(*BranchBuilder, Context, *Choice);
    Choice->Free();
}

static FConversationTaskResult* ExecuteTaskNode(UFortConversationTaskNode_SellItem* Node, FConversationTaskResult* Result, const FConversationContext* Context)
{
    memset(Result, 0, sizeof(FConversationTaskResult));
    Result->Type = EConversationTaskResultType::ReturnToCurrentClientChoice;

    auto NPCComponent = GetNPCComponent(Context);
    auto PlayerController = GetPlayerController(Context);
    auto SaleItem = NPCComponent ? GetSaleItem(NPCComponent, Node->SellSlot, Node->OptionalFilterCategoryTag) : nullptr;

    if (!SaleItem || !PlayerController || !PlayerController->WorldInventory.ObjectPointer || GetRemainingStock(Node, NPCComponent, PlayerController) <= 0)
        return Result;

    bool bDiscounted = false;
    auto Cost = GetItemCost(Node, NPCComponent, SaleItem->ItemDefinition, bDiscounted);

    if (Cost < 0)
        Cost = 0;

    if (GetCurrencyCount(PlayerController, Node->ResourceCurrency) < Cost)
        return Result;

    ConsumeResource(PlayerController, Node->ResourceCurrency, Cost);

    auto Inventory = (AFortInventory*)PlayerController->WorldInventory.ObjectPointer;
    auto Pawn = PlayerController->MyFortPawn;
    auto WeaponDefinition = SaleItem->ItemDefinition->IsA(UFortWeaponItemDefinition::StaticClass()) ? (UFortWeaponItemDefinition*)SaleItem->ItemDefinition : nullptr;
    auto Stats = WeaponDefinition ? GetStats(WeaponDefinition) : nullptr;
    auto LoadedAmmo = Stats ? Stats->ClipSize : 0;
    static auto ApplyRandomWeaponModTag = FName(L"Conversation.Sell.ApplyRandomWeaponMod");
    bool bApplyRandomWeaponMod = false;

    if (WeaponDefinition && Pawn && SaleItem->Row)
        for (int32 i = 0; i < SaleItem->Row->LootTags.GameplayTags.Num() && !bApplyRandomWeaponMod; i++)
            bApplyRandomWeaponMod = SaleItem->Row->LootTags.GameplayTags.Data[i].TagName == ApplyRandomWeaponModTag;

    if (bApplyRandomWeaponMod)
    {
        if (auto Pickup = SpawnPickup(Pawn->K2_GetActorLocation(), SaleItem->ItemDefinition, SaleItem->StackSize, LoadedAmmo, EFortPickupSourceTypeFlag::Other, EFortPickupSpawnSource::Unset, Pawn))
            ApplyModSetToPickup(GetRandomWeaponModSetData(NPCComponent, WeaponDefinition), Pickup);
    }
    else
        GiveItem(Inventory, SaleItem->ItemDefinition, SaleItem->StackSize, LoadedAmmo);

    Purchases[GetPurchaseKey(Node, NPCComponent, PlayerController)]++;
    Result->Type = EConversationTaskResultType::AdvanceConversation;

    return Result;
}

static bool ResolveMessageForContext(UFortConversationTaskNode_Speech* Node, const FGameplayTag& SpeakerParticipantTag, const FConversationContext* Context, FContextualMessageQueryResult* OutResult)
{
    TMap<FGameplayTag, FContextualMessageConfig>* ConfigMap = nullptr;
    FContextualMessageConfig* DefaultConfig = nullptr;
    bool bUsePerNPC = false;
    bool bUseServiceProviderTag = false;

    if (Node->bUseInlineConfig)
    {
        ConfigMap = &Node->SpeakerEntryTagToConfig;
        DefaultConfig = &Node->GeneralConfig;
        bUsePerNPC = Node->bUsePerNPC_ContextualMessageLookup;
        bUseServiceProviderTag = Node->bLookupViaServiceProviderTag;
    }
    else if (auto Collection = Node->ExternalContextualMessageCollection)
    {
        ConfigMap = &Collection->SpeakerEntryTagToConfig;
        DefaultConfig = &Collection->GeneralConfig;
        bUsePerNPC = Collection->bUsePerNPC_ContextualMessageLookup;
        bUseServiceProviderTag = Collection->bLookupViaServiceProviderTag;
    }
    else
        return false;

    FContextualMessageConfig* Config = nullptr;

    if (bUsePerNPC)
    {
        auto Speaker = GetParticipantActor(Context, SpeakerParticipantTag);
        auto SpeakerComponent = Speaker ? (UFortNonPlayerConversationParticipantComponent*)Speaker->GetComponentByClass(UFortNonPlayerConversationParticipantComponent::StaticClass()) : nullptr;

        if (SpeakerComponent)
        {
            auto Key = bUseServiceProviderTag ? GetServiceProviderTag(SpeakerComponent) : SpeakerComponent->ConversationEntryTag;

            if (Key.TagName.ComparisonIndex != 0)
                for (auto& Entry : *ConfigMap)
                    if (Entry.Key().TagName == Key.TagName)
                        Config = &Entry.Value();
        }
    }

    if (!Config)
        Config = DefaultConfig;

    std::vector<FContextualMessageCandidate*> BestCandidates;
    float BestPriority = 0.f;

    for (int32 i = 0; i < Config->ContextualMessages.Num(); i++)
    {
        auto& Candidate = Config->ContextualMessages.Data[i];
        auto bRequireAny = Candidate.RequirementMatchPolicy == EContextRequirementMatchPolicy::RequireAny;
        auto bRequirementsMet = Candidate.ContextRequirements.Num() == 0 || !bRequireAny;

        for (int32 x = 0; x < Candidate.ContextRequirements.Num(); x++)
        {
            auto& Requirement = Candidate.ContextRequirements.Data[x];
            auto bIsMet = IsControllerRequirementMet(Requirement.Requirement, Context, Requirement.ParticipantID);

            if (bRequireAny && bIsMet)
            {
                bRequirementsMet = true;
                break;
            }

            if (!bRequireAny && !bIsMet)
            {
                bRequirementsMet = false;
                break;
            }
        }

        if (!bRequirementsMet)
            continue;

        auto CandidatePriority = Candidate.Priority.Evaluate();

        if (BestCandidates.empty() || CandidatePriority > BestPriority)
        {
            BestCandidates.clear();
            BestPriority = CandidatePriority;
        }
        else if (CandidatePriority < BestPriority)
            continue;

        BestCandidates.push_back(&Candidate);
    }

    FContextualMessageCandidate* Chosen = nullptr;

    if (!BestCandidates.empty())
    {
        float TotalWeight = 0.f;

        for (auto Candidate : BestCandidates)
            TotalWeight += max(Candidate->Weight.Evaluate(), 0.f);

        if (TotalWeight <= 0.f)
            Chosen = BestCandidates[rand() % BestCandidates.size()];
        else
        {
            auto Roll = (float(rand()) / float(RAND_MAX)) * TotalWeight;

            for (auto Candidate : BestCandidates)
            {
                auto CandidateWeight = Candidate->Weight.Evaluate();

                if (CandidateWeight <= 0.f)
                    continue;

                if (Roll <= CandidateWeight)
                {
                    Chosen = Candidate;
                    break;
                }

                Roll -= CandidateWeight;
            }

            if (!Chosen)
                Chosen = BestCandidates.back();
        }
    }

    auto& Message = Chosen ? Chosen->Message : Config->DefaultMessage;
    auto& MetadataParameters = Chosen ? Chosen->MetadataParameters : Config->DefaultMetadataParameters;
    auto& MetadataTags = Chosen ? Chosen->MetadataTags : Config->DefaultMetadataTags;

    FText::Destroy(&OutResult->Message);
    FConversationNodeParameterPair::DestroyArray(OutResult->MetadataParameters);
    OutResult->MetadataTags.GameplayTags.Free();
    OutResult->MetadataTags.ParentTags.Free();

    FText::CopyTo(&OutResult->Message, &Message);
    FConversationNodeParameterPair::CopyArray(OutResult->MetadataParameters, MetadataParameters);

    for (int32 i = 0; i < MetadataTags.GameplayTags.Num(); i++)
        OutResult->MetadataTags.GameplayTags.Add(MetadataTags.GameplayTags.Data[i]);

    for (int32 i = 0; i < MetadataTags.ParentTags.Num(); i++)
        OutResult->MetadataTags.ParentTags.Add(MetadataTags.ParentTags.Data[i]);

    return Chosen != nullptr;
}

static void ResolveMessageForContext(UObject* Context, FFrame& Stack, bool* Ret)
{
    FGameplayTag SpeakerParticipantTag{};
    Stack.StepCompiledIn(&SpeakerParticipantTag);

    auto ConversationContextTemp = FConversationContext::Allocate();
    auto ConversationContext = (FConversationContext*)Stack.StepCompiledInRefInternal(ConversationContextTemp);
    auto OutResultTemp = (FContextualMessageQueryResult*)FMemory::Malloc(sizeof(FContextualMessageQueryResult));
    memset(OutResultTemp, 0, sizeof(FContextualMessageQueryResult));

    auto OutResult = (FContextualMessageQueryResult*)Stack.StepCompiledInRefInternal(OutResultTemp);
    Stack.IncrementCode();

    *Ret = ResolveMessageForContext((UFortConversationTaskNode_Speech*)Context, SpeakerParticipantTag, ConversationContext, OutResult);

    FText::Destroy(&OutResultTemp->Message);
    FConversationNodeParameterPair::DestroyArray(OutResultTemp->MetadataParameters);
    OutResultTemp->MetadataTags.GameplayTags.Free();
    OutResultTemp->MetadataTags.ParentTags.Free();
    FMemory::Free(OutResultTemp);
    ConversationContextTemp->Free();
}

static void SetResult(FFrame& Stack, FConversationTaskResult* Ret, EConversationTaskResultType Type)
{
    auto ConversationContext = FConversationContext::Allocate();
    Stack.StepCompiledInRefInternal(ConversationContext);
    Stack.IncrementCode();

    Ret->Set(Type);
    ConversationContext->Free();
}

static void AdvanceConversation(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    SetResult(Stack, Ret, EConversationTaskResultType::AdvanceConversation);
}

static void ReturnToLastClientChoice(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    SetResult(Stack, Ret, EConversationTaskResultType::ReturnToLastClientChoice);
}

static void ReturnToCurrentClientChoice(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    SetResult(Stack, Ret, EConversationTaskResultType::ReturnToCurrentClientChoice);
}

static void ReturnToConversationStart(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    SetResult(Stack, Ret, EConversationTaskResultType::ReturnToConversationStart);
}

static void AbortConversation(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    SetResult(Stack, Ret, EConversationTaskResultType::AbortConversation);
}

static void AdvanceConversationWithChoice(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    auto ConversationContext = FConversationContext::Allocate();
    Stack.StepCompiledInRefInternal(ConversationContext);

    auto ChoiceTemp = FAdvanceConversationRequest::Allocate();
    auto Choice = (FAdvanceConversationRequest*)Stack.StepCompiledInRefInternal(ChoiceTemp);
    Stack.IncrementCode();

    Ret->Set(EConversationTaskResultType::AdvanceConversationWithChoice, Choice);
    ChoiceTemp->Free();
    ConversationContext->Free();
}

static void PauseConversationAndSendClientChoices(UObject* Context, FFrame& Stack, FConversationTaskResult* Ret)
{
    auto ConversationContext = FConversationContext::Allocate();
    Stack.StepCompiledInRefInternal(ConversationContext);

    auto MessageTemp = FClientConversationMessage::Allocate();
    auto Message = (FClientConversationMessage*)Stack.StepCompiledInRefInternal(MessageTemp);
    Stack.IncrementCode();

    Ret->Set(EConversationTaskResultType::PauseConversationAndSendClientChoices, nullptr, Message);
    MessageTemp->Free();
    ConversationContext->Free();
}

static void MakeConversationParticipant(UObject* Context, FFrame& Stack)
{
    auto ConversationContextTemp = FConversationContext::Allocate();
    auto ConversationContext = (FConversationContext*)Stack.StepCompiledInRefInternal(ConversationContextTemp);
    AActor* ParticipantActor = nullptr;
    FGameplayTag ParticipantTag{};

    Stack.StepCompiledIn(&ParticipantActor);
    Stack.StepCompiledIn(&ParticipantTag);
    Stack.IncrementCode();

    if (ParticipantActor && ParticipantTag.TagName.ComparisonIndex != 0 && ConversationContext->ActiveConversation)
        ServerAssignParticipant(ConversationContext->ActiveConversation, ParticipantTag, ParticipantActor);

    ConversationContextTemp->Free();
}

void UConversationInstance__Init()
{
    ExecHook(UConversationLibrary::StaticClass()->FindFunction("StartConversation"), (void (*)(UObject*, FFrame&, UConversationInstance**))StartConversation);
    ExecHook(UConversationLibrary::StaticClass()->FindFunction("StartConversationFromGraph"), (void (*)(UObject*, FFrame&, UConversationInstance**))StartConversationFromGraph);

    ExecHook(UConversationParticipantComponent::StaticClass()->FindFunction("ServerAdvanceConversation"), (void (*)(UObject*, FFrame&))ServerAdvanceConversation);
    ExecHook(UFortNonPlayerConversationParticipantComponent::StaticClass()->FindFunction("StartConversation"), (void (*)(UObject*, FFrame&))StartConversation);
    ExecHook(UFortNonPlayerConversationParticipantComponent::StaticClass()->FindFunction("StartConversationFromGraph"), (void (*)(UObject*, FFrame&))StartConversationFromGraph);
    ExecHook(UFortPlayerConversationComponent::StaticClass()->FindFunction("RequestServerAbortConversation"), RequestServerAbortConversation);
    ExecHook(UFortPlayerConversationComponent::StaticClass()->FindFunction("RequestServerAbortConversationWithParticipant"), RequestServerAbortConversationWithParticipant);

    VirtualSwap(UConversationLinkNode::GetDefaultObj()->VTable, 0x63, GatherChoices);

    GatherStaticExtraDataBase = decltype(GatherStaticExtraDataBase)(UConversationTaskNode::GetDefaultObj()->VTable[0x5D]);

    for (int32 i = 0; i < UObject::GObjects->Num(); i++)
    {
        auto Object = UObject::GObjects->GetByIndex(i);

        if (Object && Object->IsDefaultObject() && Object->IsA(UFortConversationTaskNode::StaticClass()) && Object->VTable[0x5D] == (void*)GatherStaticExtraDataBase)
            VirtualSwap(Object->VTable, 0x5D, GatherStaticExtraData);
    }

    Hook<UFortConversationTaskNode_SellItem>(uint32(0x60), (EConversationRequirementResult (*)(UFortConversationTaskNode_SellItem*, const FConversationContext*))IsRequirementSatisfied);
    Hook<UFortConversationTaskNode_SellItem>(uint32(0x65), GatherDynamicChoices);
    Hook<UFortConversationTaskNode_SellItem>(uint32(0x5F), ExecuteTaskNode);

    ExecHook(UFortConversationTaskNode_Speech::StaticClass()->FindFunction("ResolveMessageForContext"), (void (*)(UObject*, FFrame&, bool*))ResolveMessageForContext);

    auto Class = UConversationContextHelpers::StaticClass();

    ExecHook(Class->FindFunction("AdvanceConversation"), AdvanceConversation);
    ExecHook(Class->FindFunction("AdvanceConversationWithChoice"), AdvanceConversationWithChoice);
    ExecHook(Class->FindFunction("PauseConversationAndSendClientChoices"), PauseConversationAndSendClientChoices);
    ExecHook(Class->FindFunction("ReturnToLastClientChoice"), ReturnToLastClientChoice);
    ExecHook(Class->FindFunction("ReturnToCurrentClientChoice"), ReturnToCurrentClientChoice);
    ExecHook(Class->FindFunction("ReturnToConversationStart"), ReturnToConversationStart);
    ExecHook(Class->FindFunction("AbortConversation"), AbortConversation);
    ExecHook(Class->FindFunction("MakeConversationParticipant"), MakeConversationParticipant);

    Hook<UFortConversationRequirement_ControllerRequirement>(uint32(0x5C), (EConversationRequirementResult (*)(UFortConversationRequirement_ControllerRequirement*, const FConversationContext*))IsRequirementSatisfied);
    Hook<UFortConversationRequirement_HasService>(uint32(0x5C), (EConversationRequirementResult (*)(UFortConversationRequirement_HasService*, const FConversationContext*))IsRequirementSatisfied);
    Hook<UFortConversationChoiceWithPreviewNode>(uint32(0x5C), FillChoice, FillChoiceOG);
}
