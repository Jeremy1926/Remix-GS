#include "pch.h"
#include "Shared.h"
#include "Config.h"
#pragma comment(lib, "minhook.lib")

void Main()
{
    AllocConsole();
    FILE* s;
    freopen_s(&s, "CONOUT$", "w", stdout);
    freopen_s(&s, "CONOUT$", "w+", stderr);
    freopen_s(&s, "CONIN$", "r", stdin);

    Sleep(5000);

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"log LogFortUIDirector None", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"log LogFortUIManager None", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"net.AllowEncryption 0", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"log LogIris None", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"log LogIrisRpc None", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"log LogIrisBridge None", nullptr);

    *(int32_t*)(ImageBase + 0x12A8B1D8) = 2;
    *(bool*)(ImageBase + 0x12C18DC8) = false;
    *(uint32_t*)(ImageBase + 0x12D6E070) = 1;
    
    auto FortInventoryName = FName(L"/Script/FortniteGame.FortInventory");

    for (auto& FilterConfig : UObjectReplicationBridgeConfig::GetDefaultObj()->FilterConfigs)
    {
        if (FilterConfig.ClassName == FortInventoryName)
        {
            FilterConfig.DynamicFilterName.ComparisonIndex = 0;
            break;
        }
    }

    SetConsoleTitleA("Remix 32.00 | Setting up");

    Patch<uint8_t>(ImageBase + 0x9EC47F0, 0xC3);
    Patch<uint8_t>(ImageBase + 0x3EF4558, 0xC3);
    Patch<uint8_t>(ImageBase + 0x41D9328, 0xC3);
    Patch<uint8_t>(ImageBase + 0x41D939C, 0xC3);
    Patch<uint8_t>(ImageBase + 0xC339E70, 0xC3);
    Patch<uint32_t>(ImageBase + 0x9CDF2E0, 0xC0FFC031);
    Patch<uint8_t>(ImageBase + 0x9CDF2E4, 0xC3);
    Patch<uint32_t>(ImageBase + 0x9CF42D8, 0xC0FFC031);
    Patch<uint8_t>(ImageBase + 0x9CF42DC, 0xC3);
    Patch<uint32_t>(ImageBase + 0x9019050, 0xC0FFC031);
    Patch<uint8_t>(ImageBase + 0x9019054, 0xC3);
    Patch<uint8_t>(ImageBase + 0x1BB250B, 0x85);
    Patch<uint8_t>(ImageBase + 0x30D2EB7, 0xEB);
    Patch<uint16_t>(ImageBase + 0x1F9E30E, 0x01B0);
    Patch<uint32_t>(ImageBase + 0x1F9E310, 0x90909090);

    MH_Initialize();

    for (auto& Initter : Initters)
        Initter();

    MH_EnableHook(MH_ALL_HOOKS);

    *(bool*)(ImageBase + 0x12B8D152) = false;
    *(bool*)(ImageBase + 0x12B8D0F4) = true;

    srand((uint32_t)time(0));

    UWorld::GetWorld()->OwningGameInstance->LocalPlayers.Remove(0);

    auto PlaylistAsset = FindObject<UFortPlaylistAthena>(Playlist.c_str());
    auto PersistentLevel = PlaylistAsset ? PlaylistAsset->PreloadPersistentLevel.ObjectID.AssetPath.PackageName : FName();
    std::wstring Open = L"open Apollo_Terrain_Retro";

    if (!PersistentLevel.IsNone())
    {
        auto Level = PersistentLevel.GetRawString();
        Open = L"open " + std::wstring(Level.begin(), Level.end());
    }

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), Open.c_str(), nullptr);

    Patch<uint8_t>(ImageBase + 0x70EE77F, 0x84);

    for (auto& PostLoader : PostLoaders)
        PostLoader();

    MH_EnableHook(MH_ALL_HOOKS);

    bInit = true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        std::thread(Main).detach();
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
