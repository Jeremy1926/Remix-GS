#include "pch.h"
#include "Shared.h"
#include "Config.h"
#include <winhttp.h>
#pragma comment(lib, "minhook.lib")
#pragma comment(lib, "winhttp.lib")

std::wstring BackendUrl;
std::wstring ClientId;
std::wstring ClientSecret;

static std::string VeloraRequest;
static std::wstring ServedPlaylist;
static std::atomic<bool> bGraceOver = false;

static DWORD CheckVelora(std::string& Response)
{
    static auto Session = WinHttpOpen(L"Remix-GS", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    wchar_t Host[256]{};
    wchar_t Path[1024]{};
    URL_COMPONENTS Url{};
    Url.dwStructSize = sizeof(Url);
    Url.lpszHostName = Host;
    Url.dwHostNameLength = _countof(Host);
    Url.lpszUrlPath = Path;
    Url.dwUrlPathLength = _countof(Path);

    if (!Session || !WinHttpCrackUrl(VeloraCheckUrl.c_str(), 0, 0, &Url))
        return 0;

    auto Connection = WinHttpConnect(Session, Host, Url.nPort, 0);
    auto Request = Connection ? WinHttpOpenRequest(Connection, L"POST", Path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    DWORD Status = 0;

    if (Request && WinHttpSendRequest(Request, L"Content-Type: application/json", (DWORD)-1, VeloraRequest.data(), (DWORD)VeloraRequest.size(), (DWORD)VeloraRequest.size(), 0) && WinHttpReceiveResponse(Request, nullptr))
    {
        DWORD Size = sizeof(Status);
        WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size, WINHTTP_NO_HEADER_INDEX);

        DWORD Available = 0;
        while (WinHttpQueryDataAvailable(Request, &Available) && Available > 0)
        {
            auto Offset = Response.size();
            Response.resize(Offset + Available);

            DWORD Read = 0;
            WinHttpReadData(Request, Response.data() + Offset, Available, &Read);
            Response.resize(Offset + Read);
        }
    }

    if (Request)
        WinHttpCloseHandle(Request);

    if (Connection)
        WinHttpCloseHandle(Connection);

    return Status;
}

static std::string JsonField(const std::string& Json, const std::string& Key)
{
    auto Start = Json.find("\"" + Key + "\":");

    if (Start == std::string::npos)
        return "";

    Start += Key.size() + 3;

    if (Start < Json.size() && Json[Start] == '"')
    {
        auto End = Json.find('"', ++Start);
        return End == std::string::npos ? "" : Json.substr(Start, End - Start);
    }

    return Json.substr(Start, Json.find_first_of(",}", Start) - Start);
}

static void Heartbeat(DWORD HeartbeatSeconds, ULONGLONG GraceSeconds)
{
    auto LastApproved = GetTickCount64();

    while (GetTickCount64() - LastApproved < GraceSeconds * 1000)
    {
        Sleep(HeartbeatSeconds * 1000);

        std::string Response;

        if (CheckVelora(Response) == 200)
            LastApproved = GetTickCount64();
    }

    bGraceOver = true;
}

static void VeloraTick()
{
    static auto GetOnlineSubsystem = (uint8* (*)(void*))(ImageBase + 0x25336F0);
    static auto GetMcpService = (FSharedRef* (*)(uint8*, FSharedRef*))(ImageBase + 0x30223F4);
    static auto GetCheckinService = (FSharedRef* (*)(uint8*, FSharedRef*))(ImageBase + 0x7487964);
    static auto GetIdentityInterface = (FSharedRef* (*)(uint8*, FSharedRef*))(ImageBase + 0x2BD8F1C);
    static auto TSharedRef_Destructor = (void (*)(FSharedRef*))(ImageBase + 0x14DA148);
    static auto FindServicePermissions = (uint8* (*)(uint8*, void*))(ImageBase + 0x3C253EC);
    static auto MakeServicePermissions = (void* (*)(void*, void*, void*, void*))(ImageBase + 0x2BF1684);
    static auto AddServicePermissions = (int32 (*)(void*, void*))(ImageBase + 0x2BF1790);
    static auto DestroyServicePermissions = (void (*)(void*))(ImageBase + 0x1805494);
    static auto AssignString = (FString* (*)(FString*, const FString*))(ImageBase + 0x4A3CC98);
    static auto GetGameCommandLine = (wchar_t* (*)())(ImageBase + 0x1819F38);
    static auto Strncat = (wchar_t* (*)(wchar_t*, const wchar_t*, int32))(ImageBase + 0x26A7A80);
    static auto GetQosInterface = (FSharedRef* (*)(FSharedRef*))(ImageBase + 0x20C5B10);
    static auto ForceSelectRegion = (void (*)(void*, const FString*))(ImageBase + 0x520CF10);
    static bool bServing = false;
    static bool bShutdownRequested = false;

    auto OnlineSubsystem = GetOnlineSubsystem((void*)(ImageBase + 0x12D183B8));
    FSharedRef Services[3]{};

    if (OnlineSubsystem)
    {
        GetMcpService(OnlineSubsystem, &Services[0]);
        GetCheckinService(OnlineSubsystem, &Services[1]);
        GetIdentityInterface(OnlineSubsystem, &Services[2]);
    }

    uint8* Configs[] = { Services[0].Object ? (uint8*)Services[0].Object + 0x10 : nullptr, Services[1].Object ? (uint8*)Services[1].Object + 0x10 : nullptr, Services[2].Object ? (uint8*)Services[2].Object + 0x198 : nullptr };

    for (auto Config : Configs)
    {
        if (!Config)
            continue;

        auto Permissions = FindServicePermissions(Config, (void*)(ImageBase + 0x12D18A90));
        auto Id = Permissions ? (FString*)(Permissions + 0x10) : nullptr;

        if (!Id || !*Id || wcscmp(Id->CStr(), ClientId.c_str()) != 0)
        {
            alignas(16) FString NewId(ClientId.c_str());
            alignas(16) FString NewKey(ClientSecret.c_str());

            if (Permissions)
            {
                AssignString(Id, &NewId);
                AssignString((FString*)(Permissions + 0x20), &NewKey);
            }
            else
            {
                alignas(16) uint8 NewPermissions[0x30];

                MakeServicePermissions(NewPermissions, (void*)(ImageBase + 0x12D18A90), &NewId, &NewKey);
                AddServicePermissions(Config + 0xB8, NewPermissions);
                DestroyServicePermissions(NewPermissions);
            }

            NewId.Free();
            NewKey.Free();
        }
    }

    for (auto& Service : Services)
        TSharedRef_Destructor(&Service);

    if (!bServing)
    {
        FSharedRef QosInterface{};
        alignas(16) FString RegionId(std::wstring(Region.begin(), Region.end()).c_str());

        bServing = true;
        Strncat(GetGameCommandLine(), (L" -ServesPlaylists=" + ServedPlaylist).c_str(), 0x4000);
        ++*(int32*)(ImageBase + 0x12B75064);

        auto RegionManager = GetQosInterface(&QosInterface)->Object ? *(void**)((uint8*)QosInterface.Object + 0x20) : nullptr;

        if (RegionManager)
            ForceSelectRegion(RegionManager, &RegionId);

        TSharedRef_Destructor(&QosInterface);
        RegionId.Free();
    }

    auto GameMode = UWorld::GetWorld()->AuthorityGameMode;
    auto GameSession = GameMode ? GameMode->GameSession : nullptr;

    if (bGraceOver && !bShutdownRequested && GameSession && GameSession->IsA(AFortGameSessionDedicatedAthena::StaticClass()))
    {
        auto RequestGracefulShutdown = (void (*)(AGameSession*, int32))GameSession->VTable[0x12C];

        bShutdownRequested = true;
        SetConsoleTitleA("Remix 32.00 | No answer from Velora, stopping");
        RequestGracefulShutdown(GameSession, 5);
    }
}

void Main()
{
    AllocConsole();
    FILE* Stream;
    freopen_s(&Stream, "CONOUT$", "w", stdout);
    freopen_s(&Stream, "CONOUT$", "w+", stderr);
    freopen_s(&Stream, "CONIN$", "r", stdin);

    ServedPlaylist = Playlist.substr(Playlist.rfind(L'.') + 1);

    std::string PlaylistName;

    for (auto Char : ServedPlaylist)
        PlaylistName += char(Char);

    VeloraRequest = std::format(R"({{"role":"gameserver","build":"++Fortnite+Release-32.00-CL-37770125","version":"{}","region":"{}","playlist":"{}","port":{}}})", __DATE__ " " __TIME__, Region, PlaylistName, Port);

    std::string Response;
    auto Status = CheckVelora(Response);
    auto Error = JsonField(Response, "errorCode");
    auto HeartbeatSeconds = atoi(JsonField(Response, "heartbeatSeconds").c_str());
    auto GraceSeconds = atoi(JsonField(Response, "graceSeconds").c_str());
    auto WideField = [&](const char* Key)
    {
        auto Value = JsonField(Response, Key);
        return std::wstring(Value.begin(), Value.end());
    };

    BackendUrl = WideField("backendUrl");
    ClientId = WideField("clientId");
    ClientSecret = WideField("clientSecret");

    if (Status != 200 || JsonField(Response, "status") != "approved" || BackendUrl.empty() || ClientId.empty() || ClientSecret.empty() || HeartbeatSeconds <= 0 || GraceSeconds < 0)
    {
        static auto RequestExitWithStatus = (void (*)(bool, uint8, const wchar_t*))(ImageBase + 0x41D939C);

        auto bPending = Status == 403 && Error == "errors.com.velora.gameserver.pending";
        auto bDisabled = Status == 403 && Error == "errors.com.velora.gameserver.disabled";

        if (bPending)
            SetConsoleTitleA("Remix 32.00 | Waiting for approval in Velora");
        else if (bDisabled)
            SetConsoleTitleA("Remix 32.00 | This server is disabled in Velora");
        else if (Status == 404)
            SetConsoleTitleA("Remix 32.00 | This build is no longer allowed");
        else if (Status == 429)
            SetConsoleTitleA("Remix 32.00 | Too many servers are waiting for approval");
        else if (Status == 422)
            SetConsoleTitleA("Remix 32.00 | Velora refused this request");
        else
            SetConsoleTitleA("Remix 32.00 | Velora unreachable");

        RequestExitWithStatus(true, bPending || bDisabled || Status == 404 ? 3 : 4, nullptr);
        return;
    }

    std::thread(Heartbeat, (DWORD)HeartbeatSeconds, (ULONGLONG)GraceSeconds).detach();
    Tickers.insert(Tickers.begin(), VeloraTick);

    Sleep(5000);

    for (auto Command : { L"log LogFortUIDirector None", L"log LogFortUIManager None", L"net.AllowEncryption 0", L"log LogIris None", L"log LogIrisRpc None", L"log LogIrisBridge None" })
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), Command, nullptr);

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

BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID Reserved)
{
    if (Reason == DLL_PROCESS_ATTACH)
        std::thread(Main).detach();

    return TRUE;
}
