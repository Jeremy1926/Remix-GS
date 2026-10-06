#include "pch.h"
#include "Shared.h"
#include "UFortControllerComponent_TournamentStatsTracker.h"
#include <objbase.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

INIT_MODULE(UFortControllerComponent_TournamentStatsTracker);
INIT_TICKER(UFortControllerComponent_TournamentStatsTracker);
INIT_RESET(UFortControllerComponent_TournamentStatsTracker);

constexpr auto MaxSendAttempts = 3;
constexpr auto SendRetryDelay = 2000;
constexpr auto EventsGameId = L"Fortnite";

struct FTournamentPlayer
{
    TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
    uint8 TeamIndex = 0;
    float EliminatedTime = -1.f;
    bool bReported = false;
    bool bInTournament = false;
};

struct FTournamentRequest
{
    std::wstring Path;
    std::string Body;
    bool bMatchPlayers = false;
};

struct FTournamentAssignment
{
    std::string AccountId;
    std::string EventId;
    std::string EventWindowId;
};

static std::wstring BackendUrl;
static std::wstring ClientCredentials;
static std::wstring HostLogin;
static std::wstring HostPassword;
static std::string AccessToken;
static HINTERNET Session = nullptr;

static std::mutex RequestMutex;
static std::condition_variable RequestSignal;
static std::deque<FTournamentRequest> Requests;

static std::mutex AssignmentMutex;
static std::atomic<bool> bAssignmentsReady = false;
static std::vector<FTournamentAssignment> Assignments;

static std::unordered_map<std::string, FTournamentPlayer> TournamentPlayers;
static std::unordered_map<uint8, int32> TeamEliminations;
static std::wstring MatchId;
static float MatchStartTime = 0.f;
static int32 StartingTeams = 0;

static std::wstring CommandLineValue(const std::wstring& Name)
{
    int32 Count = 0;
    auto Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    auto Prefix = L"-" + Name + L"=";
    std::wstring Value;

    for (int32 i = 0; Arguments && i < Count; i++)
    {
        if (_wcsnicmp(Arguments[i], Prefix.c_str(), Prefix.size()) == 0)
            Value = Arguments[i] + Prefix.size();
    }

    LocalFree(Arguments);
    return Value;
}

static std::string Utf8(const std::wstring& Text)
{
    auto Size = WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), (int32)Text.size(), nullptr, 0, nullptr, nullptr);
    std::string Result(Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), (int32)Text.size(), Result.data(), Size, nullptr, nullptr);
    return Result;
}

static std::wstring Wide(const std::string& Text)
{
    auto Size = MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), (int32)Text.size(), nullptr, 0);
    std::wstring Result(Size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), (int32)Text.size(), Result.data(), Size);
    return Result;
}

static std::string UrlEncode(const std::string& Text)
{
    std::string Result;

    for (auto Char : Text)
    {
        if (isalnum((uint8)Char) || Char == '-' || Char == '_' || Char == '.' || Char == '~')
            Result += Char;
        else
            Result += std::format("%{:02X}", (uint8)Char);
    }

    return Result;
}

static std::string Base64(const std::string& Text)
{
    DWORD Size = 0;
    CryptBinaryToStringA((const BYTE*)Text.data(), (DWORD)Text.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &Size);

    std::string Result(Size, '\0');
    CryptBinaryToStringA((const BYTE*)Text.data(), (DWORD)Text.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, Result.data(), &Size);
    Result.resize(Size);
    return Result;
}

static std::string JsonString(const std::string& Json, const std::string& Key, size_t& Cursor)
{
    auto Pattern = "\"" + Key + "\":\"";
    auto Start = Json.find(Pattern, Cursor);

    if (Start == std::string::npos)
    {
        Cursor = std::string::npos;
        return "";
    }

    Start += Pattern.size();
    auto End = Json.find('"', Start);
    Cursor = End;
    return End == std::string::npos ? "" : Json.substr(Start, End - Start);
}

static DWORD Send(const std::wstring& Path, const std::string& Body, const std::wstring& Headers, std::string& Response)
{
    wchar_t Host[256]{};
    URL_COMPONENTS Url{};
    Url.dwStructSize = sizeof(Url);
    Url.lpszHostName = Host;
    Url.dwHostNameLength = _countof(Host);

    if (!WinHttpCrackUrl(BackendUrl.c_str(), 0, 0, &Url))
        return 0;

    auto Connection = WinHttpConnect(Session, Host, Url.nPort, 0);
    auto Request = Connection ? WinHttpOpenRequest(Connection, L"POST", Path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    DWORD Status = 0;

    if (Request && WinHttpSendRequest(Request, Headers.c_str(), (DWORD)-1, (LPVOID)Body.data(), (DWORD)Body.size(), (DWORD)Body.size(), 0) && WinHttpReceiveResponse(Request, nullptr))
    {
        DWORD Size = sizeof(Status);
        WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size, WINHTTP_NO_HEADER_INDEX);

        Response.clear();
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

static bool Login()
{
    auto Body = "grant_type=password&username=" + UrlEncode(Utf8(HostLogin)) + "&password=" + UrlEncode(Utf8(HostPassword));
    auto Headers = L"Content-Type: application/x-www-form-urlencoded\r\nAuthorization: basic " + Wide(Base64(Utf8(ClientCredentials)));

    std::string Response;
    size_t Cursor = 0;
    AccessToken = Send(L"/account/api/oauth/token", Body, Headers, Response) == 200 ? JsonString(Response, "access_token", Cursor) : "";
    return !AccessToken.empty();
}

static void ReadAssignments(const std::string& Response)
{
    std::vector<FTournamentAssignment> Read;
    size_t Cursor = 0;

    while (true)
    {
        FTournamentAssignment Assignment;
        Assignment.AccountId = JsonString(Response, "accountId", Cursor);
        Assignment.EventId = JsonString(Response, "eventId", Cursor);
        Assignment.EventWindowId = JsonString(Response, "eventWindowId", Cursor);

        if (Cursor == std::string::npos)
            break;

        Read.push_back(std::move(Assignment));
    }

    std::lock_guard Lock(AssignmentMutex);
    Assignments.insert(Assignments.end(), Read.begin(), Read.end());
    bAssignmentsReady = true;
}

static void TournamentWorker()
{
    while (true)
    {
        FTournamentRequest Request;

        {
            std::unique_lock Lock(RequestMutex);
            RequestSignal.wait(Lock, [] { return !Requests.empty(); });
            Request = std::move(Requests.front());
            Requests.pop_front();
        }

        std::string Response;
        DWORD Status = 0;

        for (int32 Attempt = 0; Attempt < MaxSendAttempts && Status != 200; Attempt++)
        {
            if (AccessToken.empty() && !Login())
            {
                Sleep(SendRetryDelay);
                continue;
            }

            Status = Send(Request.Path, Request.Body, L"Content-Type: application/json\r\nAuthorization: bearer " + Wide(AccessToken), Response);

            if (Status == 401)
                AccessToken.clear();
            else if (Status == 0 || Status >= 500)
                Sleep(SendRetryDelay);
            else if (Status != 200)
                break;
        }

        if (Status == 200 && Request.bMatchPlayers)
            ReadAssignments(Response);
    }
}

static void Queue(const std::wstring& Route, std::string Body, bool bMatchPlayers)
{
    if (!Session)
        return;

    {
        std::lock_guard Lock(RequestMutex);
        Requests.push_back({ std::wstring(L"/api/v1/events/") + EventsGameId + L"/gameserver/matches/" + MatchId + Route, std::move(Body), bMatchPlayers });
    }

    RequestSignal.notify_one();
}

static std::string GetAccountId(AFortPlayerStateAthena* PlayerState)
{
    auto DebugString = UFortKismetLibrary::GetDebugStringForUniqueId(PlayerState->UniqueID);
    auto Text = DebugString.ToString();
    DebugString.Free();

    if (Text.size() < 32)
        return "";

    auto AccountId = Text.substr(Text.size() - 32);

    for (auto& Char : AccountId)
    {
        if (!isxdigit((uint8)Char))
            return "";

        Char = (char)tolower((uint8)Char);
    }

    return AccountId;
}

static float GetMatchTime()
{
    return (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) - MatchStartTime;
}

static void ReportTeamEliminations(uint8 TeamIndex, int32 Eliminations)
{
    for (auto& [AccountId, Player] : TournamentPlayers)
    {
        auto PlayerState = Player.PlayerState.Get();

        if (Player.TeamIndex != TeamIndex || !Player.bInTournament || !PlayerState)
            continue;

        FFortTournamentStatInfo TournamentStatInfo{};
        TournamentStatInfo.StatName = FString(L"TEAM_ELIMS_STAT_INDEX");
        TournamentStatInfo.StatDisplayName = FName(L"Eliminations");
        TournamentStatInfo.bIsDeltaCount = false;
        TournamentStatInfo.StatValue = Eliminations;

        PlayerState->ClientReportTournamentStatUpdate(TournamentStatInfo);
        TournamentStatInfo.StatName.Free();
    }
}

static void BuildAndSendTournamentStats(uint8 TeamIndex, int32 Placement)
{
    std::string Results;

    for (auto& [AccountId, Player] : TournamentPlayers)
    {
        if (Player.TeamIndex != TeamIndex || Player.bReported || !Player.bInTournament)
            continue;

        Player.bReported = true;
        auto TimeAlive = Player.EliminatedTime >= 0.f ? Player.EliminatedTime : GetMatchTime();

        Results += std::format(R"({}{{"accountId":"{}","trackedStats":{{"PLACEMENT_STAT_INDEX":{},"TEAM_ELIMS_STAT_INDEX":{},"VICTORY_ROYALE_STAT":{},"TIME_ALIVE_STAT":{},"MATCH_PLAYED_STAT":1,"PLACEMENT_TIEBREAKER_STAT":{}}}}})",
                               Results.empty() ? "" : ",", AccountId, Placement, TeamEliminations[TeamIndex], Placement == 1 ? 1 : 0, (int32)TimeAlive, StartingTeams > Placement ? StartingTeams - Placement : 0);
    }

    if (!Results.empty())
        Queue(L"/results", R"({"results":[)" + Results + "]}", false);
}

static std::set<uint8> GetAliveTeams(AFortGameModeAthena* GameMode)
{
    std::set<uint8> AliveTeams;

    for (auto& PlayerController : GameMode->AlivePlayers)
    {
        if (PlayerController && PlayerController->PlayerState)
            AliveTeams.insert(((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex);
    }

    for (auto& BotController : GameMode->AliveBots)
    {
        if (BotController && BotController->PlayerState)
            AliveTeams.insert(((AFortPlayerStateAthena*)BotController->PlayerState)->TeamIndex);
    }

    return AliveTeams;
}

void TournamentMatchStarted(AFortGameModeAthena* GameMode)
{
    if (!Session)
        return;

    GUID Guid{};
    CoCreateGuid(&Guid);
    MatchId = Wide(std::format("{:08x}{:04x}{:04x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", Guid.Data1, Guid.Data2, Guid.Data3, Guid.Data4[0], Guid.Data4[1], Guid.Data4[2], Guid.Data4[3], Guid.Data4[4], Guid.Data4[5], Guid.Data4[6], Guid.Data4[7]));
    MatchStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    TournamentPlayers.clear();
    TeamEliminations.clear();

    std::string AccountIds;

    for (auto& PlayerController : GameMode->AlivePlayers)
    {
        auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;
        auto AccountId = PlayerState ? GetAccountId(PlayerState) : "";

        if (AccountId.empty() || TournamentPlayers.contains(AccountId))
            continue;

        TournamentPlayers[AccountId] = { TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState), PlayerState->TeamIndex };
        AccountIds += std::format(R"({}"{}")", AccountIds.empty() ? "" : ",", AccountId);
    }

    StartingTeams = (int32)GetAliveTeams(GameMode).size();

    if (!AccountIds.empty())
        Queue(L"/players", R"({"accountIds":[)" + AccountIds + "]}", true);
}

void TournamentPlayerEliminated(AFortGameModeAthena* GameMode, AFortPlayerStateAthena* PlayerState, AFortPlayerStateAthena* KillerPlayerState)
{
    if (MatchId.empty() || !PlayerState)
        return;

    auto Found = TournamentPlayers.find(GetAccountId(PlayerState));

    if (Found != TournamentPlayers.end() && Found->second.EliminatedTime < 0.f)
        Found->second.EliminatedTime = GetMatchTime();

    if (KillerPlayerState && KillerPlayerState->TeamIndex != PlayerState->TeamIndex)
        ReportTeamEliminations(KillerPlayerState->TeamIndex, ++TeamEliminations[KillerPlayerState->TeamIndex]);

    auto AliveTeams = GetAliveTeams(GameMode);

    if (!AliveTeams.contains(PlayerState->TeamIndex))
        BuildAndSendTournamentStats(PlayerState->TeamIndex, (int32)AliveTeams.size() + 1);

    if (AliveTeams.size() == 1)
        BuildAndSendTournamentStats(*AliveTeams.begin(), 1);
}

void UFortControllerComponent_TournamentStatsTracker__Init()
{
    BackendUrl = CommandLineValue(L"TournamentBackend");
    ClientCredentials = CommandLineValue(L"TournamentClient");
    HostLogin = CommandLineValue(L"AUTH_LOGIN");
    HostPassword = CommandLineValue(L"AUTH_PASSWORD");

    if (BackendUrl.empty() || ClientCredentials.empty() || HostLogin.empty())
        return;

    Session = WinHttpOpen(L"Remix-GS", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (Session)
        std::thread(TournamentWorker).detach();
}

void UFortControllerComponent_TournamentStatsTracker__Tick()
{
    if (!bAssignmentsReady.exchange(false))
        return;

    std::vector<FTournamentAssignment> Ready;

    {
        std::lock_guard Lock(AssignmentMutex);
        Ready.swap(Assignments);
    }

    for (auto& Assignment : Ready)
    {
        auto Found = TournamentPlayers.find(Assignment.AccountId);

        if (Found == TournamentPlayers.end())
            continue;

        Found->second.bInTournament = true;

        auto PlayerState = Found->second.PlayerState.Get();

        if (!PlayerState)
            continue;

        FEventTournamentIds EventIds{};
        EventIds.EventId = FString(Wide(Assignment.EventId).c_str());
        EventIds.WindowId = FString(Wide(Assignment.EventWindowId).c_str());

        PlayerState->ClientNotifyMatchEntered(EventIds);

        EventIds.EventId.Free();
        EventIds.WindowId.Free();
    }
}

void UFortControllerComponent_TournamentStatsTracker__Reset()
{
    {
        std::lock_guard Lock(AssignmentMutex);
        Assignments.clear();
        bAssignmentsReady = false;
    }

    TournamentPlayers.clear();
    TeamEliminations.clear();
    MatchId.clear();
    MatchStartTime = 0.f;
    StartingTeams = 0;
}
