#include "pch.h"
#include "Shared.h"
#include "UFortQuestManager.h"

INIT_POSTLOAD(UFortQuestManager);

struct FParseConditionResult
{
    bool bMatch;
    size_t NextStart;
};

static std::unordered_map<UFortQuestManager*, std::unordered_set<UFortQuestItemDefinition*>> OncePerMatchMap;
static std::unordered_map<AActor*, std::unordered_map<int32, int32>> CountThresholdMap;
static std::unordered_set<UFortAccoladeItemDefinition*> OnlyOnceMap;

static FParseConditionResult ParseCondition(std::string Condition, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags)
{
    size_t CondTypeStart = -1, CondTypeEnd = -1, NextCond = -1;

    for (auto& c : Condition)
    {
        if (CondTypeStart == -1)
        {
            bool ud = c == '>' || c == '<' || c == '=' || c == '&' || c == '|';

            if (!ud && strncmp(&c, "HasTag", 6) == 0)
            {
                CondTypeStart = &c - Condition.data();
                CondTypeEnd = &c - Condition.data() + 6;
                continue;
            }
            else if (!ud && strncmp(&c, "MissingTag", 10) == 0)
            {
                CondTypeStart = &c - Condition.data();
                CondTypeEnd = &c - Condition.data() + 10;
                continue;
            }
            else if (ud)
                CondTypeStart = &c - Condition.data();
        }
        else if (CondTypeStart != -1 && CondTypeEnd == -1 && (c != '>' && c != '<' && c != '=' && c != '&' && c != '|'))
            CondTypeEnd = &c - Condition.data();
        else if (CondTypeEnd != -1 && (c == '=' || c == '&' || c == '|'))
            NextCond = &c - Condition.data();

        if (CondTypeStart != -1 && CondTypeEnd != -1 && NextCond != -1)
            break;
    }

    if (CondTypeStart == Condition.npos)
    {
        CondTypeStart = Condition.find(" ");

        if (CondTypeStart == Condition.npos)
            return { false, NextCond };

        CondTypeStart++;

        if (CondTypeEnd == Condition.npos)
            CondTypeEnd = Condition.find(" ", CondTypeStart);

        NextCond = Condition.find("&&", CondTypeEnd + 1);

        if (NextCond == Condition.npos)
            NextCond = Condition.find("||", CondTypeEnd + 1);
    }
    else if (CondTypeEnd == Condition.npos)
        CondTypeEnd = CondTypeStart + 1;

    auto Left = Condition.substr(0, CondTypeStart - 1);
    auto CondType = Condition.substr(CondTypeStart, CondTypeEnd - CondTypeStart);
    auto Right = Condition.substr(CondTypeEnd + 1, NextCond == Condition.npos ? NextCond : (Condition.substr(NextCond - 1, 1) == " " ? NextCond - 1 : NextCond) - CondTypeEnd - 1);

    if (CondType != "HasTag" && CondType != "MissingTag")
        return { false, NextCond };

    const FGameplayTagContainer* Container = nullptr;

    if (Left == "Target.Tags")
        Container = &TargetTags;
    else if (Left == "Source.Tags")
        Container = &SourceTags;
    else if (Left == "Context.Tags")
        Container = &ContextTags;
    else
        return { false, NextCond };

    FGameplayTag Tag{};
    Tag.TagName = FName(std::wstring(Right.begin(), Right.end()).c_str());

    return { CondType == "HasTag" ? Container->HasTag(Tag) : !Container->HasTag(Tag), NextCond };
}

static bool IsConditionMet(const FString& InCondition, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags)
{
    if (InCondition.Num() <= 0)
        return true;

    auto Condition = InCondition.ToString();
    auto Result = ParseCondition(Condition, TargetTags, SourceTags, ContextTags);

    while (Result.NextStart != Condition.npos)
    {
        auto Start = Condition.substr(Result.NextStart, 2);

        if (Start != "&&" && Start != "||")
            break;

        Condition = Condition.substr(Result.NextStart + 2);

        if (Condition.substr(0, 1) == " ")
            Condition = Condition.substr(1);

        auto LastResult = Result;
        Result = ParseCondition(Condition, TargetTags, SourceTags, ContextTags);

        if (Start == "&&" && (!LastResult.bMatch || !Result.bMatch))
            Result.bMatch = false;
        else if (Start == "||" && (LastResult.bMatch || Result.bMatch))
            Result.bMatch = true;
    }

    return Result.bMatch;
}

static void GiveAccolade(AFortPlayerControllerAthena* PlayerController, UFortAccoladeItemDefinition* Accolade, FPrimaryAssetId AssetId)
{
    auto XPComponent = PlayerController->XPComponent;
    FXPEventInfo Info{};

    Info.Accolade = AssetId;

    float XpValue = Accolade->XpRewardAmount.Evaluate();

    if (XpValue == 0)
        UDataTableFunctionLibrary::EvaluateCurveTableRow(Accolade->XpRewardAmount.Curve.CurveTable, FName(L"Default_Medal"), Accolade->XpRewardAmount.Value, nullptr, &XpValue, FString());

    Info.EventXpValue = (int32)XpValue;
    Info.RestedValuePortion = Info.EventXpValue;
    Info.RestedXPRemaining = Info.EventXpValue;
    Info.TotalXpEarnedInMatch = Info.EventXpValue + XPComponent->TotalXpEarned;
    Info.Priority = Accolade->Priority;
    Info.SimulatedText = Accolade->ItemDescription;
    Info.SimulatedName = Accolade->ItemName;
    Info.EventName = Accolade->Name;
    Info.SeasonBoostValuePortion = 0;
    Info.PlayerController = PlayerController;

    XPComponent->MatchXp += Info.EventXpValue;
    XPComponent->TotalXpEarned += Info.EventXpValue;
    XPComponent->OnXPEvent(Info);

    if (Accolade->AccoladeType == EFortAccoladeType::Medal)
    {
        XPComponent->MedalsEarned.Add(Accolade);
        XPComponent->ClientMedalsRecived(XPComponent->PlayerAccolades);
    }
}

static void ProgressQuest(UFortQuestManager* QuestManager, AFortPlayerControllerAthena* PlayerController, UFortQuestItem* QuestItem, FName BackendName, int32 Count)
{
    auto QuestDefinition = (UFortQuestItemDefinition*)QuestItem->ItemDefinition;
    UFortQuestObjectiveInfo* QuestObjective = nullptr;

    for (auto Objective : QuestItem->Objectives)
    {
        if (Objective->BackendName == BackendName)
        {
            QuestObjective = Objective;
            break;
        }
    }

    if (!QuestObjective)
        return;

    if (QuestDefinition->bAthenaUpdateObjectiveOncePerMatch)
    {
        auto& OncePerMatchSet = OncePerMatchMap[QuestManager];

        if (OncePerMatchSet.contains(QuestDefinition))
            return;

        OncePerMatchSet.emplace(QuestDefinition);
    }

    auto AchievedCount = QuestObjective->AchievedCount;
    bool bObjectiveCompleted = AchievedCount + Count == QuestObjective->RequiredCount;

    for (auto Component : QuestManager->Components)
    {
        if (Component && Component->IsA(UFortQuestManagerComponent_Athena::StaticClass()))
        {
            ((UFortQuestManagerComponent_Athena*)Component)->HandleQuestObjectiveUpdated(PlayerController, QuestDefinition, BackendName, AchievedCount + Count, Count, nullptr, (EFortQuestObjectiveStatEvent)bObjectiveCompleted);
            break;
        }
    }
}

static void SendStatEvent__Internal(UFortQuestManager* QuestManager, AActor* PlayerController, EFortQuestObjectiveStatEvent StatEvent, int32 Count, UObject* TargetObject, FGameplayTagContainer TargetTags,
                                    FGameplayTagContainer SourceTags, FGameplayTagContainer ContextTags)
{
    static auto XPTable = FindObject<UDataTable>(L"/Game/Athena/Items/Quests/AthenaObjectiveStatXPTable.AthenaObjectiveStatXPTable");

    auto FortPC = (AFortPlayerControllerAthena*)PlayerController;

    if (XPTable)
    {
        for (auto& [Key, Value] : XPTable->RowMap)
        {
            auto Row = (FFortQuestObjectiveStatXPTableRow*)Value;

            if (Row->Type != StatEvent)
                continue;

            auto Accolade = (UFortAccoladeItemDefinition*)UKismetSystemLibrary::GetObjectFromPrimaryAssetId(Row->AccoladePrimaryAssetId);

            if (!Accolade)
                continue;

            if (Row->bOnceOnly && OnlyOnceMap.contains(Accolade))
                continue;

            if (!TargetTags.HasAll(Row->TargetTags) || !SourceTags.HasAll(Row->SourceTags) || !ContextTags.HasAll(Row->ContextTags))
                continue;

            if (TargetTags.HasAny(Row->ExcludeTargetTags) || SourceTags.HasAny(Row->ExcludeSourceTags) || ContextTags.HasAny(Row->ExcludeContextTags))
                continue;

            if (!IsConditionMet(Row->Condition, TargetTags, SourceTags, ContextTags) || !PlayerController)
                continue;

            auto PrimaryAssetName = Row->AccoladePrimaryAssetId.PrimaryAssetName.ComparisonIndex;
            CountThresholdMap[PlayerController][PrimaryAssetName] += Count;

            if (Row->CountThreshhold > 0 && CountThresholdMap[PlayerController][PrimaryAssetName] != Row->CountThreshhold)
                continue;

            auto AccoladeCount = 0;

            for (auto& AthenaAccolade : FortPC->XPComponent->PlayerAccolades)
            {
                if (AthenaAccolade.AccoladeDef == Accolade)
                {
                    AccoladeCount = AthenaAccolade.Count;
                    break;
                }
            }

            if (AccoladeCount > Row->MaxCount)
                continue;

            if (Row->bOnceOnly)
                OnlyOnceMap.emplace(Accolade);

            GiveAccolade(FortPC, Accolade, Row->AccoladePrimaryAssetId);
        }
    }

    for (auto Quest : QuestManager->CurrentQuests)
    {
        if (Quest->HasCompletedQuest())
            continue;

        auto QuestDefinition = (UFortQuestItemDefinition*)Quest->ItemDefinition;

        for (auto& Objective : QuestDefinition->Objectives)
        {
            if (Quest->HasCompletedObjectiveWithName(Objective.BackendName))
                continue;

            if (!Objective.ObjectiveStatHandle.RowName.IsNone() && Objective.ObjectiveStatHandle.DataTable)
            {
                for (auto& [Key, Value] : Objective.ObjectiveStatHandle.DataTable->RowMap)
                {
                    if (Key != Objective.ObjectiveStatHandle.RowName)
                        continue;

                    auto Row = (FFortQuestObjectiveStatTableRow*)Value;

                    if (Row->Type != StatEvent || !TargetTags.HasAll(Row->TargetTagContainer) || !SourceTags.HasAll(Row->SourceTagContainer) || !ContextTags.HasAll(Row->ContextTagContainer))
                        continue;

                    if (!IsConditionMet(Row->Condition, TargetTags, SourceTags, ContextTags))
                        continue;

                    ProgressQuest(QuestManager, FortPC, Quest, Objective.BackendName, Count);
                }

                continue;
            }

            for (auto& ObjectiveStat : Objective.InlineObjectiveStats)
            {
                if (ObjectiveStat.Type != StatEvent)
                    continue;

                bool bFound = true;

                for (auto& TagCondition : ObjectiveStat.TagConditions)
                {
                    bool bHasTag = false;

                    switch (TagCondition.Type)
                    {
                    case EInlineObjectiveStatTagCheckEntryType::Target:
                        bHasTag = TargetTags.HasTag(TagCondition.Tag);
                        break;
                    case EInlineObjectiveStatTagCheckEntryType::Source:
                        bHasTag = SourceTags.HasTag(TagCondition.Tag);
                        break;
                    case EInlineObjectiveStatTagCheckEntryType::Context:
                        bHasTag = ContextTags.HasTag(TagCondition.Tag);
                        break;
                    }

                    if (TagCondition.Require ? !bHasTag : bHasTag)
                    {
                        bFound = false;
                        break;
                    }
                }

                if (bFound && IsConditionMet(ObjectiveStat.Condition, TargetTags, SourceTags, ContextTags))
                    ProgressQuest(QuestManager, FortPC, Quest, Objective.BackendName, Count);
            }
        }
    }
}

static UFortPlaylistAthena* GetCurrentPlaylist()
{
    return ((AFortGameStateAthena*)UWorld::GetWorld()->GameState)->CurrentPlaylistInfo.BasePlaylist;
}

void SendStatEvent(UFortQuestManager* QuestManager, AActor* PlayerController, EFortQuestObjectiveStatEvent StatEvent, int32 Count, bool bAllowQueueStatEvent, UObject* TargetObject, FGameplayTagContainer TargetTags,
                   FGameplayTagContainer AdditionalSourceTags)
{
    if (!bAllowQueueStatEvent || !QuestManager)
        return;

    FGameplayTagContainer PlayerSourceTags;
    FGameplayTagContainer ContextTags;

    QuestManager->GetSourceAndContextTags(&PlayerSourceTags, &ContextTags);

    if (auto Playlist = GetCurrentPlaylist())
        ContextTags.AppendTags(Playlist->GameplayTagContainer);

    AdditionalSourceTags.AppendTags(PlayerSourceTags);

    SendStatEvent__Internal(QuestManager, PlayerController, StatEvent, Count, TargetObject, TargetTags, AdditionalSourceTags, ContextTags);
}

static void QueueStatEvent(UFortQuestManager* QuestManager, EFortQuestObjectiveStatEvent InType, UObject* InTargetObject, FGameplayTagContainer* InTargetTags, FGameplayTagContainer* InSourceTags,
                           FGameplayTagContainer* InContextTags, void* InObjectiveStat, FName InObjectiveBackendName, int32 InCount)
{
    if (auto Playlist = GetCurrentPlaylist())
        InContextTags->AppendTags(Playlist->GameplayTagContainer);

    SendStatEvent__Internal(QuestManager, QuestManager->GetPlayerControllerBP(), InType, InCount, InTargetObject, *InTargetTags, *InSourceTags, *InContextTags);
}

void UFortQuestManager__PostLoad()
{
    Hook(ImageBase + 0xA12E4E8, QueueStatEvent);
}
