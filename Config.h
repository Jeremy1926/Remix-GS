#pragma once

constexpr auto bProd = false;
inline auto Port = 7777;
inline std::wstring GameserverIP;
inline std::string Region = "EU";
inline bool bReload = true;
inline std::wstring ReloadPlaylist = L"/BlastBerry/Playlists/Playlist_PunchBerrySolo.Playlist_PunchBerrySolo";
inline std::wstring Playlist = bReload ? ReloadPlaylist : L"/BRPlaylists/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
inline bool bInit = false;
inline bool bReady = false;
inline bool bEnableZones = true;
inline bool bEvent = false;
inline bool bForceRespawns = true;
inline bool bStormRespawns = true;
inline bool bLateGame = true;
inline int LateGameZone = 3;
inline bool bLateGameLongZone = true;
inline bool bLateGameMovingBus = true;
inline std::vector<std::pair<std::wstring, int>> LateGameLoadout;
