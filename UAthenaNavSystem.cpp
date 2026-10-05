#include "pch.h"
#include "Shared.h"
#include "UAthenaNavSystem.h"
#include <fstream>

INIT_MODULE(UAthenaNavSystem);
INIT_TICKER(UAthenaNavSystem);

static void (*RemoveNavigationLocksOG)(UAthenaNavSystem*);

static constexpr uint32 CacheMagic = 0x31434E52;
static std::fstream CacheStream;
static int64 CacheSize = 0;
static uint32 FortniteMainVersion[10] = {};
static void* CustomVersions[2] = {};
static void* ArchiveVTable[0x2F] = {};
static uint64 Archive[8] = {};
static std::vector<std::pair<std::wstring, int32>> CachedAreas;
static std::vector<uint64> CachedLinks;
static bool bNavCacheDone = false;

static void ArchiveSerialize(void* Ar, void* Data, int64 Length)
{
    if (!(Archive[5] & 1))
        CacheStream.write((char*)Data, Length);
    else if (!CacheStream.read((char*)Data, Length))
        memset(Data, 0, Length);
}

static void* GetCustomVersions(void* Ar)
{
    return CustomVersions;
}

template <typename T>
static void Serialize(T& Value)
{
    ArchiveSerialize(Archive, &Value, sizeof(T));
}

static bool CanRead(uint64 Bytes)
{
    if (!(Archive[5] & 1) || (CacheStream && Bytes <= uint64(CacheSize - int64(CacheStream.tellg()))))
        return true;

    CacheStream.setstate(std::ios::failbit);

    return false;
}

static void Serialize(std::vector<uint64>& Values)
{
    auto Num = uint32(Values.size());

    Serialize(Num);

    if (!CanRead(uint64(Num) * sizeof(uint64)))
        Num = 0;

    Values.resize(Num);
    ArchiveSerialize(Archive, Values.data(), Num * sizeof(uint64));
}

static void Serialize(std::vector<std::pair<std::wstring, int32>>& Areas)
{
    auto Num = uint32(Areas.size());

    Serialize(Num);

    if (!CanRead(uint64(Num) * (sizeof(uint32) + sizeof(int32))))
        Num = 0;

    Areas.resize(Num);

    for (auto& [Name, Id] : Areas)
    {
        auto Length = uint32(Name.size());

        Serialize(Length);

        if (!CanRead(uint64(Length) * sizeof(wchar_t)))
            Length = 0;

        Name.resize(Length);
        ArchiveSerialize(Archive, Name.data(), Length * sizeof(wchar_t));
        Serialize(Id);
    }
}

static bool SerializeHeader(uint32 Stamp, uint8 (&Params)[0x7C], std::vector<std::pair<std::wstring, int32>>& Areas, std::vector<uint64>& Links, std::vector<uint64>& Columns, int32& Version, int64& PayloadSize)
{
    auto Magic = CacheMagic;
    auto SavedStamp = Stamp;
    uint8 SavedParams[0x7C];

    memcpy(SavedParams, Params, sizeof(SavedParams));

    Serialize(Magic);
    Serialize(SavedStamp);
    Serialize(SavedParams);

    if (!CacheStream || Magic != CacheMagic || SavedStamp != Stamp || memcmp(SavedParams, Params, sizeof(SavedParams)))
        return false;

    Serialize(Areas);
    Serialize(Links);
    Serialize(Columns);
    Serialize(Version);
    Serialize(PayloadSize);

    return bool(CacheStream);
}

static bool OpenCache(const std::wstring& Path, bool bLoading)
{
    CacheStream.clear();
    CacheStream.open(Path, std::ios::binary | (bLoading ? std::ios::in | std::ios::ate : std::ios::out | std::ios::trunc));
    Archive[5] = bLoading ? 1 : 4;

    if (bLoading)
    {
        CacheSize = int64(CacheStream.tellg());
        CacheStream.seekg(0);
    }

    return CacheStream.is_open();
}

static std::wstring GetCachePath()
{
    auto World = UWorld::GetWorld();
    auto GameState = (AFortGameStateAthena*)World->GameState;
    auto Playlist = GameState ? GameState->CurrentPlaylistInfo.BasePlaylist : nullptr;
    wchar_t LocalAppData[MAX_PATH];

    if (!Playlist || !GetEnvironmentVariableW(L"LOCALAPPDATA", LocalAppData, MAX_PATH))
        return std::wstring();

    auto Name = World->Name.GetRawString() + "_" + Playlist->PlaylistName.GetRawString() + ".bin";

    return std::wstring(LocalAppData) + L"\\FortniteGame\\Saved\\RemixNavCache_" + std::wstring(Name.begin(), Name.end());
}

static void GetParams(ARecastNavMesh* NavMesh, uint8 (&Params)[0x7C])
{
    memcpy(Params, (void*)(uint64(NavMesh) + 0x514), 0x54);
    memcpy(Params + 0x54, (void*)(uint64(NavMesh) + 0x570), 0x28);
}

static uint32 GetImageStamp()
{
    return ((PIMAGE_NT_HEADERS)(ImageBase + ((PIMAGE_DOS_HEADER)ImageBase)->e_lfanew))->FileHeader.TimeDateStamp;
}

template <typename F>
static void ForEachSetElement(uint64 Set, F Callback)
{
    auto Data = *(uint8**)Set;
    auto Bits = *(uint32**)(Set + 0x20) ? *(uint32**)(Set + 0x20) : (uint32*)(Set + 0x10);

    for (int32 i = 0; i < *(int32*)(Set + 0x8); i++)
    {
        if (Bits[i / 32] & (1u << (i % 32)))
            Callback(Data + i * 0x20);
    }
}

static std::vector<std::pair<std::wstring, int32>> GetAreas(ARecastNavMesh* NavMesh)
{
    std::vector<std::pair<std::wstring, int32>> Areas;

    for (auto& Area : NavMesh->SupportedAreas)
        Areas.push_back({ Area.AreaClassName.ToWString(), Area.AreaID });

    return Areas;
}

static std::vector<uint64> GetLinks(UNavigationSystemV1* NavSystem)
{
    std::vector<uint64> Links;

    ForEachSetElement(uint64(NavSystem) + 0x470, [&](uint8* Element) { Links.push_back(*(uint64*)Element); });

    return Links;
}

template <typename T>
static bool IsSubset(const std::vector<T>& Runtime, const std::vector<T>& Saved, bool bExact)
{
    if (bExact && Runtime.size() != Saved.size())
        return false;

    for (auto& Entry : Runtime)
    {
        if (std::find(Saved.begin(), Saved.end(), Entry) == Saved.end())
            return false;
    }

    return true;
}

static void RebuildAll(ARecastNavMesh* NavMesh)
{
    auto Generator = *(void**)(uint64(NavMesh) + 0x368);

    ((void (*)(void*))(*(void***)Generator)[0x1])(Generator);
}

static bool AttachCache(UNavigationSystemV1* NavSystem, ARecastNavMesh* NavMesh)
{
    static auto SerializeRecastData = (void (*)(UObject*, void*, int32))(ImageBase + 0x758AF98);
    static auto AttachTiles = (TArray<uint64>* (*)(UObject*, TArray<uint64>*, ARecastNavMesh*, bool, bool))(ImageBase + 0x7574304);
    static auto MarkEmptyTileCacheLayers = (void (*)(ARecastNavMesh*, int32, int32))(ImageBase + 0x7561F08);

    auto Path = GetCachePath();

    if (Path.empty() || !*(void**)(uint64(NavMesh) + 0x750) || !OpenCache(Path, true))
        return false;

    uint8 Params[0x7C];
    std::vector<uint64> Columns;
    int32 Version = 0;
    int64 PayloadSize = 0;
    UObject* Chunk = nullptr;

    GetParams(NavMesh, Params);

    if (SerializeHeader(GetImageStamp(), Params, CachedAreas, CachedLinks, Columns, Version, PayloadSize) && PayloadSize == CacheSize - int64(CacheStream.tellg()) &&
        Version >= 20 && Version <= 26 && IsSubset(GetAreas(NavMesh), CachedAreas, false) && IsSubset(GetLinks(NavSystem), CachedLinks, false))
    {
        Chunk = UGameplayStatics::SpawnObject(URecastNavMeshDataChunk::StaticClass(), NavMesh);
        *(FName*)(uint64(Chunk) + 0x28) = NavMesh->Name;
        SerializeRecastData(Chunk, Archive, Version);
    }

    auto bConsumed = CacheStream && int64(CacheStream.tellg()) == CacheSize;

    CacheStream.close();

    auto Tiles = Chunk && bConsumed ? *(int32*)(uint64(Chunk) + 0x38) : 0;

    if (!Tiles)
        return false;

    TArray<uint64> Refs;

    AttachTiles(Chunk, &Refs, NavMesh, false, false);

    auto bAttached = Refs.Num() == Tiles;

    if (!bAttached)
        RebuildAll(NavMesh);
    else
    {
        auto Generator = *(void**)(uint64(NavMesh) + 0x368);

        ((void (*)(void*))(*(void***)Generator)[0x3])(Generator);

        for (auto Column : Columns)
            MarkEmptyTileCacheLayers(NavMesh, int32(Column), int32(Column >> 32));

        ((void (*)(ARecastNavMesh*, TArray<uint64>*))NavMesh->VTable[0x119])(NavMesh, &Refs);
    }

    Refs.Free();

    return bAttached;
}

static void BakeCache(UNavigationSystemV1* NavSystem, ARecastNavMesh* NavMesh)
{
    static auto SerializeRecastData = (void (*)(UObject*, void*, int32))(ImageBase + 0x758AF98);

    TArray<ANavMeshBoundsVolume*> Volumes;
    FBox Bounds{};

    GetAll<ANavMeshBoundsVolume>(Volumes);

    for (auto Volume : Volumes)
    {
        if (!Volume || !(*(uint32*)&Volume->SupportedAgents & *(uint32*)&NavSystem->SupportedAgentsMask))
            continue;

        FVector Origin, Extent;
        Volume->GetActorBounds(false, &Origin, &Extent, false);

        auto Min = Origin - Extent;
        auto Max = Origin + Extent;

        if (Bounds.IsValid)
        {
            Min = FVector((std::min)(Min.X, Bounds.Min.X), (std::min)(Min.Y, Bounds.Min.Y), (std::min)(Min.Z, Bounds.Min.Z));
            Max = FVector((std::max)(Max.X, Bounds.Max.X), (std::max)(Max.Y, Bounds.Max.Y), (std::max)(Max.Z, Bounds.Max.Z));
        }

        Bounds.Min = Min;
        Bounds.Max = Max;
        Bounds.IsValid = true;
    }

    Volumes.Free();

    auto Path = GetCachePath();
    auto ChunkActor = Bounds.IsValid && !Path.empty() ? SpawnActor<ANavigationDataChunkActor>(FVector{}, FRotator{}) : nullptr;

    if (!ChunkActor)
        return;

    FBox TilesBounds{};

    ((void (*)(ARecastNavMesh*, FBox*, ANavigationDataChunkActor*, FBox*))NavMesh->VTable[0xEC])(NavMesh, &Bounds, ChunkActor, &TilesBounds);

    auto Chunk = ChunkActor->NavDataChunks.Num() > 0 ? ChunkActor->NavDataChunks[0] : nullptr;
    auto TempPath = Path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

    if (Chunk && OpenCache(TempPath, false))
    {
        uint8 Params[0x7C];
        auto Areas = GetAreas(NavMesh);
        auto Links = GetLinks(NavSystem);
        std::vector<uint64> Columns;
        int32 Version = 26;
        int64 PayloadSize = 0;

        GetParams(NavMesh, Params);

        ForEachSetElement(*(uint64*)(uint64(NavMesh) + 0x750) + 0x10, [&](uint8* Element)
        {
            if (!*(int32*)(Element + 0x10))
                Columns.push_back(*(uint64*)Element);
        });

        SerializeHeader(GetImageStamp(), Params, Areas, Links, Columns, Version, PayloadSize);

        auto PayloadStart = int64(CacheStream.tellp());

        SerializeRecastData(Chunk, Archive, Version);
        PayloadSize = int64(CacheStream.tellp()) - PayloadStart;
        CacheStream.seekp(PayloadStart - int64(sizeof(PayloadSize)));
        Serialize(PayloadSize);
        CacheStream.close();

        if (CacheStream.fail() || !MoveFileExW(TempPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(TempPath.c_str());
    }

    ChunkActor->K2_DestroyActor();
}

static void RemoveNavigationLocks(UAthenaNavSystem* NavSystem)
{
    auto NavMesh = (ARecastNavMesh*)NavSystem->MainNavData;

    if ((*(uint8*)(uint64(NavSystem) + 0x3C8) & 1) && NavMesh && NavMesh->IsA(AAthenaNavMesh::StaticClass()) && *(void**)(uint64(NavMesh) + 0x368))
        bNavCacheAttached = AttachCache(NavSystem, NavMesh);

    RemoveNavigationLocksOG(NavSystem);
}

void UAthenaNavSystem__Init()
{
    memcpy(FortniteMainVersion, (void*)(ImageBase + 0xF09A790), 0x10);
    FortniteMainVersion[4] = 170;
    CustomVersions[0] = FortniteMainVersion;
    CustomVersions[1] = (void*)0x100000001;
    ArchiveVTable[0xB] = (void*)GetCustomVersions;
    ArchiveVTable[0x2E] = (void*)ArchiveSerialize;
    Archive[0] = uint64(ArchiveVTable);
    Archive[1] = uint64(&Archive[2]);

    Hook(ImageBase + 0xA5DB3C0, RemoveNavigationLocks, RemoveNavigationLocksOG);
}

void UAthenaNavSystem__Tick()
{
    if (bNavCacheDone)
        return;

    auto World = UWorld::GetWorld();

    if (!World || !World->GameState || !World->GameState->HasMatchStarted())
        return;

    bNavCacheDone = true;

    auto NavSystem = (UNavigationSystemV1*)World->NavigationSystem;
    auto NavMesh = NavSystem ? (ARecastNavMesh*)NavSystem->MainNavData : nullptr;

    if (!NavMesh || !NavMesh->IsA(AAthenaNavMesh::StaticClass()) || !*(void**)(uint64(NavMesh) + 0x368))
        return;

    if (bNavCacheAttached && (!IsSubset(GetAreas(NavMesh), CachedAreas, true) || !IsSubset(GetLinks(NavSystem), CachedLinks, true)))
    {
        RebuildAll(NavMesh);
        DeleteFileW(GetCachePath().c_str());
        bNavCacheAttached = false;
    }
    else if (!bNavCacheAttached && !UNavigationSystemV1::IsNavigationBeingBuiltOrLocked(World))
        BakeCache(NavSystem, NavMesh);
}
