#include "pch.h"
#include "Shared.h"

INIT_MODULE(UFortAthenaAISpawnerDataComponent_CosmeticLibrary);

static float RandomFraction()
{
    return float(rand()) / (float(RAND_MAX) + 1.f);
}

static UObject* LoadPrimaryAsset(const FPrimaryAssetId& PrimaryAssetId)
{
    if (auto Object = UKismetSystemLibrary::GetObjectFromPrimaryAssetId(PrimaryAssetId))
        return Object;

    return UKismetSystemLibrary::LoadAsset_Blocking(UKismetSystemLibrary::GetSoftObjectReferenceFromPrimaryAssetId(PrimaryAssetId));
}

static std::vector<FPrimaryAssetId> GetPrimaryAssetIds(const wchar_t* PrimaryAssetType)
{
    TArray<FPrimaryAssetId> List;
    std::vector<FPrimaryAssetId> Ids;

    UKismetSystemLibrary::GetPrimaryAssetIdList(FPrimaryAssetType{ FName(PrimaryAssetType) }, &List);

    for (auto& Id : List)
        Ids.push_back(Id);

    List.Free();

    return Ids;
}

static UObject* PickItem(std::vector<FPrimaryAssetId>& Ids, const UClass* Class)
{
    while (!Ids.empty())
    {
        auto Index = size_t(RandomFraction() * float(Ids.size()));
        auto Item = LoadPrimaryAsset(Ids[Index]);

        if (Item && Item->IsA(Class))
            return Item;

        Ids[Index] = Ids.back();
        Ids.pop_back();
    }

    return nullptr;
}

static bool HasOwnedTag(UObject* Object, const FName& TagName)
{
    auto TagInterface = Object->GetInterface(IGameplayTagAssetInterface::StaticClass());
    FGameplayTagContainer OwnedTags{};
    bool bHasTag = false;

    if (!TagInterface)
        return false;

    ((void (*)(void*, FGameplayTagContainer*))(*(void***)TagInterface)[0x2])(TagInterface, &OwnedTags);

    for (auto& Tag : OwnedTags.GameplayTags)
        bHasTag |= Tag.TagName == TagName;

    OwnedTags.GameplayTags.Free();
    OwnedTags.ParentTags.Free();

    return bHasTag;
}

static void GetLoadout(UFortAthenaAISpawnerDataComponent_CosmeticLibrary*, FFortAthenaLoadout* Loadout)
{
    static auto Characters = GetPrimaryAssetIds(L"AthenaCharacter");
    static auto Backpacks = GetPrimaryAssetIds(L"AthenaBackpack");
    static auto Gliders = GetPrimaryAssetIds(L"AthenaGlider");
    static auto Pickaxes = GetPrimaryAssetIds(L"AthenaPickaxe");
    static auto Contrails = GetPrimaryAssetIds(L"AthenaSkyDiveContrail");

    Loadout->Character = (UAthenaCharacterItemDefinition*)PickItem(Characters, UAthenaCharacterItemDefinition::StaticClass());
    Loadout->Backpack = (UAthenaBackpackItemDefinition*)PickItem(Backpacks, UAthenaBackpackItemDefinition::StaticClass());
    Loadout->Glider = (UAthenaGliderItemDefinition*)PickItem(Gliders, UAthenaGliderItemDefinition::StaticClass());
    Loadout->Pickaxe = (UAthenaPickaxeItemDefinition*)PickItem(Pickaxes, UAthenaPickaxeItemDefinition::StaticClass());
    Loadout->SkyDiveContrail = (UAthenaSkyDiveContrailItemDefinition*)PickItem(Contrails, UAthenaSkyDiveContrailItemDefinition::StaticClass());
}

static void GetDances(UFortAthenaAISpawnerDataComponent_CosmeticLibrary* Component, TArray<UAthenaDanceItemDefinition*>* Dances, AFortAthenaAIBotController*)
{
    static auto Synced = FName(L"Cosmetics.UserFacingFlags.Synced");

    auto& Libraries = Component->CosmeticLibraries;
    std::vector<float> LibraryWeights;
    float TotalWeight = 0.f;

    for (int32 i = 0; i < Libraries.Num(); i++)
    {
        auto Weight = Component->DefaultCosmeticLibraryWeight.Evaluate(float(i));

        LibraryWeights.push_back(Weight > 0.f ? Weight : 0.f);
        TotalWeight += LibraryWeights.back();
    }

    if (TotalWeight <= 0.f)
        return;

    auto Roll = RandomFraction() * TotalWeight;
    int32 LibraryIndex = 0;

    for (; LibraryIndex < Libraries.Num() - 1 && Roll >= LibraryWeights[LibraryIndex]; LibraryIndex++)
        Roll -= LibraryWeights[LibraryIndex];

    auto Library = Libraries.Data[LibraryIndex].Get();
    auto EmotesDataTable = Library ? Library->EmotesDataTable.Get() : nullptr;

    if (!EmotesDataTable)
        return;

    std::vector<FFortBotCosmeticItemDataTableRow*> Rows;
    float RowsWeight = 0.f;

    for (auto& Pair : EmotesDataTable->RowMap)
    {
        auto Row = (FFortBotCosmeticItemDataTableRow*)Pair.Value();

        if (!Row || Row->Weight <= 0.f)
            continue;

        Rows.push_back(Row);
        RowsWeight += Row->Weight;
    }

    for (auto Remaining = int32(Component->EmotesMaxCount.Evaluate()); Remaining > 0 && !Rows.empty(); Remaining--)
    {
        auto RowRoll = RandomFraction() * RowsWeight;
        size_t RowIndex = 0;

        for (; RowIndex < Rows.size() - 1 && RowRoll >= Rows[RowIndex]->Weight; RowIndex++)
            RowRoll -= Rows[RowIndex]->Weight;

        auto Row = Rows[RowIndex];
        auto Dance = LoadPrimaryAsset(Row->PrimaryAssetId);

        RowsWeight -= Row->Weight;
        Rows[RowIndex] = Rows.back();
        Rows.pop_back();

        if (Dance && Dance->IsA(UAthenaDanceItemDefinition::StaticClass()) && !HasOwnedTag(Dance, Synced))
            Dances->Add((UAthenaDanceItemDefinition*)Dance);
    }
}

void UFortAthenaAISpawnerDataComponent_CosmeticLibrary__Init()
{
    Hook<UFortAthenaAISpawnerDataComponent_CosmeticLibrary>(uint32(0x5F), GetDances);
    Hook<UFortAthenaAISpawnerDataComponent_CosmeticLibrary>(uint32(0x62), GetLoadout);
}
