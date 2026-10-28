/*
 * SCCT_OnlineCoop: Universal Online Co-op Restorer for Tom Clancy's Splinter Cell: Chaos Theory.
 * Compatibility depends on the game's executable build and verified patch bytes.
 *
 * Version 1.0.0-rc5: client-side Steam session recovery.
 */

#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>
#include <dbghelp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")

/* Dynamic resolution of Process APIs to eliminate anti-virus heuristic false positives */
typedef HANDLE (WINAPI *pfnOpenProcess)(DWORD, BOOL, DWORD);
typedef BOOL   (WINAPI *pfnVirtualProtectEx)(HANDLE, LPVOID, SIZE_T, DWORD, PDWORD);
typedef BOOL   (WINAPI *pfnReadProcessMemory)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T *);
typedef BOOL   (WINAPI *pfnWriteProcessMemory)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T *);
typedef HANDLE (WINAPI *pfnCreateToolhelp32Snapshot)(DWORD, DWORD);
typedef BOOL   (WINAPI *pfnProcess32First)(HANDLE, LPPROCESSENTRY32);
typedef BOOL   (WINAPI *pfnProcess32Next)(HANDLE, LPPROCESSENTRY32);

static pfnOpenProcess pOpenProcess = NULL;
static pfnVirtualProtectEx pVirtualProtectEx = NULL;
static pfnReadProcessMemory pReadProcessMemory = NULL;
static pfnWriteProcessMemory pWriteProcessMemory = NULL;
static pfnCreateToolhelp32Snapshot pCreateToolhelp32Snapshot = NULL;
static pfnProcess32First pProcess32First = NULL;
static pfnProcess32Next pProcess32Next = NULL;

static void init_process_apis(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;
    pOpenProcess = (pfnOpenProcess)GetProcAddress(k32, "OpenProcess");
    pVirtualProtectEx = (pfnVirtualProtectEx)GetProcAddress(k32, "VirtualProtectEx");
    pReadProcessMemory = (pfnReadProcessMemory)GetProcAddress(k32, "ReadProcessMemory");
    pWriteProcessMemory = (pfnWriteProcessMemory)GetProcAddress(k32, "WriteProcessMemory");
    pCreateToolhelp32Snapshot = (pfnCreateToolhelp32Snapshot)GetProcAddress(k32, "CreateToolhelp32Snapshot");
    pProcess32First = (pfnProcess32First)GetProcAddress(k32, "Process32First");
    pProcess32Next = (pfnProcess32Next)GetProcAddress(k32, "Process32Next");
}

#undef OpenProcess
#undef VirtualProtectEx
#undef ReadProcessMemory
#undef WriteProcessMemory
#undef CreateToolhelp32Snapshot
#undef Process32First
#undef Process32Next

#define OpenProcess pOpenProcess
#define VirtualProtectEx pVirtualProtectEx
#define ReadProcessMemory pReadProcessMemory
#define WriteProcessMemory pWriteProcessMemory
#define CreateToolhelp32Snapshot pCreateToolhelp32Snapshot
#define Process32First pProcess32First
#define Process32Next pProcess32Next

typedef uint64_t u64;

#define SCCT_VERSION "1.0.0-rc5"
#define DIRECT_IP_DEFAULT_PORT 7790

static const int DISC_PORTS[] = { 9102, 9103, 8878 };
#define NUM_DISC_PORTS 3

static const int GAME_PORTS[] = { 7777, 7776 };
#define NUM_GAME_PORTS 2

#define MAX_PEERS 4
#define MAX_CONNS 64
#define MAX_RELAY_DRAIN 128
#define IDENT_SIZE 160

#define SEND_UNRELIABLE_NO_NAGLE 1
#define SEND_RELIABLE 8
/* SteamNetworkingMessages can reopen a failed session when a new send is attempted.
 * Use it only for throttled recovery probes, never on ordinary traffic. */
#define SEND_AUTO_RESTART_BROKEN_SESSION 32

#define CH_GAME  0
#define CH_CTRL  1
#define CH_DISC  2
#define CH_VOICE 3

/* ---------------------------------------------------------------- Steam Flat API Declarations */

typedef struct { int32_t user; int32_t callback; uint8_t *param; int32_t param_size; } CallbackMsg;
#pragma pack(push, 8)
typedef struct { u64 call; int32_t callback; uint32_t param_size; } APICallCompleted;
typedef struct { int32_t result; u64 lobby; } LobbyCreated;
typedef struct { u64 lobby; uint32_t perms; bool locked; uint32_t response; } LobbyEnter;
typedef struct { u64 lobby, user_changed, making_change; uint32_t state; } LobbyChatUpdate;
typedef struct { u64 lobby, friend_id; } GameLobbyJoinRequested;
typedef struct { u64 friend_id; char connect[256]; } GameRichPresenceJoinRequested;
typedef struct { u64 game_id; uint32_t ip; uint16_t port, query_port; u64 lobby; } FriendGameInfo;
typedef struct { uint32_t m_nLobbiesMatching; } LobbyMatchList_t;
#pragma pack(pop)

/* SteamNetworkingMessagesSessionFailed_t embeds SteamNetConnectionInfo_t.
 * Keep this public-header field prefix so we can log the exact end code while
 * omitting the debug string, which can contain network addresses. */
#pragma pack(push, 8)
typedef struct {
    unsigned char identity[136];
    int64_t user_data;
    uint32_t listen_socket;
    unsigned char remote_address[18];
    uint16_t pad1;
    uint32_t pop_remote;
    uint32_t pop_relay;
    int32_t state;
    int32_t end_reason;
} SteamSessionFailurePrefix;
#pragma pack(pop)
typedef char SteamSessionFailureReasonOffsetMustBe180[
    (offsetof(SteamSessionFailurePrefix, end_reason) == 180) ? 1 : -1];

#define CB_LOBBY_JOIN_REQUESTED 333
#define CB_RICH_PRESENCE_JOIN   337
#define CB_LOBBY_ENTER          504
#define CB_LOBBY_CHAT_UPDATE    506
#define CB_LOBBY_MATCH_LIST     510
#define CB_LOBBY_CREATED        513
#define CB_CALL_COMPLETED       703
#define CB_SESSION_REQUEST      1251
#define CB_SESSION_FAILED       1252

static struct {
    bool (__cdecl *IsSteamRunning)(void);
    int (__cdecl *InitFlat)(char *err);
    void (__cdecl *Shutdown)(void);
    int32_t (__cdecl *GetHSteamPipe)(void);
    void (__cdecl *DispatchInit)(void);
    void (__cdecl *RunFrame)(int32_t);
    bool (__cdecl *GetNextCallback)(int32_t, CallbackMsg *);
    void (__cdecl *FreeLastCallback)(int32_t);
    bool (__cdecl *GetAPICallResult)(int32_t, u64, void *, int, int, bool *);
    void *(__cdecl *User)(void);
    void *(__cdecl *Matchmaking)(void);
    void *(__cdecl *Friends)(void);
    void *(__cdecl *Messages)(void);
    void *(__cdecl *Utils)(void);
    u64 (__cdecl *GetSteamID)(void *);
    u64 (__cdecl *CreateLobby)(void *, int, int);
    u64 (__cdecl *JoinLobby)(void *, u64);
    void (__cdecl *LeaveLobby)(void *, u64);
    bool (__cdecl *SetLobbyData)(void *, u64, const char *, const char *);
    const char *(__cdecl *GetLobbyData)(void *, u64, const char *);
    u64 (__cdecl *GetLobbyOwner)(void *, u64);
    bool (__cdecl *SetRichPresence)(void *, const char *, const char *);
    void (__cdecl *InviteDialog)(void *, u64);
    int (__cdecl *GetFriendCount)(void *, int);
    u64 (__cdecl *GetFriendByIndex)(void *, int, int);
    bool (__cdecl *GetFriendGamePlayed)(void *, u64, FriendGameInfo *);
    const char *(__cdecl *GetFriendPersonaName)(void *, u64);
    const char *(__cdecl *GetPersonaName)(void *);
    void (__cdecl *IdentSet)(void *, u64);
    u64 (__cdecl *IdentGet)(const void *);
    int (__cdecl *SendMessageToUser)(void *, const void *, const void *, uint32_t, int, int);
    int (__cdecl *ReceiveMessages)(void *, int, void **, int);
    bool (__cdecl *AcceptSession)(void *, const void *);
    bool (__cdecl *CloseSession)(void *, const void *);
    void (__cdecl *MessageRelease)(void *);
    bool (__cdecl *SetConfigInt)(void *, int, int32_t);
    void (__cdecl *InitRelay)(void *);

    /* Worldwide Lobby Matchmaking */
    u64 (__cdecl *RequestLobbyList)(void *);
    void (__cdecl *AddRequestLobbyListStringFilter)(void *, const char *, const char *, int);
    void (__cdecl *AddRequestLobbyListDistanceFilter)(void *, int);
    void (__cdecl *AddRequestLobbyListResultCountFilter)(void *, int);
    bool (__cdecl *RequestLobbyData)(void *, u64);
    u64 (__cdecl *GetLobbyByIndex)(void *, int);
    int (__cdecl *GetNumLobbyMembers)(void *, u64);
} S;

static void *g_user, *g_mm, *g_friends, *g_msgs, *g_utils;
static int32_t g_pipe;
static u64 g_me;
static bool g_steam_available = false;

/* ---------------------------------------------------------------- Peer & Connection Tracking */

typedef struct {
    int id;
    int port;
    SOCKET s;
    DWORD last;
} Conn;

typedef struct {
    u64 sid;
    Conn conns[MAX_CONNS];
    DWORD last_heard;
    DWORD last_ping;
    DWORD last_restart_tick;
    uint32_t packets_in;
    uint32_t packets_out;
} Peer;

static Peer g_peers[MAX_PEERS];
static u64 g_lobby = 0;
static bool g_lobby_failed = false;

static char g_game_exe_path[MAX_PATH];
static char g_game_dir[MAX_PATH];
static HANDLE g_game_proc = NULL;
static bool g_no_launch = false;

static volatile bool g_running = true;
static DWORD g_last_steam_send_error_log = 0;
static DWORD g_client_last_restart_tick = 0;
static DWORD g_client_recovery_due = 0;
static void scct_log(const char *fmt, ...);

static BOOL WINAPI console_ctrl_handler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- Utilities */

static void fail(const char *msg)
{
    printf("\n[ERROR] %s\n\nPress any key to exit.\n", msg);
    _getch();
    exit(1);
}

static void copy_to_clipboard(const char *text)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        memcpy(GlobalLock(hMem), text, len);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

static bool fetch_public_ip(char *out_ip, size_t max_len)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    struct hostent *he = gethostbyname("api.ipify.org");
    if (!he || !he->h_addr_list[0]) {
        closesocket(s);
        return false;
    }

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(80);
    memcpy(&target.sin_addr, he->h_addr_list[0], he->h_length);

    DWORD timeout = 2500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    if (connect(s, (struct sockaddr *)&target, sizeof(target)) != 0) {
        closesocket(s);
        return false;
    }

    const char *req = "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    send(s, req, (int)strlen(req), 0);

    char buf[1024];
    int got = recv(s, buf, sizeof(buf) - 1, 0);
    closesocket(s);

    if (got <= 0) return false;
    buf[got] = 0;

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return false;
    body += 4;

    while (*body == ' ' || *body == '\r' || *body == '\n') body++;
    char *end = body;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
    *end = 0;

    if (strlen(body) >= 7 && strlen(body) < max_len) {
        strncpy(out_ip, body, max_len);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- Steam Loader */

static FARPROC need_sym(HMODULE m, const char *name)
{
    FARPROC p = GetProcAddress(m, name);
    if (!p) {
        char buf[300];
        snprintf(buf, sizeof(buf), "steam_api.dll missing export '%s'.", name);
        fail(buf);
    }
    return p;
}

#define LOAD_SYM(field, name) *(FARPROC *)&S.field = need_sym(m, name)

static bool init_steam(void)
{
    FILE *f = fopen("steam_appid.txt", "r");
    if (f) fclose(f);
    else if ((f = fopen("steam_appid.txt", "w"))) { fputs("480", f); fclose(f); }

    HMODULE m = LoadLibraryA("steam_api.dll");
    if (!m) m = LoadLibraryA("..\\steam_api.dll");
    if (!m) return false;

    LOAD_SYM(IsSteamRunning, "SteamAPI_IsSteamRunning");
    LOAD_SYM(InitFlat, "SteamAPI_InitFlat");
    LOAD_SYM(Shutdown, "SteamAPI_Shutdown");
    LOAD_SYM(GetHSteamPipe, "SteamAPI_GetHSteamPipe");
    LOAD_SYM(DispatchInit, "SteamAPI_ManualDispatch_Init");
    LOAD_SYM(RunFrame, "SteamAPI_ManualDispatch_RunFrame");
    LOAD_SYM(GetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback");
    LOAD_SYM(FreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback");
    LOAD_SYM(GetAPICallResult, "SteamAPI_ManualDispatch_GetAPICallResult");

    FARPROC pUser = GetProcAddress(m, "SteamAPI_SteamUser_v023");
    if (!pUser) pUser = GetProcAddress(m, "SteamAPI_SteamUser_v021");
    if (!pUser) pUser = GetProcAddress(m, "SteamAPI_SteamUser_v020");
    if (!pUser) pUser = GetProcAddress(m, "SteamUser");
    if (!pUser) return false;
    *(FARPROC *)&S.User = pUser;

    FARPROC pMM = GetProcAddress(m, "SteamAPI_SteamMatchmaking_v009");
    if (!pMM) pMM = GetProcAddress(m, "SteamAPI_SteamMatchmaking_v008");
    if (!pMM) pMM = GetProcAddress(m, "SteamMatchmaking");
    if (!pMM) return false;
    *(FARPROC *)&S.Matchmaking = pMM;

    FARPROC pFriends = GetProcAddress(m, "SteamAPI_SteamFriends_v018");
    if (!pFriends) pFriends = GetProcAddress(m, "SteamAPI_SteamFriends_v017");
    if (!pFriends) pFriends = GetProcAddress(m, "SteamAPI_SteamFriends_v015");
    if (!pFriends) pFriends = GetProcAddress(m, "SteamFriends");
    if (!pFriends) return false;
    *(FARPROC *)&S.Friends = pFriends;

    LOAD_SYM(Messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");

    FARPROC pUtils = GetProcAddress(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    if (!pUtils) pUtils = GetProcAddress(m, "SteamAPI_SteamNetworkingUtils_SteamAPI_v003");
    if (!pUtils) pUtils = GetProcAddress(m, "SteamNetworkingUtils");
    if (!pUtils) return false;
    *(FARPROC *)&S.Utils = pUtils;

    LOAD_SYM(GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    LOAD_SYM(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    LOAD_SYM(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    LOAD_SYM(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    LOAD_SYM(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    LOAD_SYM(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    LOAD_SYM(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    LOAD_SYM(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    LOAD_SYM(InviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    LOAD_SYM(GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    LOAD_SYM(GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    LOAD_SYM(GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    LOAD_SYM(GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    LOAD_SYM(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    LOAD_SYM(IdentSet, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    LOAD_SYM(IdentGet, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    LOAD_SYM(SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    LOAD_SYM(ReceiveMessages, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    LOAD_SYM(AcceptSession, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    LOAD_SYM(CloseSession, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    LOAD_SYM(MessageRelease, "SteamAPI_SteamNetworkingMessage_t_Release");
    LOAD_SYM(SetConfigInt, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");

    /* Worldwide Matchmaking Symbols */
    LOAD_SYM(RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    LOAD_SYM(AddRequestLobbyListStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    LOAD_SYM(AddRequestLobbyListDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    LOAD_SYM(AddRequestLobbyListResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    LOAD_SYM(RequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    LOAD_SYM(GetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    LOAD_SYM(GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");

    if (!S.IsSteamRunning()) return false;

    char err[1024] = "";
    int r = S.InitFlat(err);
    if (r != 0) return false;

    S.DispatchInit();
    g_pipe = S.GetHSteamPipe();
    g_user = S.User();
    g_mm = S.Matchmaking();
    g_friends = S.Friends();
    g_msgs = S.Messages();
    g_utils = S.Utils();
    g_me = S.GetSteamID(g_user);

    FARPROC pRelay = GetProcAddress(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if (pRelay) ((void (__cdecl *)(void *))pRelay)(g_utils);

    S.SetConfigInt(g_utils, 9, 4 << 20);
    S.SetConfigInt(g_utils, 47, 4 << 20);
    /* Do not set config 31 here. Steam defines SDRClient_ForceProxyAddr (31) as a
     * global string, not an integer ICE switch. ICE candidate policy (104) is
     * connection-scoped, and SteamNetworkingMessages does not expose per-message
     * connection configuration. Leave Steam's transport policy at its default. */

    g_steam_available = true;
    return true;
}

static void ident(void *buf, u64 sid) { memset(buf, 0, IDENT_SIZE); S.IdentSet(buf, sid); }

static u64 msg_sender(void *msg)
{
    u64 sid = S.IdentGet((char *)msg + 16);
    if (!sid) sid = S.IdentGet((char *)msg + 12);
    return sid;
}

static int steam_send(u64 sid, const void *data, int size, int flags, int channel)
{
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    int result = S.SendMessageToUser(g_msgs, id, data, (uint32_t)size,
                                     (uint32_t)flags, channel);
    if (result != 1) {
        DWORD now = GetTickCount();
        if (now - g_last_steam_send_error_log >= 5000) {
            g_last_steam_send_error_log = now;
            scct_log("[STEAM] SendMessageToUser failed (result=%d channel=%d)", result, channel);
        }
    }
    return result;
}

static int steam_restart_peer(Peer *peer, const char *reason)
{
    DWORD now = GetTickCount();
    if (!peer || !peer->sid) return 0;
    if (peer->last_restart_tick && now - peer->last_restart_tick < 10000u) return 0;
    peer->last_restart_tick = now;
    unsigned char id[IDENT_SIZE];
    ident(id, peer->sid);
    int result = S.SendMessageToUser(g_msgs, id, "K", 1,
                                     SEND_RELIABLE | SEND_AUTO_RESTART_BROKEN_SESSION, CH_CTRL);
    scct_log("[STEAM] Recovery probe (%s) result=%d; requested a single session restart",
             reason ? reason : "session recovery", result);
    return result;
}

static int steam_restart_client(u64 sid, const char *reason)
{
    DWORD now = GetTickCount();
    if (!sid) return 0;
    if (g_client_last_restart_tick && now - g_client_last_restart_tick < 10000u) return 0;
    g_client_last_restart_tick = now;
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    int result = S.SendMessageToUser(g_msgs, id, "K", 1,
                                     SEND_RELIABLE | SEND_AUTO_RESTART_BROKEN_SESSION, CH_CTRL);
    scct_log("[STEAM] Recovery probe (%s) result=%d; requested a single session restart",
             reason ? reason : "session recovery", result);
    return result;
}

static const char *steam_failure_state_name(int state)
{
    switch (state) {
    case 0: return "none";
    case 1: return "connecting";
    case 2: return "finding route";
    case 3: return "connected";
    case 4: return "closed by peer";
    case 5: return "problem detected locally";
    case 6: return "fin wait";
    case 7: return "linger";
    case 8: return "dead";
    default: return "unknown state";
    }
}

static const char *steam_end_reason_name(int reason)
{
    /* Steam's public k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous value. */
    if (reason == 5008) return "P2P rendezvous failed (no specific cause supplied)";
    return "other/unspecified Steam end reason";
}

static void steam_accept(u64 sid)
{
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    S.AcceptSession(g_msgs, id);
}

static void steam_close(u64 sid)
{
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    S.CloseSession(g_msgs, id);
}

static const char *steam_friend_name(u64 sid)
{
    const char *n = S.GetFriendPersonaName(g_friends, sid);
    return (n && *n) ? n : "Agent";
}

/* ---------------------------------------------------------------- Sockets & Helpers */

static void configure_udp_socket(SOCKET s)
{
    if (s == INVALID_SOCKET) return;
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) != 0)
        scct_log("[NETWORK] Could not set UDP socket nonblocking (WSA=%d)", WSAGetLastError());
    DWORD bytes = 0;
    BOOL off = FALSE;
    if (WSAIoctl(s, 0x9800000C, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL) != 0)
        scct_log("[NETWORK] Could not disable UDP connection-reset notifications (WSA=%d)", WSAGetLastError());
    int buf_size = 1 << 20;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&buf_size, sizeof(buf_size)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&buf_size, sizeof(buf_size)) != 0)
        scct_log("[NETWORK] Could not enlarge UDP buffers (WSA=%d)", WSAGetLastError());
}

static SOCKET udp_bind(int port, bool broadcast, bool reuse)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    if (reuse) {
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
    }
    if (broadcast) {
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    configure_udp_socket(s);

    return s;
}

static SOCKET udp_connect_loopback(int port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        scct_log("[NETWORK] Loopback UDP connect to port %d failed (WSA=%d)", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    configure_udp_socket(s);

    return s;
}

/* ---------------------------------------------------------------- Alphanumeric Room Codes */

static void ip_to_code(const char *ip_str, char *code_out, size_t out_len)
{
    unsigned int a, b, c, d;
    char tail;
    if (sscanf(ip_str, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
        a <= 255 && b <= 255 && c <= 255 && d <= 255) {
        snprintf(code_out, out_len, "%02X%02X-%02X%02X", a, b, c, d);
    } else {
        snprintf(code_out, out_len, "%s", ip_str);
    }
}

static bool code_to_ip(const char *code_in, char *ip_out, size_t out_len)
{
    char clean[32] = "";
    int j = 0;
    for (int i = 0; code_in[i] && j < 16; i++) {
        if (isxdigit((unsigned char)code_in[i])) clean[j++] = code_in[i];
    }
    clean[j] = 0;
    if (j == 8) {
        unsigned int hexval = 0;
        if (sscanf(clean, "%x", &hexval) == 1) {
            snprintf(ip_out, out_len, "%u.%u.%u.%u",
                     (hexval >> 24) & 0xFF,
                     (hexval >> 16) & 0xFF,
                     (hexval >> 8) & 0xFF,
                     hexval & 0xFF);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- In-Game Menu Patcher */

static bool patch_game_disk(const char *exe_path)
{
    FILE *f = fopen(exe_path, "r+b");
    if (!f) { scct_log("[PATCH] Cannot open game executable for reading/writing: %s", exe_path); return false; }

    uint32_t jump = 0;
    uint8_t page = 0;
    bool changed = false;
    if (fseek(f, 0x24BAD0, SEEK_SET) != 0 || fread(&jump, 4, 1, f) != 1 ||
        fseek(f, 0x24B980, SEEK_SET) != 0 || fread(&page, 1, 1, f) != 1 ||
        (jump != 0x10B4BA8C && jump != 0x10B4BA6F) || (page != 0x50 && page != 0x4C)) {
        fclose(f);
        scct_log("[PATCH] Refusing unsupported executable bytes at menu patch offsets: %s", exe_path);
        return false;
    }

    /* Replace all 'MainTitleLan' (0x110DF404) & 'LanCaption' (0x110E2CC8) pushes with 'MainTitleOnline' & 'OnlineCaption' */
    static const struct { long off; uint32_t from; uint32_t to; } str_patches[] = {
        { 0x22403F, 0x110DF404, 0x110DF3E4 },
        { 0x22BDB9, 0x110DF404, 0x110DF3E4 },
        { 0x25119D, 0x110DF404, 0x110DF3E4 },
        { 0x252289, 0x110DF404, 0x110DF3E4 },
        { 0x24BB90, 0x110E2CC8, 0x110E2CAC }
    };
    uint32_t current[5];
    for (int i = 0; i < 5; i++) {
        if (fseek(f, str_patches[i].off, SEEK_SET) != 0 || fread(&current[i], 4, 1, f) != 1 ||
            (current[i] != str_patches[i].from && current[i] != str_patches[i].to)) {
            fclose(f);
            scct_log("[PATCH] Refusing unsupported executable bytes at string patch %d: %s", i, exe_path);
            return false;
        }
    }
    changed = (jump != 0x10B4BA6F || page != 0x4C);
    for (int i = 0; i < 5; i++) if (current[i] != str_patches[i].to) changed = true;

    char backup[MAX_PATH + 32];
    snprintf(backup, sizeof(backup), "%s.scct_original.bak", exe_path);
    if (changed && GetFileAttributesA(backup) == INVALID_FILE_ATTRIBUTES && !CopyFileA(exe_path, backup, TRUE)) {
        fclose(f);
        scct_log("[PATCH] Could not create executable backup (err=%lu); no patch written", GetLastError());
        return false;
    }

    bool write_ok = true;
    if (jump != 0x10B4BA6F && (fseek(f, 0x24BAD0, SEEK_SET) != 0 || fwrite(&(uint32_t){0x10B4BA6F}, 4, 1, f) != 1)) write_ok = false;
    if (write_ok && page != 0x4C && (fseek(f, 0x24B980, SEEK_SET) != 0 || fwrite("L", 1, 1, f) != 1)) write_ok = false;
    for (int i = 0; i < 5 && write_ok; i++) {
        if (current[i] != str_patches[i].to &&
            (fseek(f, str_patches[i].off, SEEK_SET) != 0 || fwrite(&str_patches[i].to, 4, 1, f) != 1)) write_ok = false;
    }
    if (write_ok && fflush(f) != 0) write_ok = false;
    fclose(f);
    if (!write_ok) {
        if (!CopyFileA(backup, exe_path, FALSE))
            scct_log("[PATCH] CRITICAL: executable write failed and rollback failed (err=%lu)", GetLastError());
        else scct_log("[PATCH] Executable write failed; original file restored from backup.");
        return false;
    }

    if (changed) {
        printf("[CONFIG] In-game menu configured: 'COOP -> ONLINE' & 'COOP ONLINE' active.\n");
    }
    return true;
}

static bool locate_game(void)
{
    char self_dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, self_dir, sizeof(self_dir))) {
        char *sl = strrchr(self_dir, '\\');
        if (sl) *sl = 0;
    }

    char try_self1[MAX_PATH] = {0}, try_self2[MAX_PATH] = {0}, try_self3[MAX_PATH] = {0};
    if (self_dir[0]) {
        snprintf(try_self1, sizeof(try_self1), "%s\\splintercell3.exe", self_dir);
        snprintf(try_self2, sizeof(try_self2), "%s\\System\\splintercell3.exe", self_dir);
        snprintf(try_self3, sizeof(try_self3), "%s\\..\\System\\splintercell3.exe", self_dir);
    }

    const char *tries[] = {
        try_self1,
        try_self2,
        try_self3,
        "splintercell3.exe",
        "System\\splintercell3.exe",
        "..\\System\\splintercell3.exe",
        "C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\games\\Splinter Cell Chaos Theory\\System\\splintercell3.exe",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Splinter Cell Chaos Theory\\System\\splintercell3.exe",
        "C:\\Games\\Splinter Cell Chaos Theory\\System\\splintercell3.exe",
        "D:\\Games\\Splinter Cell Chaos Theory\\System\\splintercell3.exe",
        "D:\\SteamLibrary\\steamapps\\common\\Splinter Cell Chaos Theory\\System\\splintercell3.exe"
    };

    for (int i = 0; i < (int)(sizeof(tries) / sizeof(tries[0])); i++) {
        if (tries[i][0] && GetFileAttributesA(tries[i]) != INVALID_FILE_ATTRIBUTES) {
            GetFullPathNameA(tries[i], sizeof(g_game_exe_path), g_game_exe_path, NULL);
            char *slash = strrchr(g_game_exe_path, '\\');
            if (slash) {
                *slash = 0;
                snprintf(g_game_dir, sizeof(g_game_dir), "%s", g_game_exe_path);
                *slash = '\\';
            }
            patch_game_disk(g_game_exe_path);
            char ws_mod[MAX_PATH];
            snprintf(ws_mod, sizeof(ws_mod), "%s\\scripts\\SplinterCellChaosTheory.WidescreenFix.asi", g_game_dir);
            if (GetFileAttributesA(ws_mod) != INVALID_FILE_ATTRIBUTES) {
                printf("[MOD NOTE] ThirteenAG WidescreenFix detected in game directory.\n");
                printf("           If you get disconnected at the start of Panama, ensure both players\n");
                printf("           have the exact same WidescreenFix files, or temporarily disable it.\n");
            }
            return true;
        }
    }
    return false;
}


/* ---------------------------------------------------------------- DIAGNOSTIC LOG & HANG WATCHDOG
 * Everything important (level detection, voice gating, network drops, game hangs)
 * is written to SCCT_OnlineCoop.log next to the launcher (or %TEMP% if that folder is read-only),
 * so a freeze / disconnect can be audited after the fact instead of guessed.
 */
static FILE *g_log_f = NULL;
static CRITICAL_SECTION g_log_cs;
static bool g_log_ready = false;
static char g_log_dir[MAX_PATH] = {0};

static volatile DWORD g_last_game_pkt_tick = 0;

static void scct_log_init(void)
{
    if (g_log_ready) return;
    InitializeCriticalSection(&g_log_cs);
    g_log_ready = true;

    char dir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    char *sl = strrchr(dir, '\\');
    if (sl) *sl = 0;

    char full[MAX_PATH];
    const char *mode = "a";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    snprintf(full, sizeof(full), "%s\\SCCT_OnlineCoop.log", dir);
    if (GetFileAttributesExA(full, GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 2u * 1024u * 1024u) mode = "w";
    g_log_f = fopen(full, mode);
    if (g_log_f) {
        strncpy(g_log_dir, dir, sizeof(g_log_dir) - 1);
    } else {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(sizeof(tmp), tmp);
        size_t tl = strlen(tmp);
        if (tl && tmp[tl - 1] == '\\') tmp[tl - 1] = 0;
        snprintf(full, sizeof(full), "%s\\SCCT_OnlineCoop.log", tmp);
        g_log_f = fopen(full, mode);
        if (g_log_f) strncpy(g_log_dir, tmp, sizeof(g_log_dir) - 1);
    }
    if (g_log_f) {
        fprintf(g_log_f, "\n===== SCCT_OnlineCoop %s =====\n", SCCT_VERSION);
        fflush(g_log_f);
        printf("[LOG] Diagnostics are written to %s\n", full);
    }
}

static void scct_log(const char *fmt, ...)
{
    if (!g_log_ready || !g_log_f) return;
    char msg[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log_f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fflush(g_log_f);
    LeaveCriticalSection(&g_log_cs);
}

static bool ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, nl) == 0) return true;
    }
    return false;
}

typedef BOOL (WINAPI *pfnMiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct HangWndCtx { DWORD pid; HWND hwnd; };

static BOOL CALLBACK hang_find_wnd(HWND hWnd, LPARAM lp)
{
    struct HangWndCtx *c = (struct HangWndCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == c->pid && IsWindowVisible(hWnd) && GetWindow(hWnd, GW_OWNER) == NULL) {
        char cls[64] = {0};
        GetClassNameA(hWnd, cls, sizeof(cls));
        if (_stricmp(cls, "ConsoleWindowClass") != 0) { c->hwnd = hWnd; return FALSE; }
    }
    return TRUE;
}


/* Writes a small minidump (threads + stacks, no heap) of a hung game so the exact instruction pointer
 * and call stack of the frozen thread can be analysed. Size is typically a few MB. */
static void hang_write_dump(DWORD pid)
{
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) { scct_log("[HANG] dbghelp.dll unavailable, no dump written"); return; }
    pfnMiniDumpWriteDump pDump = (pfnMiniDumpWriteDump)GetProcAddress(dbg, "MiniDumpWriteDump");
    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (pDump && hp) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\SCCT_hang_%lu.dmp", g_log_dir[0] ? g_log_dir : ".", pid);
        HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            BOOL okd = pDump(hp, pid, hf, (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo), NULL, NULL, NULL);
            CloseHandle(hf);
            scct_log("[HANG] Minidump %s: %s", okd ? "written" : "FAILED", path);
        } else {
            scct_log("[HANG] Could not create dump file %s (err %lu)", path, GetLastError());
        }
    }
    if (hp) CloseHandle(hp);
    FreeLibrary(dbg);
}

static void game_hang_watchdog(DWORD pid)
{
    static DWORD s_pid = 0, s_hung_since = 0, s_last_check = 0, s_dumped_pid = 0;
    DWORD now = GetTickCount();
    if (pid != s_pid) { s_pid = pid; s_hung_since = 0; s_dumped_pid = 0; }
    if (now - s_last_check < 500) return;
    s_last_check = now;

    struct HangWndCtx c = { pid, NULL };
    EnumWindows(hang_find_wnd, (LPARAM)&c);
    if (!c.hwnd) return;

    if (IsHungAppWindow(c.hwnd)) {
        if (!s_hung_since) {
            s_hung_since = now;
            DWORD last_pkt = (DWORD)InterlockedCompareExchange((volatile LONG *)&g_last_game_pkt_tick, 0, 0);
            if (last_pkt) {
                scct_log("[HANG] Game window (pid %lu) is NOT RESPONDING. last_game_packet_age=%lu ms",
                         pid, now - last_pkt);
            } else {
                scct_log("[HANG] Game window (pid %lu) is NOT RESPONDING. last_game_packet=never",
                         pid);
            }
            if (s_dumped_pid != pid) { s_dumped_pid = pid; hang_write_dump(pid); }
        }
    } else if (s_hung_since) {
        scct_log("[HANG] Game window responding again after %lu ms", now - s_hung_since);
        s_hung_since = 0;
    }
}

static void patch_game_memory(HANDLE hProc);
static void patch_game_voice(HANDLE hProc, DWORD pid);

static void configure_game_borderless_ini(void);
static void enforce_borderless_fullscreen(HWND hWnd);
static void voice_tick_game(void);
static void voice_start(void);
static void voice_stop(void);
static int  voice_pop_outgoing(uint8_t *out_buf, int max_len);
static void voice_push_incoming(const uint8_t *data, int len);

static void patch_game_memory(HANDLE hProc)
{
    DWORD target_jump_entry = 0x10B4BAD0;
    DWORD new_jump_val = 0x10B4BA6F;
    DWORD current_jump = 0;
    SIZE_T r = 0;

    if (ReadProcessMemory(hProc, (LPCVOID)target_jump_entry, &current_jump, 4, &r) && current_jump == 0x10B4BA8C) {
        DWORD old_prot;
        if (VirtualProtectEx(hProc, (LPVOID)target_jump_entry, 4, PAGE_EXECUTE_READWRITE, &old_prot)) {
            SIZE_T written = 0;
            WriteProcessMemory(hProc, (LPVOID)target_jump_entry, &new_jump_val, 4, &written);
            VirtualProtectEx(hProc, (LPVOID)target_jump_entry, 4, old_prot, &old_prot);
            printf("[CONFIG] In-game menu redirect active.\n");
        }
    }

    DWORD target_page_call = 0x10B4B980;
    BYTE new_page_byte = 0x4C;
    BYTE current_page = 0;
    if (ReadProcessMemory(hProc, (LPCVOID)target_page_call, &current_page, 1, &r) && current_page == 0x50) {
        DWORD old_prot;
        if (VirtualProtectEx(hProc, (LPVOID)target_page_call, 1, PAGE_EXECUTE_READWRITE, &old_prot)) {
            SIZE_T written = 0;
            WriteProcessMemory(hProc, (LPVOID)target_page_call, &new_page_byte, 1, &written);
            VirtualProtectEx(hProc, (LPVOID)target_page_call, 1, old_prot, &old_prot);
        }
    }

    /* Live in-memory patch: replace 'MainTitleLan' & 'LanCaption' pushes with 'MainTitleOnline' & 'OnlineCaption'
     * so 'COOP ONLINE' is always displayed on BOTH Host and Guest regardless of install directory or file locks!
     */
    static const struct { DWORD addr; DWORD from; DWORD to; } mem_str_patches[] = {
        { 0x10B2403F, 0x110DF404, 0x110DF3E4 },
        { 0x10B2BDB9, 0x110DF404, 0x110DF3E4 },
        { 0x10B5119D, 0x110DF404, 0x110DF3E4 },
        { 0x10B52289, 0x110DF404, 0x110DF3E4 },
        { 0x10B4BB90, 0x110E2CC8, 0x110E2CAC }
    };
    for (int i = 0; i < 5; i++) {
        DWORD cur = 0;
        if (ReadProcessMemory(hProc, (LPCVOID)(uintptr_t)mem_str_patches[i].addr, &cur, 4, &r) && cur == mem_str_patches[i].from) {
            DWORD old_prot;
            if (VirtualProtectEx(hProc, (LPVOID)(uintptr_t)mem_str_patches[i].addr, 4, PAGE_EXECUTE_READWRITE, &old_prot)) {
                SIZE_T written = 0;
                WriteProcessMemory(hProc, (LPVOID)(uintptr_t)mem_str_patches[i].addr, &mem_str_patches[i].to, 4, &written);
                VirtualProtectEx(hProc, (LPVOID)(uintptr_t)mem_str_patches[i].addr, 4, old_prot, &old_prot);
            }
        }
    }

    /* Native D3D9 Force-Windowed patch: force D3DPRESENT_PARAMETERS.Windowed = TRUE (0x10CA3D08)
     * Vanilla: 33 C9 3B EB 0F 94 C1 (xor ecx, ecx; cmp ebp, ebx; sete cl)
     * Patch:   B9 01 00 00 00 90 90 (mov ecx, 1; nop; nop)
     * This forces the D3D9 swapchain to create in Windowed mode without WidescreenFix or d3d9.ini!
     */
    DWORD target_windowed_patch = 0x10CA3D08;
    BYTE cur_win_bytes[7] = {0};
    if (ReadProcessMemory(hProc, (LPCVOID)(uintptr_t)target_windowed_patch, cur_win_bytes, 7, &r) &&
        memcmp(cur_win_bytes, "\x33\xC9\x3B\xEB\x0F\x94\xC1", 7) == 0) {
        DWORD old_prot;
        if (VirtualProtectEx(hProc, (LPVOID)(uintptr_t)target_windowed_patch, 7, PAGE_EXECUTE_READWRITE, &old_prot)) {
            static const BYTE force_windowed_code[7] = { 0xB9, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90 };
            SIZE_T written = 0;
            WriteProcessMemory(hProc, (LPVOID)(uintptr_t)target_windowed_patch, force_windowed_code, 7, &written);
            VirtualProtectEx(hProc, (LPVOID)(uintptr_t)target_windowed_patch, 7, old_prot, &old_prot);
            printf("[CONFIG] D3D9 Force-Windowed mode patch applied (zero screen blinking).\n");
            scct_log("[WINDOW] Patched 0x10CA3D08 to force D3D9 Windowed = TRUE");
        }
    }
}

/* ---------------------------------------------------------------- CO-OP VOICE CHAT & AI NOISE RESTORATION
 * Restores Co-op Voice Chat on Windows Vista/7/10/11 (where legacy DirectPlay Voice COM
 * servers dpvoice.dll / dpvvox.dll were removed by Microsoft):
 *   1. Enables the in-game Sound Menu options (Voice Chat ON/OFF, Mute Voice Chat, Mic Sensitivity)
 *      and Co-op Lobby headset indicator by patching IsVoiceChatAvailable (0x10A3C940) and
 *      stubbing legacy DirectPlay Voice session setup calls (0x10AE75E0, 0x10AE7AA0, 0x10AE7690).
 *   2. Hooks VoiceController::Tick (0x10AE73A0) on the game's main thread via a small code cave
 *      so speaking into the microphone invokes the game's native VoiceController::OnLocalPlayerSpeak
 *      (0x10AE7720 -> 0x1095E000 -> 0x10964A60), alerting nearby AI guards at the speaking player's
 *      3D position and driving the in-game sound meter ([PlayerController+0x440]).
 *   3. Captures 16 kHz mono audio via WinMM, compresses to 4-bit IMA ADPCM, multiplexes over CH_VOICE,
 *      and applies 3D proximity distance attenuation between the two players' pawns in-game.
 */

#define VOICE_SAMPLE_RATE 16000
#define VOICE_FRAME_SAMPLES 320  /* 20 ms at 16 kHz */
#define VOICE_ADPCM_BYTES   160  /* 320 samples * 4 bits */
#define VOICE_PKT_BYTES     (8 + VOICE_ADPCM_BYTES)
#define VOICE_NUM_BUFS      8
#define VOICE_RING_MAX      32

typedef enum {
    VOICE_MODE_OPEN = 0,       /* Voice Activated / Open Mic */
    VOICE_MODE_PTT_V = 1,      /* Push-to-Talk: Hold V */
    VOICE_MODE_PTT_CAPS = 2,   /* Push-to-Talk: Hold Caps Lock */
    VOICE_MODE_PTT_MOUSE4 = 3, /* Push-to-Talk: Hold Mouse 4 (XBUTTON1) */
    VOICE_MODE_PTT_T = 4       /* Push-to-Talk: Hold T */
} VoiceMode;

static DWORD  g_voice_patched_pid = 0;
static LPVOID g_voice_remote_cave = NULL;
static volatile LONG g_voice_enabled = 1;
static volatile LONG g_voice_muted = 0;
static volatile LONG g_voice_mode = VOICE_MODE_OPEN;
static volatile LONG g_voice_ptt_active = 0;
static volatile LONG g_voice_tx_count = 0;
static volatile LONG g_voice_rx_count = 0;
static volatile LONG g_voice_channel_active = 0; /* 1 only when active in-game co-op session */

static inline bool is_voice_channel_active(void)
{
    return (g_voice_channel_active != 0);
}
static volatile LONG g_voice_sens = 65;       /* 0..100 from byte [cfg+0x2A] */
static volatile LONG g_voice_vol = 90;        /* 0..100 from byte [cfg+0x2B] */
static volatile LONG g_voice_mic_peak = 0;    /* 0..100 latest mic level for AI noise */
static volatile LONG g_voice_ui_level = 0;    /* 0..100 latest mic level for GUI meter */
static volatile LONG g_voice_loopback = 0;    /* 1 = hear own mic in Server Browser */
static volatile LONG g_mic_device_id = (LONG)WAVE_MAPPER; /* Selected waveIn device ID */
static volatile LONG g_mic_reopen_req = 0;    /* 1 = reopen waveIn with new g_mic_device_id */
static char   g_mic_device_name[96] = "Windows Default Microphone";
static volatile LONG g_voice_prox_scale_bits = 0x3F800000; /* atomic float bits, starts at 1.0 */
static volatile LONG g_voice_partner_dist_bits = 0x451C4000; /* atomic float bits, starts at 2500 */
static float s_voice_lpf_prev = 0.0f;              /* LPF previous sample state */

static LONG voice_float_bits(float value)
{
    union { float f; LONG l; } bits;
    bits.f = value;
    return bits.l;
}

static float voice_bits_float(LONG value)
{
    union { float f; LONG l; } bits;
    bits.l = value;
    return bits.f;
}

static CRITICAL_SECTION g_voice_cs;
static bool g_voice_cs_init = false;
static uint8_t g_voice_out_q[VOICE_RING_MAX][VOICE_PKT_BYTES];
static int g_voice_out_head = 0, g_voice_out_tail = 0, g_voice_out_count = 0;
static uint8_t g_voice_in_q[VOICE_RING_MAX][VOICE_PKT_BYTES];
static int g_voice_in_head = 0, g_voice_in_tail = 0, g_voice_in_count = 0;
static volatile LONG g_voice_thread_running = 0;
static HANDLE g_voice_thread_handle = NULL;
/* Serializes worker creation, shutdown, and handle reaping across GUI/relay threads. */
static SRWLOCK g_voice_lifecycle_lock = SRWLOCK_INIT;

static const int g_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int g_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static uint8_t ima_encode_sample(int sample, int *pred, int *idx)
{
    int step = g_ima_step_table[*idx];
    int diff = sample - *pred;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }
    if (code & 8) *pred -= vpdiff;
    else          *pred += vpdiff;
    if (*pred > 32767)  *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += g_ima_index_table[code];
    if (*idx < 0)  *idx = 0;
    if (*idx > 88) *idx = 88;
    return code;
}

static int16_t ima_decode_sample(uint8_t code, int *pred, int *idx)
{
    int step = g_ima_step_table[*idx];
    int vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += (step >> 1);
    if (code & 1) vpdiff += (step >> 2);
    if (code & 8) *pred -= vpdiff;
    else          *pred += vpdiff;
    if (*pred > 32767)  *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += g_ima_index_table[code & 0x0F];
    if (*idx < 0)  *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)(*pred);
}

static void ima_encode_frame(const int16_t *pcm, uint8_t *pkt, int *io_pred, int *io_idx, uint8_t mic_vol)
{
    pkt[0] = 'S'; pkt[1] = 'C'; pkt[2] = 'V'; pkt[3] = 'O';
    *(int16_t *)(pkt + 4) = (int16_t)(*io_pred);
    pkt[6] = (uint8_t)(*io_idx);
    pkt[7] = mic_vol;
    uint8_t *dst = pkt + 8;
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i += 2) {
        uint8_t lo = ima_encode_sample(pcm[i], io_pred, io_idx);
        uint8_t hi = ima_encode_sample(pcm[i + 1], io_pred, io_idx);
        *dst++ = (uint8_t)((lo & 0x0F) | ((hi & 0x0F) << 4));
    }
}

static bool ima_decode_frame(const uint8_t *pkt, int len, int16_t *pcm_out, float gain)
{
    if (len < VOICE_PKT_BYTES) return false;
    if (pkt[0] != 'S' || pkt[1] != 'C' || pkt[2] != 'V' || pkt[3] != 'O') return false;
    int pred = *(const int16_t *)(pkt + 4);
    int idx = pkt[6];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    const uint8_t *src = pkt + 8;
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i += 2) {
        uint8_t b = *src++;
        int s0 = ima_decode_sample(b & 0x0F, &pred, &idx);
        int s1 = ima_decode_sample((b >> 4) & 0x0F, &pred, &idx);
        s0 = (int)(s0 * gain);
        s1 = (int)(s1 * gain);
        if (s0 > 32767) s0 = 32767; else if (s0 < -32768) s0 = -32768;
        if (s1 > 32767) s1 = 32767; else if (s1 < -32768) s1 = -32768;
        pcm_out[i]     = (int16_t)s0;
        pcm_out[i + 1] = (int16_t)s1;
    }
    return true;
}

/* Dynamic distance single-pole IIR low-pass filter:
 * y[n] = y[n-1] + alpha * (x[n] - y[n-1])
 * At dist <= 350.0: alpha = 1.0 (crystal clear, direct speech)
 * At dist >= 2500.0: alpha = 0.18 (cutoff ~500 Hz: muffled tactical radio / distance rolloff)
 */
static void apply_voice_lowpass(int16_t *samples, int count, float dist)
{
    if (dist <= 350.0f) {
        if (count > 0) s_voice_lpf_prev = (float)samples[count - 1];
        return;
    }

    float t = (dist - 350.0f) / 2150.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    float alpha = 1.0f - 0.82f * t; /* Smooth transition from 1.0 down to 0.18 */

    float y = s_voice_lpf_prev;
    for (int i = 0; i < count; i++) {
        y += alpha * ((float)samples[i] - y);
        int out_val = (int)(y + 0.5f);
        if (out_val > 32767) out_val = 32767;
        else if (out_val < -32768) out_val = -32768;
        samples[i] = (int16_t)out_val;
    }
    s_voice_lpf_prev = y;
}

static bool write_proc_bytes(HANDLE hProc, DWORD addr, const void *data, SIZE_T len)
{
    DWORD old_prot = 0;
    if (!VirtualProtectEx(hProc, (LPVOID)(uintptr_t)addr, len, PAGE_EXECUTE_READWRITE, &old_prot)) return false;
    SIZE_T w = 0;
    bool ok = WriteProcessMemory(hProc, (LPVOID)(uintptr_t)addr, data, len, &w) && (w == len);
    VirtualProtectEx(hProc, (LPVOID)(uintptr_t)addr, len, old_prot, &old_prot);
    return ok;
}

static void patch_game_voice(HANDLE hProc, DWORD pid)
{
    /* Check if 0x10AE7320 (VoiceController::Stop) is already patched in this process */
    uint8_t check[8] = {0};
    SIZE_T r = 0;
    if (!ReadProcessMemory(hProc, (LPCVOID)0x10AE7320, check, sizeof(check), &r)) return;

    if (check[0] == 0x85 && check[1] == 0xC9 && g_voice_patched_pid == pid && g_voice_remote_cave != NULL) {
        return;
    }

    /* Allocate dedicated executable code cave in game process:
     *   cave + 0x00: uint32_t magic ('VOIC' = 0x564F4943)
     *   cave + 0x04: uint32_t mic_volume (0..100, written by launcher, atomically consumed by game thread)
     *   cave + 0x80: VoiceController::Tick (0x10AE73A0) hook machine code
     */
    LPVOID cave = g_voice_remote_cave;
    if (!cave || g_voice_patched_pid != pid) {
        cave = VirtualAllocEx(hProc, NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave) return;
    }
    uint32_t cave_addr = (uint32_t)(uintptr_t)cave;
    uint32_t mic_vol_addr = cave_addr + 0x04;
    uint32_t code_addr = cave_addr + 0x80;

    uint8_t cave_buf[512];
    memset(cave_buf, 0, sizeof(cave_buf));
    *(uint32_t *)(cave_buf + 0x00) = 0x564F4943; /* 'VOIC' */
    *(uint32_t *)(cave_buf + 0x04) = 0;

    /* Assemble game-thread hook at cave + 0x80:
     *   Safely initializes ds:[0x11223794] (VoiceController*) if NULL, verifies Engine -> Level -> PlayerList chain,
     *   and invokes VoiceController::OnLocalPlayerSpeak (0x10AE7720) with current mic_volume for AI guard detection.
     *   DOES NOT WRITE TO [pawn + 0x420] (which is player Health)!
     */
    uint8_t *p = cave_buf + 0x80;
    *p++ = 0x53;                                     /* push ebx */
    *p++ = 0x56;                                     /* push esi */
    *p++ = 0x8B; *p++ = 0xF1;                        /* mov esi, ecx */
    *p++ = 0x85; *p++ = 0xF6;                        /* test esi, esi */
    uint8_t *je_done1 = p; *p++ = 0x74; *p++ = 0x00; /* je done */

    /* Ensure GEngine pointer at [esi + 0x18] */
    *p++ = 0x83; *p++ = 0x7E; *p++ = 0x18; *p++ = 0x00; /* cmp dword ptr [esi+18h], 0 */
    *p++ = 0x75; *p++ = 0x08;                        /* jne +8 */
    *p++ = 0xA1; *(uint32_t *)p = 0x111F5554; p += 4;/* mov eax, ds:[0x111F5554] */
    *p++ = 0x89; *p++ = 0x46; *p++ = 0x18;           /* mov [esi+18h], eax */

    /* Cooldown check: if [esi+0x0C] > 0, decrement and exit without consuming mic_volume */
    *p++ = 0x8B; *p++ = 0x46; *p++ = 0x0C;           /* mov eax, [esi+0Ch] */
    *p++ = 0x85; *p++ = 0xC0;                        /* test eax, eax */
    *p++ = 0x76; *p++ = 0x08;                        /* jbe +8 (to cd_zero) */
    *p++ = 0x48;                                     /* dec eax */
    *p++ = 0x89; *p++ = 0x46; *p++ = 0x0C;           /* mov [esi+0Ch], eax */
    uint8_t *jmp_done_cd = p; *p++ = 0xEB; *p++ = 0x00; /* jmp done */

    /* cd_zero: atomically consume mic_volume */
    *p++ = 0x33; *p++ = 0xC0;                        /* xor eax, eax */
    *p++ = 0x87; *p++ = 0x05;                        /* xchg [mic_vol_addr], eax */
    *(uint32_t *)p = mic_vol_addr; p += 4;
    *p++ = 0x85; *p++ = 0xC0;                        /* test eax, eax */
    uint8_t *jbe_done2 = p; *p++ = 0x76; *p++ = 0x00;/* jbe done */
    /* Verify Engine -> [+0x44] -> [+0x2C] -> [0] -> [+0x64] (playerList) has >= 1 active player */
    *p++ = 0x8B; *p++ = 0x5E; *p++ = 0x18;           /* mov ebx, [esi+18h] */
    *p++ = 0x85; *p++ = 0xDB;                        /* test ebx, ebx */
    uint8_t *je_done3 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    *p++ = 0x8B; *p++ = 0x5B; *p++ = 0x44;           /* mov ebx, [ebx+44h] */
    *p++ = 0x85; *p++ = 0xDB;                        /* test ebx, ebx */
    uint8_t *je_done4 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    *p++ = 0x8B; *p++ = 0x5B; *p++ = 0x2C;           /* mov ebx, [ebx+2Ch] */
    *p++ = 0x85; *p++ = 0xDB;                        /* test ebx, ebx */
    uint8_t *je_done5 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    *p++ = 0x8B; *p++ = 0x1B;                        /* mov ebx, [ebx] */
    *p++ = 0x85; *p++ = 0xDB;                        /* test ebx, ebx */
    uint8_t *je_done6 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    *p++ = 0x8B; *p++ = 0x5B; *p++ = 0x64;           /* mov ebx, [ebx+64h] */
    *p++ = 0x85; *p++ = 0xDB;                        /* test ebx, ebx */
    uint8_t *je_done7 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    *p++ = 0x83; *p++ = 0x7B; *p++ = 0x04; *p++ = 0x00; /* cmp dword ptr [ebx+4], 0 */
    uint8_t *jle_done8 = p; *p++ = 0x7E; *p++ = 0x00;/* jle done */
    *p++ = 0x83; *p++ = 0x3B; *p++ = 0x00;           /* cmp dword ptr [ebx], 0 */
    uint8_t *je_done9 = p; *p++ = 0x74; *p++ = 0x00; /* je done */
    /* Call 0x10AE7720: __thiscall VoiceController::OnLocalPlayerSpeak(dwVolume, a2)
     *   On entry: ecx = this (VoiceController), [esp+4] = dwVolume (arg1), [esp+8] = 0 (arg2)
     */
    *p++ = 0x6A; *p++ = 0x00;                        /* push 0 (arg2) */
    *p++ = 0x50;                                     /* push eax (arg1 = dwVolume) */
    *p++ = 0x8B; *p++ = 0xCE;                        /* mov ecx, esi */
    *p++ = 0xE8;                                     /* call 0x10AE7720 */
    uint32_t c_call = code_addr + (uint32_t)(p + 4 - (cave_buf + 0x80));
    *(int32_t *)p = (int32_t)(0x10AE7720 - c_call); p += 4;
    /* done: */
    uint8_t *done_lbl = p;
    je_done1[1]    = (uint8_t)(done_lbl - (je_done1 + 2));
    jmp_done_cd[1] = (uint8_t)(done_lbl - (jmp_done_cd + 2));
    jbe_done2[1]   = (uint8_t)(done_lbl - (jbe_done2 + 2));
    je_done3[1]    = (uint8_t)(done_lbl - (je_done3 + 2));
    je_done4[1]    = (uint8_t)(done_lbl - (je_done4 + 2));
    je_done5[1]    = (uint8_t)(done_lbl - (je_done5 + 2));
    je_done6[1]    = (uint8_t)(done_lbl - (je_done6 + 2));
    je_done7[1]    = (uint8_t)(done_lbl - (je_done7 + 2));
    jle_done8[1]   = (uint8_t)(done_lbl - (jle_done8 + 2));
    je_done9[1]    = (uint8_t)(done_lbl - (je_done9 + 2));
    *p++ = 0x5E;                                     /* pop esi */
    *p++ = 0x5B;                                     /* pop ebx */
    *p++ = 0xC3;                                     /* ret */

    SIZE_T w = 0;
    if (!WriteProcessMemory(hProc, cave, cave_buf, sizeof(cave_buf), &w)) return;

    /* 1. Patch 0x10AE76F0 (TestAudioSetup): xor eax, eax (S_OK = 0); ret 4
     *    Makes both 0x10A3C8C0 (Engine::InitVoice, checks >= 0) and 0x10A3C940 (IsVoiceChatAvailable, checks == 0) succeed!
     *    Also patch 0x10A3C940 directly to mov eax, 1; ret 4.
     */
    static const uint8_t patch_s_ok04[6] = { 0x33, 0xC0, 0xC2, 0x04, 0x00, 0x90 };
    static const uint8_t patch_avail[8]  = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00 };
    write_proc_bytes(hProc, 0x10AE76F0, patch_s_ok04, sizeof(patch_s_ok04));
    write_proc_bytes(hProc, 0x10A3C940, patch_avail, sizeof(patch_avail));

    /* 2. Patch 0x10AE7320 (VoiceController::Stop) so lobby/mission transitions (e.g. launching Panama at 0x10B4E0B8)
     *    never dereference the uninitialized DirectPlay PCVoiceManager pointer at ds:[0x1122F8A8]:
     *    test ecx, ecx; je +4; mov byte ptr [ecx+4], 0; ret
     */
    static const uint8_t patch_stop[9] = { 0x85, 0xC9, 0x74, 0x04, 0xC6, 0x41, 0x04, 0x00, 0xC3 };
    write_proc_bytes(hProc, 0x10AE7320, patch_stop, sizeof(patch_stop));

    /* 3. Patch DirectPlay Voice session & vtable methods so player join/leave succeed:
     *    0x10AE75E0: xor eax, eax; ret 0Ch  (StartClientVoiceSession -> S_OK)
     *    0x10AE7AA0: xor eax, eax; ret 8    (StartHostVoiceSession   -> S_OK)
     *    0x10AE7680: xor eax, eax; ret 8    (VoiceController vtable+0x08)
     *    0x10AE76B0: mov byte ptr [ecx+4], 1; xor eax, eax; ret (VoiceController vtable+0x14)
     *
     *    NOTE: We DO NOT patch 0x10A3C7F0, 0x10A3C8C0, or 0x10A3EA70!
     *    0x10A3C7F0 registers the audio listeners and initializes all AI guard sound sensors for the mission!
     *    0x10A3C8C0 cleanly constructs VoiceController and links it to GEngine.
     *    0x10A3EA70 activates VoiceDetection in the engine without touching legacy DPlay.
     */
    static const uint8_t patch_s_ok0C[6] = { 0x33, 0xC0, 0xC2, 0x0C, 0x00, 0x90 };
    static const uint8_t patch_s_ok08[6] = { 0x33, 0xC0, 0xC2, 0x08, 0x00, 0x90 };
    static const uint8_t patch_v14[7]    = { 0xC6, 0x41, 0x04, 0x01, 0x33, 0xC0, 0xC3 };
    write_proc_bytes(hProc, 0x10AE75E0, patch_s_ok0C, sizeof(patch_s_ok0C));
    write_proc_bytes(hProc, 0x10AE7AA0, patch_s_ok08, sizeof(patch_s_ok08));
    write_proc_bytes(hProc, 0x10AE7680, patch_s_ok08, sizeof(patch_s_ok08));
    write_proc_bytes(hProc, 0x10AE76B0, patch_v14, sizeof(patch_v14));

    /* 4. Ensure AI noise threshold & cooldown globals are initialized */
    uint32_t cur_thresh = 0;
    if (ReadProcessMemory(hProc, (LPCVOID)0x11223780, &cur_thresh, 4, &r) && cur_thresh == 0) {
        uint32_t v_thresh = 20, v_cd = 2, v_5 = 5, v_1000 = 1000;
        float v_mult = 1.0f;
        WriteProcessMemory(hProc, (LPVOID)0x11223780, &v_thresh, 4, &w);
        WriteProcessMemory(hProc, (LPVOID)0x11223784, &v_cd, 4, &w);
        WriteProcessMemory(hProc, (LPVOID)0x11223788, &v_5, 4, &w);
        WriteProcessMemory(hProc, (LPVOID)0x1122378C, &v_mult, 4, &w);
        WriteProcessMemory(hProc, (LPVOID)0x11223790, &v_1000, 4, &w);
    }

    /* 8. Hook 0x10AE73A0 (VoiceController::Tick): jmp code_addr */
    uint8_t jmp_hook[6];
    jmp_hook[0] = 0xE9;
    *(int32_t *)(jmp_hook + 1) = (int32_t)(code_addr - (0x10AE73A0 + 5));
    jmp_hook[5] = 0x90;
    write_proc_bytes(hProc, 0x10AE73A0, jmp_hook, sizeof(jmp_hook));

    /* 10. Default Voice Chat to ON (bit 0 of [cfg+0x30]) on attach so user doesn't start with it forced off */
    uint32_t cfg_ptr = 0;
    if (ReadProcessMemory(hProc, (LPCVOID)0x111F9F64, &cfg_ptr, 4, &r) && cfg_ptr != 0) {
        uint32_t flags = 0;
        if (ReadProcessMemory(hProc, (LPCVOID)(uintptr_t)(cfg_ptr + 0x30), &flags, 4, &r)) {
            flags |= 1; /* Bit 0 = VoiceChatEnable ON */
            WriteProcessMemory(hProc, (LPVOID)(uintptr_t)(cfg_ptr + 0x30), &flags, 4, &w);
        }
    }

    g_voice_remote_cave = cave;
    g_voice_patched_pid = pid;
    printf("[VOICE] Restored Co-op Voice Chat, Proximity Audio & AI Noise Detection (PID %lu)!\n", pid);
}


static u64 g_host_sid = 0;

/* ---------------------------------------------------------------- Network control and game-state helpers */

static volatile SOCKET g_direct_ext_sock = INVALID_SOCKET;
static struct sockaddr_in g_direct_remote_host = {0};

static void voice_tick_game(void)
{
    enforce_borderless_fullscreen(NULL);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "splintercell3.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    if (!pid) return;

    HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) return;

    patch_game_memory(h);
    patch_game_voice(h, pid);
    game_hang_watchdog(pid);

    SIZE_T r = 0, w = 0;
    /* 0. Detect active co-op mission: inspect Engine -> Level -> PlayerList -> Local Pawn.
     *    In Main Menu, Coop Menu, Server Browser, or Lobby: pawn0 == NULL (in_mission = false).
     *    When mission map loads and player spawns in 3D level: pawn0 != NULL and health > 0 (in_mission = true).
     */
    uint32_t eng = 0, p44 = 0, p2c = 0, p0 = 0, plist = 0, parr = 0, pcnt = 0;
    uint32_t e0 = 0, e1 = 0, c0 = 0, c1 = 0, pawn0 = 0, pawn1 = 0;
    float pos0[3] = {0}, pos1[3] = {0};
    bool in_mission = false;
    float prox = 0.20f; /* Unknown partner position is quiet, never full volume. */
    /* Never retain a stale partner position while the player is in menus or loading. */
    InterlockedExchange(&g_voice_partner_dist_bits, voice_float_bits(2500.0f));

    if (ReadProcessMemory(h, (LPCVOID)0x111F5554, &eng, 4, &r) && eng &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(eng + 0x44), &p44, 4, &r) && p44 &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(p44 + 0x2C), &p2c, 4, &r) && p2c &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)p2c, &p0, 4, &r) && p0 &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(p0 + 0x64), &plist, 4, &r) && plist &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)plist, &parr, 4, &r) && parr &&
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(plist + 0x04), &pcnt, 4, &r) && pcnt >= 1) {

        if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)parr, &e0, 4, &r) && e0 &&
            ReadProcessMemory(h, (LPCVOID)(uintptr_t)(e0 + 4), &c0, 4, &r) && c0) {
            ReadProcessMemory(h, (LPCVOID)(uintptr_t)(c0 + 0x2B4), &pawn0, 4, &r);
            if (pawn0 != 0) {
                int32_t health = 0;
                ReadProcessMemory(h, (LPCVOID)(uintptr_t)(pawn0 + 0x420), &health, 4, &r);
                if (health > 0) {
                    in_mission = true;
                    ReadProcessMemory(h, (LPCVOID)(uintptr_t)(pawn0 + 0xE8), pos0, 12, &r);
                }
            }
        }

        if (pcnt >= 2 && in_mission) {
            if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(parr + 4), &e1, 4, &r) && e1 &&
                ReadProcessMemory(h, (LPCVOID)(uintptr_t)(e1 + 4), &c1, 4, &r) && c1) {
                ReadProcessMemory(h, (LPCVOID)(uintptr_t)(c1 + 0x2B4), &pawn1, 4, &r);
                if (pawn1 != 0 && ReadProcessMemory(h, (LPCVOID)(uintptr_t)(pawn1 + 0xE8), pos1, 12, &r)) {
                    float dx = pos0[0] - pos1[0];
                    float dy = pos0[1] - pos1[1];
                    float dz = pos0[2] - pos1[2];
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    InterlockedExchange(&g_voice_partner_dist_bits, voice_float_bits(dist));
                    if (dist <= 300.0f) prox = 1.0f;
                    else if (dist >= 2500.0f) prox = 0.20f;
                    else prox = 1.0f - 0.80f * ((dist - 300.0f) / 2200.0f);
                }
            }
        }
    }
    InterlockedExchange(&g_voice_prox_scale_bits, voice_float_bits(prox));

    /* Read the active level name for voice scene gating and diagnostics. */
    char cur_map[64] = {0};
    uint32_t glevel = 0;
    uint32_t gnames = 0;
    ReadProcessMemory(h, (LPCVOID)0x1130DFC0, &gnames, 4, &r);

    if (eng != 0 && gnames != 0) {
        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(eng + 0x17C), &glevel, 4, &r);
        if (glevel != 0) {
            /* 1. Check GLevel->Outer (UPackage) name */
            uint32_t outer = 0;
            if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(glevel + 0x18), &outer, 4, &r) && outer != 0) {
                uint32_t name_idx = 0;
                if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(outer + 0x20), &name_idx, 4, &r)) {
                    uint32_t entry = 0;
                    if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(gnames + name_idx * 4), &entry, 4, &r) && entry != 0) {
                        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(entry + 0x0C), cur_map, sizeof(cur_map) - 1, &r);
                    }
                }
            }
            /* 2. Fallback: check GLevel name itself */
            if (!cur_map[0]) {
                uint32_t name_idx = 0;
                if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(glevel + 0x20), &name_idx, 4, &r)) {
                    uint32_t entry = 0;
                    if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(gnames + name_idx * 4), &entry, 4, &r) && entry != 0) {
                        ReadProcessMemory(h, (LPCVOID)(uintptr_t)(entry + 0x0C), cur_map, sizeof(cur_map) - 1, &r);
                    }
                }
            }
            /* 3. Fallback: check GLevel->Actors[0] (ALevelInfo) outer */
            if (!cur_map[0]) {
                uint32_t actors = 0;
                if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(glevel + 0x28), &actors, 4, &r) && actors != 0) {
                    uint32_t a0 = 0;
                    if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(actors), &a0, 4, &r) && a0 != 0) {
                        uint32_t a_outer = 0;
                        if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(a0 + 0x18), &a_outer, 4, &r) && a_outer != 0) {
                            uint32_t a_idx = 0;
                            if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(a_outer + 0x20), &a_idx, 4, &r)) {
                                uint32_t entry = 0;
                                if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(gnames + a_idx * 4), &entry, 4, &r) && entry != 0) {
                                    ReadProcessMemory(h, (LPCVOID)(uintptr_t)(entry + 0x0C), cur_map, sizeof(cur_map) - 1, &r);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    bool is_menu = (ci_contains(cur_map, "menu") || ci_contains(cur_map, "Index") || ci_contains(cur_map, "Entry") || (eng != 0 && glevel == 0) || !cur_map[0]);
    bool in_game_level = in_mission && !is_menu && (cur_map[0] != '\0');
    InterlockedExchange(&g_voice_channel_active, in_game_level ? 1 : 0);

    static char s_last_map_log[64] = "";
    if (strcmp(s_last_map_log, cur_map) != 0) {
        strncpy(s_last_map_log, cur_map, sizeof(s_last_map_log) - 1);
        scct_log("[MAP] Current map: '%s' (in_game_level=%d in_mission=%d)", cur_map[0] ? cur_map : "<empty>", in_game_level, in_mission);
    }

    /* 1. Synchronize Sound Options from ds:[0x111F9F64] */
    uint32_t cfg_ptr = 0;
    if (ReadProcessMemory(h, (LPCVOID)0x111F9F64, &cfg_ptr, 4, &r) && cfg_ptr != 0) {
        uint8_t vols[4] = { 65, 90, 90, 90 };
        uint32_t flags = 1;
        if (ReadProcessMemory(h, (LPCVOID)(uintptr_t)(cfg_ptr + 0x2A), vols, 4, &r) &&
            ReadProcessMemory(h, (LPCVOID)(uintptr_t)(cfg_ptr + 0x30), &flags, 4, &r)) {
            /* If game profile loaded with Voice Chat disabled (bit 0 == 0), force it ON in memory */
            if (!(flags & 1)) {
                flags |= 1;
                WriteProcessMemory(h, (LPVOID)(uintptr_t)(cfg_ptr + 0x30), &flags, 4, &w);
            }
            int enabled = 1;
            int muted   = (flags & 2) ? 1 : 0;
            int sens    = vols[0];
            int vvol    = vols[1];
            if (sens <= 0) sens = 65;
            if (vvol <= 0) vvol = 90;
            InterlockedExchange(&g_voice_enabled, enabled);
            InterlockedExchange(&g_voice_muted, muted);
            InterlockedExchange(&g_voice_sens, sens);
            InterlockedExchange(&g_voice_vol, vvol);

            /* Synchronize game's internal AI voice detection globals (exact formula from 0x10AE73E0):
             *   ds:[0x1122377C] = m_bVoiceDetectionEnabled
             *   ds:[0x1122378C] = sensitivityMultiplier
             */
            uint32_t det_en = (uint32_t)enabled;
            float s_norm = (float)sens * 0.01f;
            float mult = (s_norm >= 0.5f) ? (s_norm * 1.8181819f * 0.6f) : (s_norm * 0.35f + 0.4f);
            WriteProcessMemory(h, (LPVOID)0x1122377C, &det_en, 4, &w);
            WriteProcessMemory(h, (LPVOID)0x1122378C, &mult, 4, &w);
        }
    }

    /* 2. Feed microphone peak level into in-game sound meter ([c0 + 0x440] & [c0 + 0x43c]) and AI noise
     * Conversational Tolerance Curve:
     * - peak <= 18: background breathing / room noise -> meter = 0, AI noise = 0
     * - 18 < peak <= 35: quiet/whisper conversation -> meter = 6.0..14.0 (green safe zone), AI noise = 0
     * - 35 < peak <= 52: normal speaking voice -> meter = 14.0..24.0 (gentle green/yellow), AI noise = 80..180 (0.8m..1.8m)
     * - 52 < peak <= 75: raised voice / loud callouts -> meter = 24.0..60.0 (yellow into orange), AI noise = 180..600 (1.8m..6.0m)
     * - 75 < peak <= 100: shouting / screaming -> meter = 60.0..110.0 (red spike), AI noise = 600..1200 (6.0m..12.0m)
     * This eliminates the need to constantly whisper, allowing comfortable natural speech in co-op!
     */
    static float g_voice_snd_meter = 0.0f;
    LONG peak = InterlockedExchange(&g_voice_mic_peak, 0);
    float target_snd = 0.0f;
    uint32_t ai_noise_radius = 0;

    if (peak > 18) {
        if (peak <= 35) {
            float t = (float)(peak - 18) / 17.0f;
            target_snd = 6.0f + 8.0f * t;
            ai_noise_radius = 0;
        } else if (peak <= 52) {
            float t = (float)(peak - 35) / 17.0f;
            target_snd = 14.0f + 10.0f * t;
            ai_noise_radius = (uint32_t)(80.0f + 100.0f * t);
        } else if (peak <= 75) {
            float t = (float)(peak - 52) / 23.0f;
            target_snd = 24.0f + 36.0f * t;
            ai_noise_radius = (uint32_t)(180.0f + 420.0f * t);
        } else {
            float t = (float)(peak - 75) / 25.0f;
            if (t > 1.0f) t = 1.0f;
            target_snd = 60.0f + 50.0f * t;
            ai_noise_radius = (uint32_t)(600.0f + 600.0f * t);
        }
    }

    if (in_mission && g_voice_enabled && !g_voice_muted && c0) {
        if (target_snd > g_voice_snd_meter) {
            g_voice_snd_meter = target_snd;
        } else {
            g_voice_snd_meter = (g_voice_snd_meter > 6.0f) ? (g_voice_snd_meter - 6.0f) : 0.0f;
        }

        if (g_voice_snd_meter > 0.0f) {
            float cur_440 = 0.0f;
            ReadProcessMemory(h, (LPCVOID)(uintptr_t)(c0 + 0x440), &cur_440, 4, &r);
            if (g_voice_snd_meter > cur_440) {
                WriteProcessMemory(h, (LPVOID)(uintptr_t)(c0 + 0x440), &g_voice_snd_meter, 4, &w);
            }
        }
    } else {
        g_voice_snd_meter = 0.0f;
    }

    if (ai_noise_radius > 0 && g_voice_remote_cave && g_voice_patched_pid == pid && g_voice_enabled && in_mission) {
        WriteProcessMemory(h, (LPVOID)((uintptr_t)g_voice_remote_cave + 0x04), &ai_noise_radius, 4, &w);
    }

    CloseHandle(h);
}

static int voice_pop_outgoing(uint8_t *out_buf, int max_len)
{
    if (!g_voice_cs_init || max_len < VOICE_PKT_BYTES) return 0;
    int ret = 0;
    EnterCriticalSection(&g_voice_cs);
    if (g_voice_out_count > 0) {
        memcpy(out_buf, g_voice_out_q[g_voice_out_head], VOICE_PKT_BYTES);
        g_voice_out_head = (g_voice_out_head + 1) % VOICE_RING_MAX;
        g_voice_out_count--;
        ret = VOICE_PKT_BYTES;
    }
    LeaveCriticalSection(&g_voice_cs);
    return ret;
}

static void voice_push_incoming(const uint8_t *data, int len)
{
    if (!g_voice_cs_init || len < VOICE_PKT_BYTES) return;
    if (data[0] != 'S' || data[1] != 'C' || data[2] != 'V' || data[3] != 'O') return;
    if (!is_voice_channel_active()) return; /* Drop voice while in menus / server browser */
    EnterCriticalSection(&g_voice_cs);
    if (g_voice_in_count >= VOICE_RING_MAX) {
        g_voice_in_head = (g_voice_in_head + 1) % VOICE_RING_MAX;
        g_voice_in_count--;
    }
    memcpy(g_voice_in_q[g_voice_in_tail], data, VOICE_PKT_BYTES);
    g_voice_in_tail = (g_voice_in_tail + 1) % VOICE_RING_MAX;
    g_voice_in_count++;
    LeaveCriticalSection(&g_voice_cs);
    InterlockedIncrement(&g_voice_rx_count);
}

static void mic_set_device(UINT dev_id)
{
    InterlockedExchange(&g_mic_device_id, (LONG)dev_id);
    if (dev_id == WAVE_MAPPER) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(WAVE_MAPPER, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            snprintf(g_mic_device_name, sizeof(g_mic_device_name), "Default (%s)", caps.szPname);
        else
            snprintf(g_mic_device_name, sizeof(g_mic_device_name), "Windows Default Microphone");
    } else {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(dev_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            snprintf(g_mic_device_name, sizeof(g_mic_device_name), "%s", caps.szPname);
        else
            snprintf(g_mic_device_name, sizeof(g_mic_device_name), "Microphone #%u", dev_id);
    }
    InterlockedExchange(&g_mic_reopen_req, 1);
}

static DWORD WINAPI voice_thread(LPVOID unused)
{
    (void)unused;
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = VOICE_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = VOICE_SAMPLE_RATE * 2;

    HANDLE hEventIn = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEIN hWaveIn = NULL;
    WAVEHDR inHdrs[VOICE_NUM_BUFS];
    int16_t inBufs[VOICE_NUM_BUFS][VOICE_FRAME_SAMPLES];
    bool has_mic = false;

    UINT open_dev = (UINT)g_mic_device_id;
    InterlockedExchange(&g_mic_reopen_req, 0);
    if (waveInOpen(&hWaveIn, open_dev, &wfx, (DWORD_PTR)hEventIn, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        has_mic = true;
        for (int i = 0; i < VOICE_NUM_BUFS; i++) {
            memset(&inHdrs[i], 0, sizeof(WAVEHDR));
            inHdrs[i].lpData = (LPSTR)inBufs[i];
            inHdrs[i].dwBufferLength = VOICE_FRAME_SAMPLES * sizeof(int16_t);
            waveInPrepareHeader(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
            waveInAddBuffer(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
        }
        waveInStart(hWaveIn);
    }

    HWAVEOUT hWaveOut = NULL;
    WAVEHDR outHdrs[VOICE_NUM_BUFS];
    int16_t outBufs[VOICE_NUM_BUFS][VOICE_FRAME_SAMPLES];
    bool has_spk = false;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
        has_spk = true;
        for (int i = 0; i < VOICE_NUM_BUFS; i++) {
            memset(&outHdrs[i], 0, sizeof(WAVEHDR));
            outHdrs[i].lpData = (LPSTR)outBufs[i];
            outHdrs[i].dwBufferLength = VOICE_FRAME_SAMPLES * sizeof(int16_t);
            waveOutPrepareHeader(hWaveOut, &outHdrs[i], sizeof(WAVEHDR));
            outHdrs[i].dwFlags |= WHDR_DONE;
        }
    }

    int enc_pred = 0, enc_idx = 0;
    int hang_frames = 0;
    int out_idx = 0;

    while (g_running && InterlockedCompareExchange(&g_voice_thread_running, 0, 0)) {
        /* Push-to-Talk key detection */
        bool ptt_pressed = false;
        int vmode = (int)g_voice_mode;
        if (vmode == VOICE_MODE_OPEN) {
            ptt_pressed = true;
        } else if (vmode == VOICE_MODE_PTT_V) {
            ptt_pressed = (GetAsyncKeyState('V') & 0x8000) != 0;
        } else if (vmode == VOICE_MODE_PTT_CAPS) {
            ptt_pressed = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
        } else if (vmode == VOICE_MODE_PTT_MOUSE4) {
            ptt_pressed = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
        } else if (vmode == VOICE_MODE_PTT_T) {
            ptt_pressed = (GetAsyncKeyState('T') & 0x8000) != 0;
        }
        InterlockedExchange(&g_voice_ptt_active, ptt_pressed ? 1 : 0);

        if (InterlockedExchange(&g_mic_reopen_req, 0) != 0) {
            if (has_mic) {
                waveInStop(hWaveIn);
                waveInReset(hWaveIn);
                for (int i = 0; i < VOICE_NUM_BUFS; i++) waveInUnprepareHeader(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
                waveInClose(hWaveIn);
                hWaveIn = NULL;
                has_mic = false;
            }
            InterlockedExchange(&g_voice_ui_level, 0);
            open_dev = (UINT)g_mic_device_id;
            if (waveInOpen(&hWaveIn, open_dev, &wfx, (DWORD_PTR)hEventIn, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
                has_mic = true;
                for (int i = 0; i < VOICE_NUM_BUFS; i++) {
                    memset(&inHdrs[i], 0, sizeof(WAVEHDR));
                    inHdrs[i].lpData = (LPSTR)inBufs[i];
                    inHdrs[i].dwBufferLength = VOICE_FRAME_SAMPLES * sizeof(int16_t);
                    waveInPrepareHeader(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
                    waveInAddBuffer(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
                }
                waveInStart(hWaveIn);
                printf("[MIC] Active microphone switched to: %s\n", g_mic_device_name);
            } else {
                printf("[MIC] Could not open microphone device %u (%s).\n", open_dev, g_mic_device_name);
            }
        }

        if (has_mic) {
            WaitForSingleObject(hEventIn, 15);
            for (int i = 0; i < VOICE_NUM_BUFS; i++) {
                if (inHdrs[i].dwFlags & WHDR_DONE) {
                    const int16_t *pcm = inBufs[i];
                    int max_abs = 0;
                    double sum_sq = 0.0;
                    for (int s = 0; s < VOICE_FRAME_SAMPLES; s++) {
                        int v = pcm[s];
                        int a = (v < 0) ? -v : v;
                        if (a > max_abs) max_abs = a;
                        sum_sq += (double)v * (double)v;
                    }
                    double rms = sqrt(sum_sq / VOICE_FRAME_SAMPLES);
                    int sens = (int)g_voice_sens; /* 0..100 */
                    /* Scale microphone level (0..100) using rms/peak and sensitivity slider */
                    double sens_gain = 0.4 + ((double)sens / 100.0) * 1.6;
                    int mic_vol = (int)((rms / 5000.0) * 100.0 * sens_gain);
                    int peak_vol = (int)((max_abs / 12000.0) * 100.0 * sens_gain);
                    if (peak_vol > mic_vol) mic_vol = (mic_vol + peak_vol) / 2;
                    if (mic_vol > 100) mic_vol = 100;

                    /* Update GUI live meter with smooth decay */
                    LONG prev_ui = g_voice_ui_level;
                    LONG next_ui = (mic_vol >= prev_ui) ? (LONG)mic_vol : (prev_ui > 4 ? prev_ui - 4 : 0);
                    InterlockedExchange(&g_voice_ui_level, next_ui);

                    if (g_voice_enabled) {
                        bool should_transmit = false;
                        if (vmode == VOICE_MODE_OPEN) {
                            int vox_thresh = 15 - (sens / 8);
                            if (vox_thresh < 4) vox_thresh = 4;
                            if (mic_vol >= vox_thresh) {
                                hang_frames = 8; /* 160ms hangover */
                                LONG prev = g_voice_mic_peak;
                                if (mic_vol > prev) InterlockedExchange(&g_voice_mic_peak, (LONG)mic_vol);
                            }
                            if (hang_frames > 0) {
                                hang_frames--;
                                should_transmit = true;
                            }
                        } else {
                            /* Push-to-Talk */
                            if (ptt_pressed) {
                                hang_frames = 4; /* 80ms hangover to prevent syllable clipping */
                                LONG prev = g_voice_mic_peak;
                                if (mic_vol > prev) InterlockedExchange(&g_voice_mic_peak, (LONG)mic_vol);
                            }
                            if (hang_frames > 0) {
                                hang_frames--;
                                should_transmit = true;
                            }
                        }

                        if (should_transmit) {
                            uint8_t pkt[VOICE_PKT_BYTES];
                            ima_encode_frame(pcm, pkt, &enc_pred, &enc_idx, (uint8_t)mic_vol);
                            EnterCriticalSection(&g_voice_cs);
                            if (is_voice_channel_active()) {
                                if (g_voice_out_count >= VOICE_RING_MAX) {
                                    g_voice_out_head = (g_voice_out_head + 1) % VOICE_RING_MAX;
                                    g_voice_out_count--;
                                }
                                memcpy(g_voice_out_q[g_voice_out_tail], pkt, VOICE_PKT_BYTES);
                                g_voice_out_tail = (g_voice_out_tail + 1) % VOICE_RING_MAX;
                                g_voice_out_count++;
                                InterlockedIncrement(&g_voice_tx_count);
                            }

                            if (g_voice_loopback) {
                                if (g_voice_in_count >= VOICE_RING_MAX) {
                                    g_voice_in_head = (g_voice_in_head + 1) % VOICE_RING_MAX;
                                    g_voice_in_count--;
                                }
                                memcpy(g_voice_in_q[g_voice_in_tail], pkt, VOICE_PKT_BYTES);
                                g_voice_in_tail = (g_voice_in_tail + 1) % VOICE_RING_MAX;
                                g_voice_in_count++;
                            }
                            LeaveCriticalSection(&g_voice_cs);
                        } else {
                            enc_pred = 0;
                            enc_idx = 0;
                        }
                    }
                    waveInAddBuffer(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
                }
            }
        } else {
            Sleep(15);
        }

        /* Play incoming voice frames with proximity attenuation and Voices Volume scaling */
        if (has_spk) {
            for (;;) {
                WAVEHDR *hdr = &outHdrs[out_idx];
                if (!(hdr->dwFlags & WHDR_DONE)) break;

                uint8_t pkt[VOICE_PKT_BYTES];
                bool got = false;
                EnterCriticalSection(&g_voice_cs);
                if (g_voice_in_count > 0) {
                    memcpy(pkt, g_voice_in_q[g_voice_in_head], VOICE_PKT_BYTES);
                    g_voice_in_head = (g_voice_in_head + 1) % VOICE_RING_MAX;
                    g_voice_in_count--;
                    got = true;
                }
                LeaveCriticalSection(&g_voice_cs);
                if (!got) break;

                if (g_voice_enabled && !g_voice_muted && (is_voice_channel_active() || g_voice_loopback)) {
                    float vvol = (float)g_voice_vol / 100.0f;
                    float gain = vvol * voice_bits_float(InterlockedCompareExchange(&g_voice_prox_scale_bits, 0, 0));
                    if (ima_decode_frame(pkt, VOICE_PKT_BYTES, outBufs[out_idx], gain)) {
                        apply_voice_lowpass(outBufs[out_idx], VOICE_FRAME_SAMPLES,
                            voice_bits_float(InterlockedCompareExchange(&g_voice_partner_dist_bits, 0, 0)));
                        hdr->dwFlags &= ~WHDR_DONE;
                        waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
                        out_idx = (out_idx + 1) % VOICE_NUM_BUFS;
                    }
                }
            }
        }

        if (!is_voice_channel_active() && !g_voice_loopback && (g_voice_in_count > 0 || g_voice_out_count > 0)) {
            EnterCriticalSection(&g_voice_cs);
            g_voice_in_count = 0;
            g_voice_in_head = 0;
            g_voice_in_tail = 0;
            g_voice_out_count = 0;
            g_voice_out_head = 0;
            g_voice_out_tail = 0;
            LeaveCriticalSection(&g_voice_cs);
        }
    }

    if (has_mic) {
        waveInStop(hWaveIn);
        waveInReset(hWaveIn);
        for (int i = 0; i < VOICE_NUM_BUFS; i++) waveInUnprepareHeader(hWaveIn, &inHdrs[i], sizeof(WAVEHDR));
        waveInClose(hWaveIn);
    }
    if (hEventIn) CloseHandle(hEventIn);
    if (has_spk) {
        waveOutReset(hWaveOut);
        for (int i = 0; i < VOICE_NUM_BUFS; i++) waveOutUnprepareHeader(hWaveOut, &outHdrs[i], sizeof(WAVEHDR));
        waveOutClose(hWaveOut);
    }
    return 0;
}

static void voice_start(void)
{
    AcquireSRWLockExclusive(&g_voice_lifecycle_lock);
    if (!g_voice_cs_init) {
        InitializeCriticalSection(&g_voice_cs);
        g_voice_cs_init = true;
    }
    if (g_voice_thread_handle) {
        DWORD state = WaitForSingleObject(g_voice_thread_handle, 0);
        if (state == WAIT_TIMEOUT) {
            scct_log("[VOICE] Previous audio worker is still stopping; keeping the existing worker.");
            ReleaseSRWLockExclusive(&g_voice_lifecycle_lock);
            return;
        }
        CloseHandle(g_voice_thread_handle);
        g_voice_thread_handle = NULL;
    }
    if (!InterlockedCompareExchange(&g_voice_thread_running, 0, 0)) {
        if ((UINT)g_mic_device_id == WAVE_MAPPER) {
            WAVEINCAPSA caps;
            if (waveInGetDevCapsA(WAVE_MAPPER, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
                snprintf(g_mic_device_name, sizeof(g_mic_device_name), "Default (%s)", caps.szPname);
        }
        InterlockedExchange(&g_voice_thread_running, 1);
        g_voice_thread_handle = CreateThread(NULL, 0, voice_thread, NULL, 0, NULL);
        if (!g_voice_thread_handle) {
            InterlockedExchange(&g_voice_thread_running, 0);
            scct_log("[VOICE] Could not create audio worker (err=%lu)", GetLastError());
        }
    }
    ReleaseSRWLockExclusive(&g_voice_lifecycle_lock);
    printf("[MIC] Voice Chat Microphone: %s\n", g_mic_device_name);
}

static void voice_stop(void)
{
    AcquireSRWLockExclusive(&g_voice_lifecycle_lock);
    InterlockedExchange(&g_voice_thread_running, 0);
    if (g_voice_thread_handle) {
        DWORD result = WaitForSingleObject(g_voice_thread_handle, 5000);
        if (result == WAIT_OBJECT_0) {
            CloseHandle(g_voice_thread_handle);
            g_voice_thread_handle = NULL;
        } else {
            scct_log("[VOICE] Audio worker did not stop within 5 seconds (wait=%lu); retaining its handle safely.", result);
        }
    }
    ReleaseSRWLockExclusive(&g_voice_lifecycle_lock);
}

static void patch_running_games(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "splintercell3.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                if (h) {
                    patch_game_memory(h);
                    patch_game_voice(h, pe.th32ProcessID);
                    CloseHandle(h);
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

static bool is_game_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "splintercell3.exe") == 0) {
                    CloseHandle(snap);
                    return true;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }
    return false;
}

static void configure_game_borderless_ini(void)
{
    SetProcessDPIAware();
    int scr_w = GetSystemMetrics(SM_CXSCREEN);
    int scr_h = GetSystemMetrics(SM_CYSCREEN);
    if (scr_w < 640) scr_w = 1920;
    if (scr_h < 480) scr_h = 1080;

    char str_w[16], str_h[16];
    snprintf(str_w, sizeof(str_w), "%d", scr_w);
    snprintf(str_h, sizeof(str_h), "%d", scr_h);

    char parent_dir[MAX_PATH] = "";
    snprintf(parent_dir, sizeof(parent_dir), "%s", g_game_dir);
    char *parent_slash = strrchr(parent_dir, '\\');
    if (parent_slash) *parent_slash = '\0';
    const char *ini_dirs[] = { g_game_dir, parent_dir };

    for (size_t i = 0; i < sizeof(ini_dirs) / sizeof(ini_dirs[0]); i++) {
        if (!ini_dirs[i] || !ini_dirs[i][0]) continue;
        char ini_cand[MAX_PATH];
        /* 1. SplinterCell3.ini */
        snprintf(ini_cand, sizeof(ini_cand), "%s\\SplinterCell3.ini", ini_dirs[i]);
        if (GetFileAttributesA(ini_cand) != INVALID_FILE_ATTRIBUTES) {
            WritePrivateProfileStringA("WinDrv.WindowsClient", "WindowedViewportX", str_w, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "WindowedViewportY", str_h, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "FullscreenViewportX", str_w, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "FullscreenViewportY", str_h, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "StartupFullscreen", "False", ini_cand);
            WritePrivateProfileStringA("D3DDrv.D3DRenderDevice", "UseFullscreen", "False", ini_cand);
        }
        snprintf(ini_cand, sizeof(ini_cand), "%s\\System\\SplinterCell3.ini", ini_dirs[i]);
        if (GetFileAttributesA(ini_cand) != INVALID_FILE_ATTRIBUTES) {
            WritePrivateProfileStringA("WinDrv.WindowsClient", "WindowedViewportX", str_w, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "WindowedViewportY", str_h, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "FullscreenViewportX", str_w, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "FullscreenViewportY", str_h, ini_cand);
            WritePrivateProfileStringA("WinDrv.WindowsClient", "StartupFullscreen", "False", ini_cand);
            WritePrivateProfileStringA("D3DDrv.D3DRenderDevice", "UseFullscreen", "False", ini_cand);
        }
        /* 2. d3d9.ini */
        snprintf(ini_cand, sizeof(ini_cand), "%s\\d3d9.ini", ini_dirs[i]);
        if (GetFileAttributesA(ini_cand) != INVALID_FILE_ATTRIBUTES) {
            WritePrivateProfileStringA("MAIN", "ForceWindowedMode", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "BorderlessFullscreen", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "CenterWindow", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "UsePrimaryMonitor", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "DoNotNotifyOnTaskSwitch", "1", ini_cand);
        }
        snprintf(ini_cand, sizeof(ini_cand), "%s\\System\\d3d9.ini", ini_dirs[i]);
        if (GetFileAttributesA(ini_cand) != INVALID_FILE_ATTRIBUTES) {
            WritePrivateProfileStringA("MAIN", "ForceWindowedMode", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "BorderlessFullscreen", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "CenterWindow", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "UsePrimaryMonitor", "1", ini_cand);
            WritePrivateProfileStringA("FORCEWINDOWED", "DoNotNotifyOnTaskSwitch", "1", ini_cand);
        }
    }

    SetEnvironmentVariableA("__COMPAT_LAYER", "HighDpiAware");
    scct_log("[WINDOW] Configured Windowed Fullscreen INI settings (%dx%d)", scr_w, scr_h);
}

struct FindGameWndData {
    DWORD pid;
    HWND hWnd;
};

static BOOL CALLBACK EnumGameWndProc(HWND hwnd, LPARAM lParam)
{
    struct FindGameWndData *d = (struct FindGameWndData *)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (d->pid != 0 && pid == d->pid && IsWindowVisible(hwnd)) {
        RECT rc;
        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left > 100) && (rc.bottom - rc.top > 100)) {
            d->hWnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static void enforce_borderless_fullscreen(HWND hWnd)
{
    if (!hWnd) {
        hWnd = FindWindowA("SPLINTERCELL3PCCOOP", NULL);
        if (!hWnd) hWnd = FindWindowA("SPLINTERCELL3PC", NULL);
        if (!hWnd) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32 pe;
                pe.dwSize = sizeof(pe);
                if (Process32First(snap, &pe)) {
                    do {
                        if (_stricmp(pe.szExeFile, "splintercell3.exe") == 0) {
                            struct FindGameWndData data = { pe.th32ProcessID, NULL };
                            EnumWindows(EnumGameWndProc, (LPARAM)&data);
                            if (data.hWnd) {
                                hWnd = data.hWnd;
                                break;
                            }
                        }
                    } while (Process32Next(snap, &pe));
                }
                CloseHandle(snap);
            }
        }
    }
    if (!hWnd || !IsWindow(hWnd)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (!pid) return;

    int scr_w = GetSystemMetrics(SM_CXSCREEN);
    int scr_h = GetSystemMetrics(SM_CYSCREEN);
    if (scr_w < 640 || scr_h < 480) return;

    RECT rc;
    if (!GetWindowRect(hWnd, &rc)) return;

    LONG style = GetWindowLongA(hWnd, GWL_STYLE);
    LONG exStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);

    bool has_decorations = (style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) != 0;
    bool bad_pos = (rc.left != 0 || rc.top != 0 || (rc.right - rc.left) != scr_w || (rc.bottom - rc.top) != scr_h);

    if (has_decorations || bad_pos) {
        LONG new_style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME)) | WS_POPUP | WS_VISIBLE;
        SetWindowLongA(hWnd, GWL_STYLE, new_style);

        LONG new_exStyle = exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        SetWindowLongA(hWnd, GWL_EXSTYLE, new_exStyle);

        SetWindowPos(hWnd, HWND_TOP, 0, 0, scr_w, scr_h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);

        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            scct_log("[WINDOW] Borderless Fullscreen applied to HWND 0x%p (PID %lu): %dx%d at (0,0)", hWnd, pid, scr_w, scr_h);
            printf("[WINDOW] Borderless Fullscreen active (%dx%d, 0 screen blinking).\n", scr_w, scr_h);
        }
    }
}

static void launch_and_patch_game(void)
{
    if (g_no_launch) return;
    if (!locate_game()) {
        printf("[WARN] splintercell3.exe not found automatically. Start the game manually.\n");
        return;
    }

    /* Configure game for Windowed Fullscreen to avoid screen blinking and D3D device loss */
    configure_game_borderless_ini();

    if (is_game_running()) {
        printf("[GAME] Splinter Cell Chaos Theory is already running. Integrating with session...\n");
        patch_running_games();
        enforce_borderless_fullscreen(NULL);
        return;
    }

    printf("[GAME] Launching Splinter Cell Chaos Theory in Windowed Fullscreen (no blinking)...\n");

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "\"%s\"", g_game_exe_path);

    /* Launch with CREATE_SUSPENDED so D3D9 Force-Windowed patch is written BEFORE D3D device creation */
    if (CreateProcessA(g_game_exe_path, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, g_game_dir, &si, &pi)) {
        g_game_proc = pi.hProcess;

        /* Apply in-memory patches immediately */
        patch_game_memory(pi.hProcess);

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        /* Fast-poll for window creation to enforce borderless fullscreen before first intro frame renders */
        DWORD t_start = GetTickCount();
        while (GetTickCount() - t_start < 4000) {
            HWND hEarly = FindWindowA("SPLINTERCELL3PCCOOP", NULL);
            if (!hEarly) hEarly = FindWindowA("SPLINTERCELL3PC", NULL);
            if (!hEarly) {
                struct FindGameWndData data = { pi.dwProcessId, NULL };
                EnumWindows(EnumGameWndProc, (LPARAM)&data);
                hEarly = data.hWnd;
            }
            if (hEarly && IsWindow(hEarly)) {
                enforce_borderless_fullscreen(hEarly);
                break;
            }
            Sleep(25);
        }

        patch_running_games();
    } else {
        printf("[WARN] Could not launch game automatically (error %lu). Start game manually.\n", GetLastError());
    }
}

/* ---------------------------------------------------------------- Callback Dispatcher */

typedef void (*CbHandler)(int id, void *data);

/* Steam callbacks are copied while valid, then their handlers run only after
 * FreeLastCallback. Interface calls from inside callback dispatch can hit
 * SteamNetworking's internal lock assertions on some Steam builds. */
typedef struct {
    int id;
    int size;
    unsigned char data[sizeof(GameRichPresenceJoinRequested)];
} DeferredSteamCallback;

#define MAX_DEFERRED_STEAM_CALLBACKS 64
#define STEAM_IDENTITY_CALLBACK_SIZE 136
static DeferredSteamCallback g_deferred_steam_callbacks[MAX_DEFERRED_STEAM_CALLBACKS];
static int g_deferred_steam_callback_count = 0;

static int steam_callback_payload_size(int id)
{
    switch (id) {
    case CB_LOBBY_CREATED: return sizeof(LobbyCreated);
    case CB_LOBBY_ENTER: return sizeof(LobbyEnter);
    case CB_LOBBY_CHAT_UPDATE: return sizeof(LobbyChatUpdate);
    case CB_LOBBY_JOIN_REQUESTED: return sizeof(GameLobbyJoinRequested);
    case CB_RICH_PRESENCE_JOIN: return sizeof(GameRichPresenceJoinRequested);
    case CB_LOBBY_MATCH_LIST: return sizeof(LobbyMatchList_t);
    case CB_SESSION_REQUEST: return STEAM_IDENTITY_CALLBACK_SIZE;
    case CB_SESSION_FAILED: return sizeof(SteamSessionFailurePrefix);
    default: return 0;
    }
}

static void dispatch_deferred_steam_callbacks(CbHandler h)
{
    int count = g_deferred_steam_callback_count;
    g_deferred_steam_callback_count = 0;
    for (int i = 0; i < count; i++) {
        DeferredSteamCallback *e = &g_deferred_steam_callbacks[i];
        h(e->id, e->data);
    }
}

static void pump_steam(CbHandler h)
{
    if (!g_steam_available) return;
    CallbackMsg msg;
    S.RunFrame(g_pipe);
    while (S.GetNextCallback(g_pipe, &msg)) {
        if (msg.callback == CB_CALL_COMPLETED) {
            APICallCompleted *done = (APICallCompleted *)msg.param;
            int size = steam_callback_payload_size(done->callback);
            if (size > 0 &&
                (uint32_t)size <= (uint32_t)done->param_size &&
                size <= (int)sizeof(g_deferred_steam_callbacks[0].data) &&
                g_deferred_steam_callback_count < MAX_DEFERRED_STEAM_CALLBACKS) {
                unsigned char result_buf[sizeof(g_deferred_steam_callbacks[0].data) + 16] = {0};
                bool failed = true;
                if (S.GetAPICallResult(g_pipe, done->call, result_buf,
                                       size, done->callback, &failed) && !failed) {
                    DeferredSteamCallback *e = &g_deferred_steam_callbacks[g_deferred_steam_callback_count++];
                    e->id = done->callback;
                    e->size = size;
                    memcpy(e->data, result_buf, (size_t)size);
                }
            }
        } else {
            int size = steam_callback_payload_size(msg.callback);
            if (msg.param && size > 0 && size <= (int)sizeof(g_deferred_steam_callbacks[0].data) &&
                g_deferred_steam_callback_count < MAX_DEFERRED_STEAM_CALLBACKS) {
                DeferredSteamCallback *e = &g_deferred_steam_callbacks[g_deferred_steam_callback_count++];
                e->id = msg.callback;
                e->size = size;
                memcpy(e->data, msg.param, size);
            }
        }
        S.FreeLastCallback(g_pipe);
        /* Run state handlers immediately after this callback is released, so
         * they may safely call Steam interfaces without holding its callback lock. */
        dispatch_deferred_steam_callbacks(h);
    }
}

/* ---------------------------------------------------------------- Peer Management */

static Peer *find_peer(u64 sid)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peers[i].sid == sid) return &g_peers[i];
    }
    return NULL;
}

static Peer *add_peer(u64 sid)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (p->sid) continue;
        memset(p, 0, sizeof(*p));
        p->sid = sid;
        p->last_heard = p->last_ping = GetTickCount();
        printf("[PEER] Co-op partner connected: %s\n", steam_friend_name(sid));
        return p;
    }
    return NULL;
}

static void drop_peer(Peer *p, const char *why)
{
    if (!p || !p->sid) return;
    printf("[PEER] %s left (%s).\n", steam_friend_name(p->sid), why);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (p->conns[i].s) closesocket(p->conns[i].s);
    }
    steam_close(p->sid);
    memset(p, 0, sizeof(*p));
}

static Conn *peer_game_conn(Peer *p, int id, int target_port)
{
    Conn *free_slot = NULL, *oldest = &p->conns[0];
    for (int i = 0; i < MAX_CONNS; i++) {
        Conn *c = &p->conns[i];
        if (c->s && c->id == id && c->port == target_port) return c;
        if (!c->s && !free_slot) free_slot = c;
        if (c->last < oldest->last) oldest = c;
    }
    if (!free_slot) {
        closesocket(oldest->s);
        memset(oldest, 0, sizeof(*oldest));
        free_slot = oldest;
    }
    free_slot->s = udp_connect_loopback(target_port);
    if (free_slot->s == INVALID_SOCKET) {
        free_slot->s = 0;
        return NULL;
    }
    free_slot->id = id;
    free_slot->port = target_port;
    free_slot->last = GetTickCount();
    return free_slot;
}

static void scrub_beacon_ip(unsigned char *payload, int len)
{
    /* Privacy: Overwrite the host's private LAN IP inside the game beacon with 127.0.0.1
     * BEFORE the packet ever leaves the host machine, so peers never see the host's LAN IP. */
    if (len >= 12 && payload[0] == 8) {
        *(uint32_t *)(payload + 8) = htonl(INADDR_LOOPBACK);
    } else if (len >= 32 && payload[0] == 0x45) {
        *(uint32_t *)(payload + 28) = htonl(INADDR_LOOPBACK);
    }
}

static bool is_allowed_disc_port(uint16_t p)
{
    return (p == 8888 || p == 8889 || p == 9102 || p == 9103 || p == 8878);
}

/* ---------------------------------------------------------------- UNIVERSAL DIRECT ONLINE CO-OP (HOST) */

static int run_universal_host(int listen_port)
{
    if (listen_port < 1 || listen_port > 65535) fail("Invalid direct relay port (use 1-65535).");
    printf("\n[HOST] Starting Universal Online Co-op Session...\n");

    char public_ip[64] = "127.0.0.1";
    bool got_ip = fetch_public_ip(public_ip, sizeof(public_ip));
    if (!got_ip) scct_log("[NETWORK] Public IP lookup failed; Room Code will use the fallback address.");
    char room_code[64] = "";
    ip_to_code(public_ip, room_code, sizeof(room_code));

    SOCKET s_ext = udp_bind(listen_port, false, true);
    g_direct_ext_sock = s_ext;
    if (s_ext == INVALID_SOCKET) fail("Could not bind Universal relay port.");

    SOCKET s_disc[NUM_DISC_PORTS];
    /* Host: never bind discovery ports; the game server owns 8888 and must be free to create a session */
    for (int i = 0; i < NUM_DISC_PORTS; i++) s_disc[i] = INVALID_SOCKET;

    SOCKET s_host_query = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_host_query == INVALID_SOCKET) fail("Could not create host discovery socket.");
    configure_udp_socket(s_host_query);

    copy_to_clipboard(room_code);

    printf("======================================================================\n");
    printf("  UNIVERSAL ONLINE CO-OP HOST ACTIVE (CROSS-PLAY)\n");
    printf("  Room Code : %s (COPIED TO CLIPBOARD!)\n", room_code);
    printf("  Public IP : %s\n", public_ip);
    printf("  Relay Port: %d\n", listen_port);
    printf("======================================================================\n");
    printf("Instructions for Host:\n");
    printf("  1. Send your Room Code (%s) or Public IP to your co-op partner.\n", room_code);
    printf("     (Works on Steam, Ubisoft Connect, and Standalone editions alike!)\n");
    printf("  2. In Splinter Cell Chaos Theory, go to:\n");
    printf("       -> COOP  ->  ONLINE (or LAN)  ->  CREATE GAME\n");
    printf("  3. Pick your mission and wait in the lobby.\n");
    printf("  4. Keep this console window open while playing! (Press ESC to stop)\n");
    printf("======================================================================\n\n");

    launch_and_patch_game();
    voice_start();

    struct sockaddr_in client_remote;
    memset(&client_remote, 0, sizeof(client_remote));
    bool has_client = false;
    DWORD last_client_heard = 0;
    Conn conns[MAX_CONNS];
    memset(conns, 0, sizeof(conns));

    static unsigned char buf[65536];
    DWORD last_status = GetTickCount();
    DWORD last_patch_check = 0;
    DWORD last_voice_tick = 0;
    DWORD game_start_time = 0;
    uint32_t packets_in = 0, packets_out = 0;

    printf("[STATUS] Relay & Proximity Voice Active. Waiting for co-op traffic... (Press ESC to stop)\n\n");

    while (g_running) {
        DWORD now = GetTickCount();

        if (now - last_voice_tick >= 40) {
            last_voice_tick = now;
            voice_tick_game();
        }

        if (has_client && now - last_client_heard > 60000) {
            scct_log("[NETWORK] Direct partner disconnected: heartbeat timeout after %lu ms", now - last_client_heard);
            has_client = false;
            g_direct_remote_host.sin_port = 0;
            for (int i = 0; i < MAX_CONNS; i++) {
                if (conns[i].s) closesocket(conns[i].s);
                memset(&conns[i], 0, sizeof(conns[i]));
            }
            printf("[NETWORK] Partner timed out. Slot is open again.\n");
        }

        static DWORD s_last_host_udp_ping = 0;
        if (has_client && (now - s_last_host_udp_ping > 1500)) {
            s_last_host_udp_ping = now;
            unsigned char ping[2] = { CH_CTRL, 'K' };
            sendto(s_ext, (char *)ping, sizeof(ping), 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
        }

        if (!g_no_launch) {
            bool game_alive = is_game_running();
            if (game_alive && !game_start_time) game_start_time = now;
            if (game_start_time && now - game_start_time > 20000 && !game_alive) {
                printf("[GAME] Game process terminated. Ending session...\n");
                break;
            }
        }

        /* Process packets from remote partner */
        struct sockaddr_in from;
        int fl = sizeof(from);
        int got;
        for (int drain = 0; drain < MAX_RELAY_DRAIN &&
             (got = recvfrom(s_ext, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl)) > 0; drain++) {
            if (got < 1) continue;
            if (!has_client) {
                client_remote = from;
                has_client = true;
                g_direct_remote_host = client_remote;
                last_client_heard = now;
                printf("[NETWORK] Co-op partner connected (IP hidden for privacy)!\n");
            } else if (from.sin_addr.s_addr == client_remote.sin_addr.s_addr) {
                client_remote.sin_port = from.sin_port;
                g_direct_remote_host = client_remote; /* Follow NAT port rebinding for control messages too. */
                last_client_heard = now;
            } else {
                fl = sizeof(from);
                continue; /* Ignore packets from unknown third-party IPs while partner is connected */
            }
            uint8_t ch = buf[0];
            if (ch == CH_CTRL) {
                if (got >= 2 && buf[1] == 'K') {
                    unsigned char pong[2] = { CH_CTRL, 'P' };
                    sendto(s_ext, (char *)pong, sizeof(pong), 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
                }
                continue; /* Session keepalive / NAT ping */
            }
            if (got < 2) continue;
            unsigned char *payload = buf + 1;
            int payload_len = got - 1;

            if (ch == CH_VOICE) {
                if (is_voice_channel_active()) {
                    voice_push_incoming(payload, payload_len);
                }
                fl = sizeof(from);
                continue;
            }

            if (ch == CH_DISC && payload_len >= 2) {
                uint16_t target_port = ntohs(*(uint16_t *)payload);
                if (!is_allowed_disc_port(target_port)) target_port = 8888;
                unsigned char *disc_data = payload + 2;
                int disc_len = payload_len - 2;

                struct sockaddr_in target;
                memset(&target, 0, sizeof(target));
                target.sin_family = AF_INET;
                target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                target.sin_port = htons(target_port);
                sendto(s_host_query, (char *)disc_data, disc_len, 0, (struct sockaddr *)&target, sizeof(target));

                if (target_port != 8888) {
                    target.sin_port = htons(8888);
                    sendto(s_host_query, (char *)disc_data, disc_len, 0, (struct sockaddr *)&target, sizeof(target));
                }
            } else if (ch == CH_GAME && payload_len > 3) {
                packets_in++;
                InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                uint16_t target_port = ntohs(*(uint16_t *)payload);
                int conn_id = payload[2];
                unsigned char *game_data = payload + 3;
                int game_len = payload_len - 3;

                /* Route forwarded game traffic to SCCT Unreal Engine ports (7777 or 7776) */
                int host_port = (target_port == 7776) ? 7776 : 7777;

                Conn *target_c = NULL;
                Conn *free_slot = NULL;
                for (int i = 0; i < MAX_CONNS; i++) {
                    if (conns[i].s && conns[i].id == conn_id && conns[i].port == host_port) {
                        target_c = &conns[i];
                        break;
                    }
                    if (!conns[i].s && !free_slot) free_slot = &conns[i];
                }
                if (!target_c && free_slot) {
                    free_slot->s = udp_connect_loopback(host_port);
                    if (free_slot->s == INVALID_SOCKET) {
                        memset(free_slot, 0, sizeof(*free_slot));
                        free_slot = NULL;
                    }
                    if (!free_slot) { fl = sizeof(from); continue; }
                    free_slot->id = conn_id;
                    free_slot->port = host_port;
                    target_c = free_slot;
                }
                if (target_c && target_c->s) {
                    target_c->last = GetTickCount();
                    send(target_c->s, (char *)game_data, game_len, 0);
                }
            }
            fl = sizeof(from);
        }

        /* Forward discovery responses from local host query socket */
        for (int drain = 0; drain < MAX_RELAY_DRAIN &&
             (got = recvfrom(s_host_query, (char *)buf + 3, sizeof(buf) - 3, 0, NULL, NULL)) > 0; drain++) {
            if (has_client) {
                scrub_beacon_ip(buf + 3, got);
                buf[0] = CH_DISC;
                *(uint16_t *)(buf + 1) = htons(8888);
                sendto(s_ext, (char *)buf, got + 3, 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
            }
        }

        /* Forward local discovery beacons from listeners */
        for (int i = 0; i < NUM_DISC_PORTS; i++) {
            if (s_disc[i] == INVALID_SOCKET) continue;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got = recvfrom(s_disc[i], (char *)buf + 3, sizeof(buf) - 3, 0, NULL, NULL)) > 0; drain++) {
                if (has_client) {
                    scrub_beacon_ip(buf + 3, got);
                    buf[0] = CH_DISC;
                    *(uint16_t *)(buf + 1) = htons((u_short)DISC_PORTS[i]);
                    sendto(s_ext, (char *)buf, got + 3, 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
                }
            }
        }

        /* Forward game replies from local host game */
        now = GetTickCount();
        for (int i = 0; i < MAX_CONNS; i++) {
            if (!conns[i].s) continue;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got = recv(conns[i].s, (char *)buf + 4, sizeof(buf) - 4, 0)) > 0; drain++) {
                if (has_client) {
                    packets_out++;
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    buf[0] = CH_GAME;
                    *(uint16_t *)(buf + 1) = htons((u_short)conns[i].port);
                    buf[3] = (unsigned char)conns[i].id;
                    sendto(s_ext, (char *)buf, got + 4, 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
                }
            }
            /* Loading, pauses, and videos can silence game datagrams while the
             * partner heartbeat remains healthy. Session timeout owns cleanup. */
            if (!has_client && now - conns[i].last > 60000) {
                closesocket(conns[i].s);
                memset(&conns[i], 0, sizeof(conns[i]));
            }
        }

        /* Forward outgoing voice frames to remote partner */
        int vlen;
        for (int voice_frames = 0; voice_frames < 2 &&
             (vlen = voice_pop_outgoing(buf + 1, sizeof(buf) - 1)) > 0; voice_frames++) {
            if (has_client && is_voice_channel_active()) {
                buf[0] = CH_VOICE;
                sendto(s_ext, (char *)buf, vlen + 1, 0, (struct sockaddr *)&client_remote, sizeof(client_remote));
            }
        }

        if (now - last_patch_check > 1000) {
            last_patch_check = now;
            patch_running_games();
        }

        if (now - last_status > 5000) {
            last_status = now;
            const char *mode_str = (g_voice_mode == VOICE_MODE_PTT_V) ? "PTT [V]" :
                                   (g_voice_mode == VOICE_MODE_PTT_CAPS) ? "PTT [Caps]" :
                                   (g_voice_mode == VOICE_MODE_PTT_MOUSE4) ? "PTT [Mouse4]" :
                                   (g_voice_mode == VOICE_MODE_PTT_T) ? "PTT [T]" : "Open Mic";
            printf("[STATUS] Traffic: In=%u Out=%u | Voice: %s %s | Mic: %ld%% | TX: %ld RX: %ld\n",
                   packets_in, packets_out, mode_str,
                   (g_voice_mode != VOICE_MODE_OPEN && !g_voice_ptt_active) ? "[MUTE]" : "[TALK]",
                   g_voice_ui_level, g_voice_tx_count, g_voice_rx_count);
        }

        if (_kbhit() && _getch() == 27) break;
        Sleep(1);
    }

    voice_stop();
    g_direct_ext_sock = INVALID_SOCKET;
    closesocket(s_ext);
    closesocket(s_host_query);
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        if (s_disc[i] != INVALID_SOCKET) closesocket(s_disc[i]);
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].s) closesocket(conns[i].s);
    }
    return 0;
}

/* ---------------------------------------------------------------- UNIVERSAL DIRECT ONLINE CO-OP (JOIN) */

static int run_universal_join(const char *target_input, int target_port)
{
    if (target_port < 1 || target_port > 65535) fail("Invalid direct relay port (use 1-65535).");
    char input_addr[128] = "";
    if (target_input && target_input[0]) {
        strncpy(input_addr, target_input, sizeof(input_addr) - 1);
    } else {
        printf("\n======================================================================\n");
        printf("  JOIN UNIVERSAL ONLINE ROOM\n");
        printf("======================================================================\n");
        printf("Enter the Host Room Code / Public IP: ");
        if (fgets(input_addr, sizeof(input_addr), stdin)) {
            input_addr[strcspn(input_addr, "\r\n")] = 0;
        }
    }

    if (!input_addr[0]) {
        fail("No host address provided.");
    }

    char host_addr_str[128] = "";
    if (code_to_ip(input_addr, host_addr_str, sizeof(host_addr_str))) {
        printf("[DECODE] Converted Room Code '%s' -> Host IP %s\n", input_addr, host_addr_str);
    } else {
        strncpy(host_addr_str, input_addr, sizeof(host_addr_str) - 1);
    }

    printf("\n[CLIENT] Connecting to host: %s:%d...\n", host_addr_str, target_port);

    SOCKET s_ext = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ext == INVALID_SOCKET) fail("Could not create direct relay socket.");
    configure_udp_socket(s_ext);

    struct sockaddr_in remote_host;
    memset(&remote_host, 0, sizeof(remote_host));
    remote_host.sin_family = AF_INET;
    remote_host.sin_port = htons((u_short)target_port);

    struct hostent *he = gethostbyname(host_addr_str);
    if (he && he->h_addr_list[0]) {
        memcpy(&remote_host.sin_addr, he->h_addr_list[0], he->h_length);
    } else {
        remote_host.sin_addr.s_addr = inet_addr(host_addr_str);
    }
    if (remote_host.sin_addr.s_addr == INADDR_NONE) fail("Invalid host IP address or Room Code.");
    g_direct_ext_sock = s_ext;
    g_direct_remote_host = remote_host;

    /* Send initial handshake ping to open NAT and register with host */
    unsigned char ping_pkt[1] = { CH_CTRL };
    sendto(s_ext, (char *)ping_pkt, 1, 0, (struct sockaddr *)&remote_host, sizeof(remote_host));

    SOCKET s_disc[NUM_DISC_PORTS];
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        s_disc[i] = udp_bind(DISC_PORTS[i], true, true);
        if (s_disc[i] == INVALID_SOCKET) fail("Could not bind a required game discovery port; close other copies and retry.");
    }

    SOCKET s_game[NUM_GAME_PORTS];
    for (int j = 0; j < NUM_GAME_PORTS; j++) {
        s_game[j] = udp_bind(GAME_PORTS[j], false, true);
        if (s_game[j] == INVALID_SOCKET) fail("Could not bind a required game traffic port; close other copies and retry.");
    }

    SOCKET s_inject = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_inject == INVALID_SOCKET) fail("Could not create local discovery injection socket.");
    configure_udp_socket(s_inject);

    printf("======================================================================\n");
    printf("  CONNECTED TO HOST RELAY\n");
    printf("======================================================================\n");
    printf("Instructions for Client:\n");
    printf("  1. In Splinter Cell Chaos Theory, navigate to:\n");
    printf("       -> COOP  ->  ONLINE (or LAN)  ->  JOIN GAME\n");
    printf("  2. The host's session will appear in the Games List automatically!\n");
    printf("  3. Select it and click Ready to launch the mission together!\n");
    printf("  4. Keep this console window open while playing! (Press ESC to stop)\n");
    printf("======================================================================\n\n");

    launch_and_patch_game();
    voice_start();

    struct sockaddr_in client_query_from;
    bool has_query_client = false;

    struct sockaddr_in game_clients[MAX_CONNS];
    int game_ports[MAX_CONNS];
    unsigned char game_ids[MAX_CONNS];
    int game_conns_n = 0;
    unsigned char next_conn_id = 0;

    static unsigned char buf[65536];
    DWORD last_status = GetTickCount();
    DWORD last_patch_check = 0;
    DWORD last_voice_tick = 0;
    DWORD last_keepalive = 0;
    DWORD last_host_heard = GetTickCount();
    bool host_silence_logged = false;
    DWORD game_start_time = 0;
    uint32_t packets_in = 0, packets_out = 0;

    printf("[STATUS] Relay & Proximity Voice Active. Waiting for session discovery... (Press ESC to stop)\n\n");

    while (g_running) {
        DWORD now = GetTickCount();

        if (now - last_voice_tick >= 40) {
            last_voice_tick = now;
            voice_tick_game();
        }

        if (now - last_keepalive > 1500) {
            last_keepalive = now;
            unsigned char heartbeat_pkt[2] = { CH_CTRL, 'K' };
            sendto(s_ext, (char *)heartbeat_pkt, sizeof(heartbeat_pkt), 0, (struct sockaddr *)&remote_host, sizeof(remote_host));
        }
        if (now - last_host_heard > 15000 && !host_silence_logged) {
            host_silence_logged = true;
            scct_log("[NETWORK] Direct host heartbeat missing for %lu ms; retrying handshake", now - last_host_heard);
            printf("[NETWORK] Host is not responding; retrying the room connection.\n");
        }

        if (!g_no_launch) {
            bool game_alive = is_game_running();
            if (game_alive && !game_start_time) game_start_time = now;
            if (game_start_time && now - game_start_time > 20000 && !game_alive) {
                printf("[GAME] Game process terminated. Ending session...\n");
                break;
            }
        }

        /* Forward broadcast queries from local client game */
        for (int i = 0; i < NUM_DISC_PORTS; i++) {
            if (s_disc[i] == INVALID_SOCKET) continue;
            struct sockaddr_in from;
            int fl = sizeof(from);
            int got;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got = recvfrom(s_disc[i], (char *)buf + 3, sizeof(buf) - 3, 0, (struct sockaddr *)&from, &fl)) > 0; drain++) {
                client_query_from = from;
                has_query_client = true;
                buf[0] = CH_DISC;
                *(uint16_t *)(buf + 1) = htons((u_short)DISC_PORTS[i]);
                sendto(s_ext, (char *)buf, got + 3, 0, (struct sockaddr *)&remote_host, sizeof(remote_host));
                fl = sizeof(from);
            }
        }

        /* Forward game packets from local client game */
        for (int j = 0; j < NUM_GAME_PORTS; j++) {
            if (s_game[j] == INVALID_SOCKET) continue;
            struct sockaddr_in from_game;
            int fl_g = sizeof(from_game);
            int got_game;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got_game = recvfrom(s_game[j], (char *)buf + 4, sizeof(buf) - 4, 0, (struct sockaddr *)&from_game, &fl_g)) > 0; drain++) {
                int k;
                for (k = 0; k < game_conns_n; k++) {
                    if (game_clients[k].sin_port == from_game.sin_port &&
                        game_clients[k].sin_addr.s_addr == from_game.sin_addr.s_addr &&
                        game_ports[k] == GAME_PORTS[j]) break;
                }
                if (k == game_conns_n) {
                    if (game_conns_n < MAX_CONNS) game_conns_n++;
                    else k = 0;
                    game_clients[k] = from_game;
                    game_ports[k] = GAME_PORTS[j];
                    game_ids[k] = next_conn_id++;
                }
                buf[0] = CH_GAME;
                *(uint16_t *)(buf + 1) = htons((u_short)GAME_PORTS[j]);
                buf[3] = game_ids[k];
                packets_out++;
                InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                sendto(s_ext, (char *)buf, got_game + 4, 0, (struct sockaddr *)&remote_host, sizeof(remote_host));
                fl_g = sizeof(from_game);
            }
        }

        /* Receive packets from remote host */
        int got_ext;
        for (int drain = 0; drain < MAX_RELAY_DRAIN &&
             (got_ext = recvfrom(s_ext, (char *)buf, sizeof(buf), 0, NULL, NULL)) > 0; drain++) {
            if (got_ext < 1) continue;
            last_host_heard = now;
            host_silence_logged = false;
            InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
            uint8_t ch = buf[0];
            if (ch == CH_CTRL) {
                if (got_ext >= 2 && buf[1] == 'K') {
                    unsigned char pong[2] = { CH_CTRL, 'P' };
                    sendto(s_ext, (char *)pong, sizeof(pong), 0, (struct sockaddr *)&remote_host, sizeof(remote_host));
                }
                continue;
            }
            if (ch == CH_VOICE) {
                if (is_voice_channel_active()) {
                    voice_push_incoming(buf + 1, got_ext - 1);
                }
                continue;
            }
            if (ch == CH_DISC && got_ext > 3) {
                uint16_t disc_port = ntohs(*(uint16_t *)(buf + 1));
                unsigned char *disc_payload = buf + 3;
                int disc_len = got_ext - 3;

                /* Rewrite server IP address in beacon to loopback (127.0.0.1) so client connects to local relay */
                if (disc_len >= 12 && disc_payload[0] == 8) {
                    *(uint32_t *)(disc_payload + 8) = htonl(INADDR_LOOPBACK);
                } else if (disc_len >= 32 && disc_payload[0] == 0x45) {
                    *(uint32_t *)(disc_payload + 28) = htonl(INADDR_LOOPBACK);
                }

                struct sockaddr_in to_8888;
                memset(&to_8888, 0, sizeof(to_8888));
                to_8888.sin_family = AF_INET;
                to_8888.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                to_8888.sin_port = htons(8888);
                sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));

                to_8888.sin_port = htons(8889);
                sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));

                if (disc_port != 8888 && disc_port != 8889) {
                    to_8888.sin_port = htons(disc_port);
                    sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));
                }

                if (has_query_client) {
                    sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&client_query_from, sizeof(client_query_from));
                }
            } else if (ch == CH_GAME && got_ext > 4) {
                packets_in++;
                InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                uint8_t cid = buf[3];
                unsigned char *game_data = buf + 4;
                int game_len = got_ext - 4;

                for (int k = 0; k < game_conns_n; k++) {
                    if (game_ids[k] == cid) {
                        for (int j = 0; j < NUM_GAME_PORTS; j++) {
                            if (GAME_PORTS[j] == game_ports[k] && s_game[j] != INVALID_SOCKET) {
                                sendto(s_game[j], (char *)game_data, game_len, 0,
                                       (struct sockaddr *)&game_clients[k], sizeof(game_clients[k]));
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }

        /* Forward outgoing voice frames to remote host */
        int vlen;
        for (int voice_frames = 0; voice_frames < 2 &&
             (vlen = voice_pop_outgoing(buf + 1, sizeof(buf) - 1)) > 0; voice_frames++) {
            if (is_voice_channel_active()) {
                buf[0] = CH_VOICE;
                sendto(s_ext, (char *)buf, vlen + 1, 0, (struct sockaddr *)&remote_host, sizeof(remote_host));
            }
        }

        now = GetTickCount();
        if (now - last_patch_check > 1000) {
            last_patch_check = now;
            patch_running_games();
        }

        if (now - last_status > 5000) {
            last_status = now;
            const char *mode_str = (g_voice_mode == VOICE_MODE_PTT_V) ? "PTT [V]" :
                                   (g_voice_mode == VOICE_MODE_PTT_CAPS) ? "PTT [Caps]" :
                                   (g_voice_mode == VOICE_MODE_PTT_MOUSE4) ? "PTT [Mouse4]" :
                                   (g_voice_mode == VOICE_MODE_PTT_T) ? "PTT [T]" : "Open Mic";
            printf("[STATUS] Traffic: In=%u Out=%u | Voice: %s %s | Mic: %ld%% | TX: %ld RX: %ld\n",
                   packets_in, packets_out, mode_str,
                   (g_voice_mode != VOICE_MODE_OPEN && !g_voice_ptt_active) ? "[MUTE]" : "[TALK]",
                   g_voice_ui_level, g_voice_tx_count, g_voice_rx_count);
        }

        if (_kbhit() && _getch() == 27) break;
        Sleep(1);
    }

    voice_stop();
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        if (s_disc[i] != INVALID_SOCKET) closesocket(s_disc[i]);
    }
    for (int j = 0; j < NUM_GAME_PORTS; j++) {
        if (s_game[j] != INVALID_SOCKET) closesocket(s_game[j]);
    }
    closesocket(s_inject);
    g_direct_ext_sock = INVALID_SOCKET;
    closesocket(s_ext);
    return 0;
}

/* ---------------------------------------------------------------- STEAM HOST & CLIENT */

static void host_steam_cb(int id, void *data)
{
    switch (id) {
    case CB_LOBBY_CREATED: {
        LobbyCreated *c = (LobbyCreated *)data;
        if (c->result != 1) { g_lobby_failed = true; break; }
        g_lobby = c->lobby;
        char buf[64];
        S.SetLobbyData(g_mm, g_lobby, "scct_coop", "1");
        S.SetLobbyData(g_mm, g_lobby, "host", S.GetPersonaName(g_friends));
        S.SetLobbyData(g_mm, g_lobby, "version", SCCT_VERSION);
        snprintf(buf, sizeof(buf), "+connect_lobby %llu", (unsigned long long)g_lobby);
        S.SetRichPresence(g_friends, "connect", buf);
        S.SetRichPresence(g_friends, "status", "Hosting Splinter Cell Chaos Theory Co-op");
        break;
    }
    case CB_SESSION_REQUEST: {
        u64 sid = S.IdentGet(data);
        steam_accept(sid);
        if (!find_peer(sid)) add_peer(sid);
        break;
    }
    case CB_SESSION_FAILED: {
        u64 sid = S.IdentGet((char *)data);
        SteamSessionFailurePrefix failure;
        memcpy(&failure, data, sizeof(failure));
        Peer *p = find_peer(sid);
        if (p) {
            /* Do not restart a session on every ordinary send. One host-side recovery
             * probe is enough; the 10-second guard prevents duplicate restart races. */
            scct_log("[STEAM] Session to %llu failed (state=%ld/%s end_reason=%ld/%s)",
                     sid, (long)failure.state, steam_failure_state_name(failure.state),
                     (long)failure.end_reason, steam_end_reason_name(failure.end_reason));
            steam_restart_peer(p, "failure callback");
        } else {
            /* A failure callback alone is not proof that this user joined our lobby.
             * Do not allocate a relay peer until a session request is accepted. */
            scct_log("[STEAM] Ignoring failed session for non-peer %llu (state=%ld/%s end_reason=%ld/%s)",
                     sid, (long)failure.state, steam_failure_state_name(failure.state),
                     (long)failure.end_reason, steam_end_reason_name(failure.end_reason));
        }
        break;
    }
    case CB_LOBBY_CHAT_UPDATE: {
        LobbyChatUpdate *u = (LobbyChatUpdate *)data;
        if (u->state & (0x2 | 0x8 | 0x10)) {
            drop_peer(find_peer(u->user_changed), (u->state & 0x2) ? "left lobby" : "removed from lobby");
        } else if (u->state & 0x4) {
            Peer *p = find_peer(u->user_changed);
            if (p) {
                scct_log("[STEAM] Peer %llu temporarily disconnected; preserving peer for relay recovery", u->user_changed);
            }
        }
        break;
    }
    }
}

static int run_steam_host(void)
{
    printf("\n[HOST] Initializing Worldwide Public Co-op Session (Steam SDR Relay)...\n");
    /* k_ELobbyTypePublic = 2 allows non-friends, cross-account, and standalone edition joins */
    S.CreateLobby(g_mm, 2 /* Public */, 2);

    DWORD t0 = GetTickCount();
    while (!g_lobby && !g_lobby_failed && GetTickCount() - t0 < 15000) {
        pump_steam(host_steam_cb);
        Sleep(20);
    }

    if (!g_lobby) fail("Could not create Steam lobby.");

    char lobby_id_str[64];
    snprintf(lobby_id_str, sizeof(lobby_id_str), "%llu", (unsigned long long)g_lobby);
    copy_to_clipboard(lobby_id_str);

    printf("======================================================================\n");
    printf("  WORLDWIDE PUBLIC CO-OP HOST ACTIVE (ZERO PORT-FORWARDING)\n");
    printf("  Host Name: %s\n", S.GetPersonaName(g_friends));
    printf("  Lobby ID : %s (COPIED TO CLIPBOARD!)\n", lobby_id_str);
    printf("======================================================================\n");
    printf("Instructions for Host:\n");
    printf("  1. Your session is now VISIBLE WORLDWIDE in the Public Server Browser!\n");
    printf("     (Any player can join with 1-click, or via the Lobby ID).\n");
    printf("  2. In Splinter Cell Chaos Theory, go to:\n");
    printf("       -> COOP  ->  ONLINE (or LAN)  ->  CREATE GAME\n");
    printf("  3. Pick your mission and wait in the lobby.\n");
    printf("  4. Keep this console window open while playing! (Press ESC to stop)\n");
    printf("======================================================================\n\n");

    launch_and_patch_game();
    voice_start();

    SOCKET s_disc[NUM_DISC_PORTS];
    /* Host: never bind discovery ports; the game server owns 8888 and must be free to create a session */
    for (int i = 0; i < NUM_DISC_PORTS; i++) s_disc[i] = INVALID_SOCKET;

    SOCKET s_host_query = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_host_query == INVALID_SOCKET) fail("Could not create Steam host discovery socket.");
    configure_udp_socket(s_host_query);

    static unsigned char buf[65536];
    DWORD last_patch_check = 0;
    DWORD last_voice_tick = 0;
    DWORD game_start_time = 0;

    printf("[STATUS] Steam SDR & Proximity Voice Active. Waiting for partner... (Press ESC to stop)\n\n");

    while (g_running) {
        DWORD now = GetTickCount();

        if (now - last_voice_tick >= 40) {
            last_voice_tick = now;
            voice_tick_game();
        }

        if (!g_no_launch) {
            bool game_alive = is_game_running();
            if (game_alive && !game_start_time) game_start_time = now;
            if (game_start_time && now - game_start_time > 20000 && !game_alive) {
                printf("[GAME] Game process terminated. Ending session...\n");
                break;
            }
        }

        if (now - last_patch_check > 1000) {
            last_patch_check = now;
            patch_running_games();
        }
        pump_steam(host_steam_cb);

        void *msgs[64];
        int n;

        /* Receive Game Packets from Steam Partner */
        if ((n = S.ReceiveMessages(g_msgs, CH_GAME, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                u64 sid = msg_sender(msgs[i]);
                Peer *p = find_peer(sid);
                if (!p) p = add_peer(sid);
                unsigned char *data = *(unsigned char **)msgs[i];
                int size = *(int *)((char *)msgs[i] + 4);
                if (p && size > 3) {
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    uint16_t target_port = ntohs(*(uint16_t *)data);
                    int conn_id = data[2];
                    int host_port = (target_port == 7776) ? 7776 : 7777;
                    Conn *c = peer_game_conn(p, conn_id, host_port);
                    p->last_heard = GetTickCount();
                    if (c) {
                        c->last = p->last_heard;
                        send(c->s, (char *)data + 3, size - 3, 0);
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Control Packets */
        if ((n = S.ReceiveMessages(g_msgs, CH_CTRL, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                Peer *p = find_peer(msg_sender(msgs[i]));
                if (p) {
                    p->last_heard = GetTickCount();
                    int sz = *(int *)((char *)msgs[i] + 4);
                    char *cdata = *(char **)msgs[i];
                    if (sz >= 1 && cdata[0] == 'B') drop_peer(p, "player exited");
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Voice Packets from Steam Partner */
        if ((n = S.ReceiveMessages(g_msgs, CH_VOICE, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                u64 sid = msg_sender(msgs[i]);
                Peer *p = find_peer(sid);
                if (!p) p = add_peer(sid);
                if (p) p->last_heard = GetTickCount();
                unsigned char *data = *(unsigned char **)msgs[i];
                int size = *(int *)((char *)msgs[i] + 4);
                if (is_voice_channel_active()) {
                    voice_push_incoming(data, size);
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Discovery Queries from Steam Partner */
        if ((n = S.ReceiveMessages(g_msgs, CH_DISC, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                u64 sid = msg_sender(msgs[i]);
                Peer *p = find_peer(sid);
                if (!p) p = add_peer(sid);
                unsigned char *data = *(unsigned char **)msgs[i];
                int size = *(int *)((char *)msgs[i] + 4);
                if (p && size >= 2) {
                    uint16_t target_port = ntohs(*(uint16_t *)data);
                    if (!is_allowed_disc_port(target_port)) target_port = 8888;
                    unsigned char *disc_data = data + 2;
                    int disc_len = size - 2;

                    struct sockaddr_in target;
                    memset(&target, 0, sizeof(target));
                    target.sin_family = AF_INET;
                    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    target.sin_port = htons(target_port);
                    sendto(s_host_query, (char *)disc_data, disc_len, 0, (struct sockaddr *)&target, sizeof(target));

                    if (target_port != 8888) {
                        target.sin_port = htons(8888);
                        sendto(s_host_query, (char *)disc_data, disc_len, 0, (struct sockaddr *)&target, sizeof(target));
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Forward discovery responses back to Steam Partner */
        int got_query;
        for (int drain = 0; drain < MAX_RELAY_DRAIN &&
             (got_query = recvfrom(s_host_query, (char *)buf + 2, sizeof(buf) - 2, 0, NULL, NULL)) > 0; drain++) {
            scrub_beacon_ip(buf + 2, got_query);
            *(uint16_t *)buf = htons(8888);
            for (int i = 0; i < MAX_PEERS; i++) {
                if (g_peers[i].sid) steam_send(g_peers[i].sid, buf, got_query + 2, SEND_UNRELIABLE_NO_NAGLE, CH_DISC);
            }
        }

        /* Forward discovery beacons from listener sockets */
        for (int i = 0; i < NUM_DISC_PORTS; i++) {
            if (s_disc[i] == INVALID_SOCKET) continue;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got_query = recvfrom(s_disc[i], (char *)buf + 2, sizeof(buf) - 2, 0, NULL, NULL)) > 0; drain++) {
                scrub_beacon_ip(buf + 2, got_query);
                *(uint16_t *)buf = htons((u_short)DISC_PORTS[i]);
                for (int p_idx = 0; p_idx < MAX_PEERS; p_idx++) {
                    if (g_peers[p_idx].sid) steam_send(g_peers[p_idx].sid, buf, got_query + 2, SEND_UNRELIABLE_NO_NAGLE, CH_DISC);
                }
            }
        }

        /* Forward game replies from local host game */
        now = GetTickCount();
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &g_peers[i];
            if (!p->sid) continue;
            for (int k = 0; k < MAX_CONNS; k++) {
                Conn *c = &p->conns[k];
                if (!c->s) continue;
                int got;
                for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                     (got = recv(c->s, (char *)buf + 3, sizeof(buf) - 3, 0)) > 0; drain++) {
                    *(uint16_t *)buf = htons((u_short)c->port);
                    buf[2] = (unsigned char)c->id;
                    c->last = now;
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    steam_send(p->sid, buf, got + 3, SEND_UNRELIABLE_NO_NAGLE, CH_GAME);
                }
                /* Keep game sockets through long loading gaps;
                 * drop_peer closes them after the session heartbeat expires. */
            }
            if (now - p->last_ping > 1000) { p->last_ping = now; steam_send(p->sid, "K", 1, SEND_RELIABLE, CH_CTRL); }
            if (now - p->last_heard > 8000u) steam_restart_peer(p, "heartbeat silence");
            if (now - p->last_heard > 60000) {
                scct_log("[STEAM] Disconnecting peer %llu: heartbeat timeout (%lu ms without inbound traffic)",
                         p->sid, now - p->last_heard);
                drop_peer(p, "heartbeat timeout");
            }
        }

        /* Forward outgoing voice frames to Steam Partners */
        int vlen;
        for (int voice_frames = 0; voice_frames < 2 &&
             (vlen = voice_pop_outgoing(buf, sizeof(buf))) > 0; voice_frames++) {
            if (is_voice_channel_active()) {
                for (int i = 0; i < MAX_PEERS; i++) {
                    if (g_peers[i].sid) steam_send(g_peers[i].sid, buf, vlen, SEND_UNRELIABLE_NO_NAGLE, CH_VOICE);
                }
            }
        }

        if (_kbhit() && _getch() == 27) break;
        Sleep(1);
    }

    voice_stop();
    closesocket(s_host_query);
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        if (s_disc[i] != INVALID_SOCKET) closesocket(s_disc[i]);
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peers[i].sid) { steam_send(g_peers[i].sid, "B", 1, SEND_RELIABLE, CH_CTRL); drop_peer(&g_peers[i], "host closed"); }
    }
    S.LeaveLobby(g_mm, g_lobby);
    return 0;
}

static u64 g_join_lobby = 0;
static bool g_join_failed = false;
static volatile LONG g_host_left = 0;

static void client_steam_cb(int id, void *data)
{
    switch (id) {
    case CB_LOBBY_JOIN_REQUESTED:
        if (!g_join_lobby) g_join_lobby = ((GameLobbyJoinRequested *)data)->lobby;
        break;
    case CB_RICH_PRESENCE_JOIN: {
        const char *s = ((GameRichPresenceJoinRequested *)data)->connect;
        const char *p = strstr(s, "+connect_lobby");
        if (p && !g_join_lobby) g_join_lobby = strtoull(p + 14, NULL, 10);
        break;
    }
    case CB_LOBBY_ENTER: {
        LobbyEnter *e = (LobbyEnter *)data;
        if (e->lobby != g_join_lobby) break;
        if (e->response != 1) { g_join_failed = true; break; }
        g_host_sid = S.GetLobbyOwner(g_mm, e->lobby);
        break;
    }
    case CB_SESSION_REQUEST: {
        u64 sid = S.IdentGet(data);
        if (sid == g_host_sid) steam_accept(sid);
        break;
    }
    case CB_SESSION_FAILED: {
        u64 sid = S.IdentGet((char *)data);
        SteamSessionFailurePrefix failure;
        memcpy(&failure, data, sizeof(failure));
        if (sid == g_host_sid) {
            scct_log("[STEAM] Host relay session failed (state=%ld/%s end_reason=%ld/%s); scheduling client recovery probe",
                     (long)failure.state, steam_failure_state_name(failure.state),
                     (long)failure.end_reason, steam_end_reason_name(failure.end_reason));
            g_client_recovery_due = GetTickCount() + 2500u;
        }
        break;
    }
    case CB_LOBBY_CHAT_UPDATE: {
        LobbyChatUpdate *u = (LobbyChatUpdate *)data;
        if (u->lobby == g_join_lobby && u->user_changed == g_host_sid &&
            (u->state & (0x2 | 0x8 | 0x10))) {
            InterlockedExchange(&g_host_left, 1);
            scct_log("[STEAM] Host left or was removed from the lobby (state=0x%X)", u->state);
        }
        break;
    }
    }
}

static int run_steam_client(u64 target_lobby)
{
    g_join_lobby = target_lobby;
    if (!g_join_lobby) {
        printf("\nEnter Host Steam Lobby ID: ");
        char line[128];
        if (fgets(line, sizeof(line), stdin)) g_join_lobby = strtoull(line, NULL, 10);
    }
    if (!g_join_lobby) fail("No Lobby ID provided.");

    printf("[CLIENT] Joining Steam Lobby %llu...\n", (unsigned long long)g_join_lobby);
    S.JoinLobby(g_mm, g_join_lobby);

    DWORD t0 = GetTickCount();
    while (!g_host_sid && !g_join_failed && GetTickCount() - t0 < 15000) {
        pump_steam(client_steam_cb);
        Sleep(20);
    }
    if (!g_host_sid) fail("Could not connect to Steam Lobby. Make sure host is running!");

    steam_send(g_host_sid, "P", 1, SEND_RELIABLE, CH_CTRL);

    SOCKET s_disc[NUM_DISC_PORTS];
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        s_disc[i] = udp_bind(DISC_PORTS[i], true, true);
    }

    SOCKET s_game[NUM_GAME_PORTS];
    for (int j = 0; j < NUM_GAME_PORTS; j++) {
        s_game[j] = udp_bind(GAME_PORTS[j], false, true);
    }

    SOCKET s_inject = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_inject == INVALID_SOCKET) fail("Could not create Steam discovery injection socket.");
    configure_udp_socket(s_inject);

    printf("======================================================================\n");
    printf("  CONNECTED TO HOST OVER STEAM DATAGRAM RELAY (P2P)\n");
    printf("======================================================================\n");
    printf("Instructions for Client:\n");
    printf("  1. In Splinter Cell Chaos Theory, go to:\n");
    printf("       -> COOP  ->  ONLINE (or LAN)  ->  JOIN GAME\n");
    printf("  2. The host's session will appear in the Games List automatically!\n");
    printf("  3. Select it and click Ready to launch the mission together!\n");
    printf("  4. Keep this console window open while playing! (Press ESC to stop)\n");
    printf("======================================================================\n\n");

    launch_and_patch_game();
    voice_start();

    struct sockaddr_in client_query_from;
    bool has_query_client = false;
    struct sockaddr_in game_clients[MAX_CONNS];
    int game_ports[MAX_CONNS];
    unsigned char game_ids[MAX_CONNS];
    int game_conns_n = 0;
    unsigned char next_conn_id = 0;

    static unsigned char buf[65536];
    DWORD last_patch_check = 0;
    DWORD last_voice_tick = 0;
    DWORD last_steam_ping = 0;
    DWORD last_host_heard = GetTickCount();
    bool host_silence_logged = false;
    DWORD last_session_retry = 0;
    DWORD game_start_time = 0;

    printf("[STATUS] Steam SDR & Proximity Voice Active. Waiting for session discovery... (Press ESC to stop)\n\n");

    while (g_running) {
        DWORD now = GetTickCount();

        if (now - last_voice_tick >= 40) {
            last_voice_tick = now;
            voice_tick_game();
        }

        if (now - last_steam_ping > 1500) {
            last_steam_ping = now;
            steam_send(g_host_sid, "P", 1, SEND_UNRELIABLE_NO_NAGLE, CH_CTRL);
        }
        if (now - last_host_heard > 15000 && !host_silence_logged) {
            host_silence_logged = true;
            scct_log("[STEAM] No host traffic for %lu ms; continuing relay retries", now - last_host_heard);
        }
        if (now - last_host_heard > 60000 && now - last_session_retry > 10000) {
            last_session_retry = now;
            scct_log("[STEAM] Host heartbeat timeout after %lu ms; sending a normal liveness probe",
                     now - last_host_heard);
            steam_send(g_host_sid, "P", 1, SEND_RELIABLE, CH_CTRL);
        }

        if (!g_no_launch) {
            bool game_alive = is_game_running();
            if (game_alive && !game_start_time) game_start_time = now;
            if (game_start_time && now - game_start_time > 20000 && !game_alive) {
                printf("[GAME] Game process terminated. Ending session...\n");
                break;
            }
        }

        if (now - last_patch_check > 1000) {
            last_patch_check = now;
            patch_running_games();
        }
        pump_steam(client_steam_cb);
        if (g_client_recovery_due && (LONG)(now - g_client_recovery_due) >= 0) {
            g_client_recovery_due = 0;
            steam_restart_client(g_host_sid, "client failure callback");
        }
        if (InterlockedCompareExchange(&g_host_left, 0, 0)) {
            printf("[STEAM] Host left the lobby. Ending this relay session.\n");
            break;
        }

        /* Forward broadcast queries from local client game */
        for (int i = 0; i < NUM_DISC_PORTS; i++) {
            if (s_disc[i] == INVALID_SOCKET) continue;
            struct sockaddr_in from;
            int fl = sizeof(from);
            int got;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got = recvfrom(s_disc[i], (char *)buf + 2, sizeof(buf) - 2, 0, (struct sockaddr *)&from, &fl)) > 0; drain++) {
                client_query_from = from;
                has_query_client = true;
                *(uint16_t *)buf = htons((u_short)DISC_PORTS[i]);
                steam_send(g_host_sid, buf, got + 2, SEND_UNRELIABLE_NO_NAGLE, CH_DISC);
                fl = sizeof(from);
            }
        }

        /* Forward game packets from local client game */
        for (int j = 0; j < NUM_GAME_PORTS; j++) {
            if (s_game[j] == INVALID_SOCKET) continue;
            struct sockaddr_in from_game;
            int fl_g = sizeof(from_game);
            int got_game;
            for (int drain = 0; drain < MAX_RELAY_DRAIN &&
                 (got_game = recvfrom(s_game[j], (char *)buf + 3, sizeof(buf) - 3, 0, (struct sockaddr *)&from_game, &fl_g)) > 0; drain++) {
                int k;
                for (k = 0; k < game_conns_n; k++) {
                    if (game_clients[k].sin_port == from_game.sin_port &&
                        game_clients[k].sin_addr.s_addr == from_game.sin_addr.s_addr &&
                        game_ports[k] == GAME_PORTS[j]) break;
                }
                if (k == game_conns_n) {
                    if (game_conns_n < MAX_CONNS) game_conns_n++;
                    else k = 0;
                    game_clients[k] = from_game;
                    game_ports[k] = GAME_PORTS[j];
                    game_ids[k] = next_conn_id++;
                }
                *(uint16_t *)buf = htons((u_short)GAME_PORTS[j]);
                buf[2] = game_ids[k];
                InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                steam_send(g_host_sid, buf, got_game + 3, SEND_UNRELIABLE_NO_NAGLE, CH_GAME);
                fl_g = sizeof(from_game);
            }
        }

        /* Receive Game Packets from Host */
        void *msgs[64];
        int n;
        if ((n = S.ReceiveMessages(g_msgs, CH_GAME, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                unsigned char *data = *(unsigned char **)msgs[i];
                int size = *(int *)((char *)msgs[i] + 4);
                if (msg_sender(msgs[i]) == g_host_sid && size > 3) {
                    last_host_heard = now;
                    host_silence_logged = false;
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    uint8_t cid = data[2];
                    unsigned char *game_data = data + 3;
                    int game_len = size - 3;

                    for (int k = 0; k < game_conns_n; k++) {
                        if (game_ids[k] == cid) {
                            for (int j = 0; j < NUM_GAME_PORTS; j++) {
                                if (GAME_PORTS[j] == game_ports[k] && s_game[j] != INVALID_SOCKET) {
                                    sendto(s_game[j], (char *)game_data, game_len, 0,
                                           (struct sockaddr *)&game_clients[k], sizeof(game_clients[k]));
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Control Messages from Host */
        if ((n = S.ReceiveMessages(g_msgs, CH_CTRL, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                if (msg_sender(msgs[i]) == g_host_sid) {
                    last_host_heard = now;
                    host_silence_logged = false;
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    int sz = *(int *)((char *)msgs[i] + 4);
                    char *cdata = *(char **)msgs[i];
                    if (sz == 1 && cdata[0] == 'K') {
                        /* The host heartbeat confirms that its recovery probe got through. */
                        g_client_recovery_due = 0;
                        steam_send(g_host_sid, "P", 1, SEND_UNRELIABLE_NO_NAGLE, CH_CTRL);
                    } else if (sz >= 1 && cdata[0] == 'B') {
                        InterlockedExchange(&g_host_left, 1);
                        scct_log("[STEAM] Host sent an orderly session close.");
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Discovery Responses from Host */
        if ((n = S.ReceiveMessages(g_msgs, CH_DISC, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                unsigned char *data = *(unsigned char **)msgs[i];
                int size = *(int *)((char *)msgs[i] + 4);
                if (msg_sender(msgs[i]) == g_host_sid && size > 2) {
                    last_host_heard = now;
                    host_silence_logged = false;
                    uint16_t disc_port = ntohs(*(uint16_t *)data);
                    unsigned char *disc_payload = data + 2;
                    int disc_len = size - 2;

                    /* Rewrite server IP address in beacon payload to loopback (127.0.0.1) */
                    if (disc_len >= 12 && disc_payload[0] == 8) {
                        *(uint32_t *)(disc_payload + 8) = htonl(INADDR_LOOPBACK);
                    } else if (disc_len >= 32 && disc_payload[0] == 0x45) {
                        *(uint32_t *)(disc_payload + 28) = htonl(INADDR_LOOPBACK);
                    }

                    struct sockaddr_in to_8888;
                    memset(&to_8888, 0, sizeof(to_8888));
                    to_8888.sin_family = AF_INET;
                    to_8888.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    to_8888.sin_port = htons(8888);
                    sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));

                    to_8888.sin_port = htons(8889);
                    sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));

                    if (disc_port != 8888 && disc_port != 8889) {
                        to_8888.sin_port = htons(disc_port);
                        sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&to_8888, sizeof(to_8888));
                    }

                    if (has_query_client) {
                        sendto(s_inject, (char *)disc_payload, disc_len, 0, (struct sockaddr *)&client_query_from, sizeof(client_query_from));
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Receive Voice Packets from Host */
        if ((n = S.ReceiveMessages(g_msgs, CH_VOICE, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                if (msg_sender(msgs[i]) == g_host_sid) {
                    last_host_heard = now;
                    host_silence_logged = false;
                    InterlockedExchange((volatile LONG *)&g_last_game_pkt_tick, (LONG)now);
                    unsigned char *data = *(unsigned char **)msgs[i];
                    int size = *(int *)((char *)msgs[i] + 4);
                    if (is_voice_channel_active()) {
                        voice_push_incoming(data, size);
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }

        /* Forward outgoing voice frames to Host */
        int vlen;
        for (int voice_frames = 0; voice_frames < 2 &&
             (vlen = voice_pop_outgoing(buf, sizeof(buf))) > 0; voice_frames++) {
            if (is_voice_channel_active()) {
                steam_send(g_host_sid, buf, vlen, SEND_UNRELIABLE_NO_NAGLE, CH_VOICE);
            }
        }

        if (_kbhit() && _getch() == 27) break;
        Sleep(1);
    }

    voice_stop();
    for (int i = 0; i < NUM_DISC_PORTS; i++) {
        if (s_disc[i] != INVALID_SOCKET) closesocket(s_disc[i]);
    }
    for (int j = 0; j < NUM_GAME_PORTS; j++) {
        if (s_game[j] != INVALID_SOCKET) closesocket(s_game[j]);
    }
    closesocket(s_inject);
    steam_send(g_host_sid, "B", 1, SEND_RELIABLE, CH_CTRL);
    S.LeaveLobby(g_mm, g_join_lobby);
    return 0;
}

/* ---------------------------------------------------------------- WORLDWIDE SERVER BROWSER */

static bool g_match_list_received = false;
static uint32_t g_lobby_count = 0;

static void browse_steam_cb(int id, void *data)
{
    if (id == CB_LOBBY_MATCH_LIST) {
        LobbyMatchList_t *l = (LobbyMatchList_t *)data;
        g_lobby_count = l->m_nLobbiesMatching;
        g_match_list_received = true;
    }
}

static int browse_and_join_steam_lobbies(void)
{
    if (!g_steam_available) {
        printf("\n[ERROR] Steam is not running. Please start Steam in the background to use Worldwide Matchmaking.\n");
        printf("Press any key to return to main menu...\n");
        _getch();
        return 0;
    }

    for (;;) {
        system("cls");
        printf("======================================================================\n");
        printf("    WORLDWIDE PUBLIC SERVER BROWSER (SPLINTER CELL: CHAOS THEORY)\n");
        printf("======================================================================\n");
        printf("Searching worldwide for active co-op sessions (Zero port-forwarding)...\n\n");

        g_match_list_received = false;
        g_lobby_count = 0;

        S.AddRequestLobbyListDistanceFilter(g_mm, 3 /* k_ELobbyDistanceFilterWorldwide */);
        S.AddRequestLobbyListStringFilter(g_mm, "scct_coop", "1", 0 /* k_ELobbyComparisonEqual */);
        if (S.AddRequestLobbyListResultCountFilter) S.AddRequestLobbyListResultCountFilter(g_mm, 50);
        S.RequestLobbyList(g_mm);

        DWORD t0 = GetTickCount();
        while (!g_match_list_received && GetTickCount() - t0 < 6000) {
            pump_steam(browse_steam_cb);
            Sleep(25);
        }

        if (!g_match_list_received || g_lobby_count == 0) {
            printf("[INFO] No public games currently found online worldwide.\n\n");
            printf("What would you like to do?\n");
            printf("  [1] Refresh search\n");
            printf("  [2] Host a Public Game yourself (anyone in the world can join!)\n");
            printf("  [ESC] Return to main menu\n\n");
            printf("Select option: ");
            int ch = _getch();
            if (ch == 27) return 0;
            if (ch == '1') continue;
            if (ch == '2') return run_steam_host();
            continue;
        }

        printf("Found %u active public game(s) worldwide:\n", g_lobby_count);
        printf("----------------------------------------------------------------------\n");

        u64 lobbies[32];
        uint32_t display_count = (g_lobby_count > 30) ? 30 : g_lobby_count;

        for (uint32_t i = 0; i < display_count; i++) {
            u64 lid = S.GetLobbyByIndex(g_mm, (int)i);
            lobbies[i] = lid;
            const char *hname = S.GetLobbyData(g_mm, lid, "host");
            const char *ver = S.GetLobbyData(g_mm, lid, "version");
            int num_players = S.GetNumLobbyMembers(g_mm, lid);
            if (!hname || !*hname) {
                if (S.RequestLobbyData) S.RequestLobbyData(g_mm, lid);
                hname = "Anonymous Agent";
            }
            if (!ver || !*ver) ver = "1.x";

            printf("  [%u] Host: %-22s | Players: %d/2 | Mod: %s\n",
                   i + 1, hname, num_players, ver);
        }

        printf("----------------------------------------------------------------------\n");
        printf("Enter game number (1-%u) or Lobby ID, [R] to Refresh, or [ESC] Back: ", display_count);

        char input[64] = "";
        if (fgets(input, sizeof(input), stdin)) {
            input[strcspn(input, "\r\n")] = 0;
            if (input[0] == 27) return 0;
            if (input[0] == 'r' || input[0] == 'R') continue;
            int sel = atoi(input);
            if (sel >= 1 && sel <= (int)display_count) {
                u64 target = lobbies[sel - 1];
                printf("\nJoining Game #%d (Lobby %llu)...\n", sel, (unsigned long long)target);
                return run_steam_client(target);
            } else if (strlen(input) > 10) {
                u64 target = strtoull(input, NULL, 10);
                if (target) {
                    printf("\nJoining Lobby %llu...\n", (unsigned long long)target);
                    return run_steam_client(target);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- IN-GAME MENU TEXT: "COOP LAN" -> "COOP ONLINE"
 * The menu captions are plain INI text inside System\dynamic-pc.umd (one block per language).
 * For every language block we make the LAN captions read exactly like the ONLINE captions.
 * The block keeps its exact byte size (surplus blank lines are removed / spaces pad the rest),
 * so no offsets in the archive move. Original bytes are saved so the change can be reverted
 * with:  SCCT_OnlineCoop.exe --restore-menu
 */

static const unsigned char *mem_find(const unsigned char *h, size_t hl, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) {
        const unsigned char *p = memchr(h + i, (unsigned char)needle[0], hl - i - nl + 1);
        if (!p) return NULL;
        if (!memcmp(p, needle, nl)) return p;
        i = (size_t)(p - h);
    }
    return NULL;
}

typedef struct { const unsigned char *p; int n; bool blank, drop; unsigned char txt[200]; int tn; bool edited; } MLine;

static bool menu_patch_region(unsigned char *region, int n)
{
    /* detect line terminator used by this block */
    int i = 0;
    while (i < n && region[i] != '\r' && region[i] != '\n') i++;
    int e0 = i;
    while (i < n && (region[i] == '\r' || region[i] == '\n')) i++;
    int eol = i - e0;
    if (eol < 1 || eol > 3) return false;
    unsigned char eolb[4];
    memcpy(eolb, region + e0, eol);

    static MLine L[300];
    int cnt = 0, pos = 0;
    while (pos <= n && cnt < 300) {
        int s = pos;
        while (pos < n && !(pos + eol <= n && memcmp(region + pos, eolb, eol) == 0)) pos++;
        if (pos >= n) { L[cnt].p = region + s; L[cnt].n = n - s; L[cnt].blank = (n - s == 0); cnt++; break; }
        L[cnt].p = region + s; L[cnt].n = pos - s; L[cnt].blank = (pos == s); cnt++;
        pos += eol;
    }
    if (cnt < 3 || cnt >= 300) return false;

    static const char *lan[3] = { "MsgBoxHeaderLan=", "MainTitleLan=", "LanCaption=" };
    static const char *onl[3] = { "MsgBoxHeaderOnline=", "MainTitleOnline=", "OnlineCaption=" };
    int changed = 0, last_edit = -1;
    for (int k = 0; k < 3; k++) {
        int li = -1, oi = -1;
        for (int j = 0; j < cnt; j++) {
            if (!L[j].blank && L[j].n >= (int)strlen(lan[k]) && !memcmp(L[j].p, lan[k], strlen(lan[k]))) li = j;
            if (!L[j].blank && L[j].n >= (int)strlen(onl[k]) && !memcmp(L[j].p, onl[k], strlen(onl[k]))) oi = j;
        }
        if (li < 0 || oi < 0) return false;
        const unsigned char *val = L[oi].p + strlen(onl[k]);
        int vl = L[oi].n - (int)strlen(onl[k]);
        int kl = (int)strlen(lan[k]);
        if (kl + vl > (int)sizeof(L[li].txt)) return false;
        memcpy(L[li].txt, lan[k], kl);
        memcpy(L[li].txt + kl, val, vl);
        L[li].tn = kl + vl;
        if (L[li].tn != L[li].n || memcmp(L[li].txt, L[li].p, L[li].n)) { L[li].edited = true; changed++; }
        if (li > last_edit) last_edit = li;
    }
    if (!changed) return false;

    long total = 0;
    for (int j = 0; j < cnt; j++) total += L[j].edited ? L[j].tn : L[j].n;
    total += (long)eol * (cnt - 1);
    long diff = total - n;
    int pad = 0;
    if (diff > 0) {
        int need = (int)((diff + eol - 1) / eol);
        int dropped = 0;
        for (int j = 1; j < cnt - 1 && dropped < need; j++) {
            if (L[j].blank && !L[j].edited) { L[j].drop = true; dropped++; }
        }
        if (dropped < need) return false;
        pad = dropped * eol - (int)diff;
    } else {
        pad = (int)(-diff);
    }

    unsigned char *out = (unsigned char *)malloc(n + 16);
    if (!out) return false;
    int o = 0;
    bool first = true;
    for (int j = 0; j < cnt; j++) {
        if (L[j].drop) continue;
        if (!first) { memcpy(out + o, eolb, eol); o += eol; }
        first = false;
        if (L[j].edited) { memcpy(out + o, L[j].txt, L[j].tn); o += L[j].tn; }
        else { memcpy(out + o, L[j].p, L[j].n); o += L[j].n; }
        if (j == last_edit && pad > 0) { memset(out + o, ' ', pad); o += pad; }
    }
    bool ok = (o == n);
    if (ok) memcpy(region, out, n);
    free(out);
    return ok;
}

static int patch_menu_text(bool restore)
{
    char path[MAX_PATH], bakp[MAX_PATH];
    if (!g_game_dir[0]) return 0;
    snprintf(path, sizeof(path), "%s\\dynamic-pc.umd", g_game_dir);
    snprintf(bakp, sizeof(bakp), "%s\\dynamic-pc.umd.scct_menu.bak", g_game_dir);

    if (restore) {
        FILE *b = fopen(bakp, "rb");
        FILE *f = fopen(path, "r+b");
        if (!b || !f) { if (b) fclose(b); if (f) fclose(f); printf("[MENU] Nothing to restore (or game is running).\n"); return 0; }
        uint32_t off, len;
        int n = 0;
        static unsigned char tmp[16384];
        while (fread(&off, 4, 1, b) == 1 && fread(&len, 4, 1, b) == 1 && len <= sizeof(tmp) && fread(tmp, 1, len, b) == len) {
            fseek(f, (long)off, SEEK_SET);
            fwrite(tmp, 1, len, f);
            n++;
        }
        fclose(b); fclose(f);
        DeleteFileA(bakp);
        printf("[MENU] Restored original menu text (%d blocks).\n", n);
        return n;
    }

    FILE *f = fopen(path, "r+b");
    if (!f) return 0;
    const size_t WIN = 48u << 20;
    unsigned char *buf = (unsigned char *)malloc(WIN);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, WIN, f);
    int patched = 0;
    size_t pos = 0;
    FILE *b = NULL;
    for (;;) {
        const unsigned char *hit = mem_find(buf + pos, got - pos, "MsgBoxHeaderLan=");
        if (!hit) break;
        size_t h = (size_t)(hit - buf);
        pos = h + 16;
        size_t from = h > 200 ? h - 200 : 0;
        const unsigned char *rs = mem_find(buf + from, h - from + 20, "MsgBoxHeaderOnline=");
        const unsigned char *lc = mem_find(buf + h, got - h > 9000 ? 9000 : got - h, "LanCaption=");
        if (!rs || !lc) continue;
        const unsigned char *oc = mem_find(lc, got - (size_t)(lc - buf) > 600 ? 600 : got - (size_t)(lc - buf), "OnlineCaption=");
        if (!oc) continue;
        const unsigned char *re = oc;
        while (re < buf + got && *re != '\r' && *re != '\n') re++;
        size_t rstart = (size_t)(rs - buf);
        int n = (int)(re - rs);
        if (n <= 0 || n > 12000) continue;
        unsigned char orig[12000];
        memcpy(orig, rs, n);
        if (menu_patch_region(buf + rstart, n)) {
            if (!b) b = fopen(bakp, "ab");
            if (b) {
                uint32_t off = (uint32_t)rstart, len = (uint32_t)n;
                fwrite(&off, 4, 1, b); fwrite(&len, 4, 1, b); fwrite(orig, 1, n, b);
            }
            fseek(f, (long)rstart, SEEK_SET);
            fwrite(buf + rstart, 1, n, f);
            patched++;
        }
    }
    if (b) fclose(b);
    fclose(f);
    free(buf);
    if (patched) printf("[MENU] 'COOP LAN' menu text now reads 'COOP ONLINE' (%d language blocks).\n", patched);
    return patched;
}

/* ---------------------------------------------------------------- CO-OP SAVER
 * Native port of savvamadar/Splinter-Cell-Chaos-Theory-Co-op-Saver.
 * Co-op only keeps one CacheCoop quicksave, which is overwritten when a session
 * starts. At launch we back up the existing quicksave (host.sav / client.sav).
 * After you quick save once in the new session, press F10 (hear the ding) to put
 * the backed-up save back, then quick load to resume the old progress.
 */

static char g_sav_dir[MAX_PATH];
static char g_sav_status[160] = "Co-op Saver: idle";

static void sav_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s\\%s", g_sav_dir, name);
}

static bool sav_exists(const char *name)
{
    char p[MAX_PATH];
    sav_path(p, sizeof(p), name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static bool sav_copy(const char *from, const char *to, bool overwrite)
{
    char a[MAX_PATH], b[MAX_PATH];
    sav_path(a, sizeof(a), from);
    sav_path(b, sizeof(b), to);
    return CopyFileA(a, b, overwrite ? FALSE : TRUE) != 0;
}

static void sav_delete(const char *name)
{
    char p[MAX_PATH];
    sav_path(p, sizeof(p), name);
    DeleteFileA(p);
}

/* Returns 1 if host role, 0 if client role. Sets *valid when a save is present. */
static void saver_prepare(bool *is_host, bool *valid)
{
    const char *host_f = "CacheCoopQuickSaveHost.sav", *cli_f = "CacheCoopQuickSaveClient.sav";
    const char *host_b = "host.sav", *cli_b = "client.sav";
    *is_host = false;
    *valid = false;

    /* A leftover backup from a previous session is restored first */
    if (sav_exists(host_b)) { sav_copy(host_b, host_f, true); sav_delete(host_b); }
    else if (sav_exists(cli_b)) { sav_copy(cli_b, cli_f, true); sav_delete(cli_b); }

    if (sav_exists(host_f)) {
        *is_host = true; *valid = true;
        if (sav_copy(host_f, host_b, true)) snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: host save backed up. Quick save in-game, then press F10.");
        else { *valid = false; snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: could not back up host save."); }
    } else if (sav_exists(cli_f)) {
        *valid = true;
        if (sav_copy(cli_f, cli_b, true)) snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: client save backed up. Quick save in-game, then press F10.");
        else { *valid = false; snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: could not back up client save."); }
    } else {
        snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: no co-op quick save found yet (quick save once first).");
    }
}

static DWORD WINAPI saver_thread(LPVOID unused)
{
    (void)unused;
    const char *lad = getenv("LOCALAPPDATA");
    if (!lad) return 0;
    snprintf(g_sav_dir, sizeof(g_sav_dir), "%s\\Ubisoft\\Tom Clancy's Splinter Cell Chaos Theory", lad);

    bool is_host, valid;
    saver_prepare(&is_host, &valid);
    printf("[SAVER] %s\n", g_sav_status);
    if (!valid) return 0;

    while (g_running) {
        if ((GetAsyncKeyState(VK_F10) & 0x8000) || (getenv("SCCT_SAVER_TEST") && sav_exists("f10.trigger"))) {
            const char *f = is_host ? "CacheCoopQuickSaveHost.sav" : "CacheCoopQuickSaveClient.sav";
            const char *b = is_host ? "host.sav" : "client.sav";
            if (sav_exists(b) && sav_copy(b, f, true)) {
                sav_delete(b);
                Beep(988, 120); Beep(1319, 250);
                snprintf(g_sav_status, sizeof(g_sav_status), "Co-op Saver: old save restored - now QUICK LOAD in-game!");
                printf("[SAVER] %s\n", g_sav_status);
                return 0;
            }
            Sleep(300);
        }
        Sleep(20);
    }
    return 0;
}

static void saver_start(void)
{
    HANDLE h = CreateThread(NULL, 0, saver_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

/* ---------------------------------------------------------------- GRAPHICAL SERVER BROWSER */

enum { ID_LIST = 100, ID_REFRESH, ID_JOIN, ID_HOSTPUB, ID_HOSTROOM, ID_JOINCODE, ID_CODE, ID_MICCOMBO, ID_MICBAR, ID_MICPCT, ID_MICTEST, ID_VOICEMODE, ID_STATUS, ID_SAVER, ID_TIMER = 1 };
typedef enum { ACT_NONE, ACT_JOIN_STEAM, ACT_HOST_STEAM, ACT_HOST_ROOM, ACT_JOIN_ROOM } GuiAct;

static GuiAct g_act = ACT_NONE;
static u64 g_act_lobby = 0;
static char g_act_code[128] = "";
static u64 g_gui_lobbies[64];
static HWND g_hList, g_hStatus, g_hCode, g_hSaver, g_hMicCombo, g_hMicBar, g_hMicPct, g_hMicTest, g_hVoiceMode;
static int g_gui_ticks = 0;

static void gui_populate_mics(void)
{
    SendMessageA(g_hMicCombo, CB_RESETCONTENT, 0, 0);
    WAVEINCAPSA def_caps;
    char label[128];
    if (waveInGetDevCapsA(WAVE_MAPPER, &def_caps, sizeof(def_caps)) == MMSYSERR_NOERROR)
        snprintf(label, sizeof(label), "Windows Default (%s)", def_caps.szPname);
    else
        snprintf(label, sizeof(label), "Windows Default Microphone");
    int idx = (int)SendMessageA(g_hMicCombo, CB_ADDSTRING, 0, (LPARAM)label);
    SendMessageA(g_hMicCombo, CB_SETITEMDATA, idx, (LPARAM)WAVE_MAPPER);

    UINT n = waveInGetNumDevs();
    int sel_idx = 0;
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            snprintf(label, sizeof(label), "[%u] %s", i + 1, caps.szPname);
            int row = (int)SendMessageA(g_hMicCombo, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageA(g_hMicCombo, CB_SETITEMDATA, row, (LPARAM)i);
            if ((UINT)g_mic_device_id == i) sel_idx = row;
        }
    }
    SendMessageA(g_hMicCombo, CB_SETCURSEL, sel_idx, 0);
}

static void gui_set_status(const char *s) { SetWindowTextA(g_hStatus, s); }

static void gui_refresh(void)
{
    ListView_DeleteAllItems(g_hList);
    if (!g_steam_available) {
        gui_set_status("Steam is not running: public browser unavailable. Use a Room Code below.");
        return;
    }
    S.AddRequestLobbyListDistanceFilter(g_mm, 3 /* worldwide */);
    S.AddRequestLobbyListStringFilter(g_mm, "scct_coop", "1", 0);
    if (S.AddRequestLobbyListResultCountFilter) S.AddRequestLobbyListResultCountFilter(g_mm, 50);
    S.RequestLobbyList(g_mm);
    gui_set_status("Searching worldwide for games...");
}

static void gui_cb(int id, void *data)
{
    if (id != CB_LOBBY_MATCH_LIST) return;
    LobbyMatchList_t *l = (LobbyMatchList_t *)data;
    uint32_t n = l->m_nLobbiesMatching > 64 ? 64 : l->m_nLobbiesMatching;
    ListView_DeleteAllItems(g_hList);
    for (uint32_t i = 0; i < n; i++) {
        u64 lid = S.GetLobbyByIndex(g_mm, (int)i);
        g_gui_lobbies[i] = lid;
        const char *host = S.GetLobbyData(g_mm, lid, "host");
        const char *ver = S.GetLobbyData(g_mm, lid, "version");
        char players[16];
        snprintf(players, sizeof(players), "%d / 2", S.GetNumLobbyMembers(g_mm, lid));
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)lid);
        if (!host || !*host) host = "Agent";
        if (!ver || !*ver) ver = "-";
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (char *)host;
        int row = (int)SendMessageA(g_hList, LVM_INSERTITEMA, 0, (LPARAM)&it);
        ListView_SetItemText(g_hList, row, 1, players);
        ListView_SetItemText(g_hList, row, 2, (char *)ver);
        ListView_SetItemText(g_hList, row, 3, idbuf);
    }
    char msg[96];
    if (n) snprintf(msg, sizeof(msg), "%u game(s) found. Double-click one to join.", n);
    else snprintf(msg, sizeof(msg), "No public games right now. Press 'Host Public Game' and a friend can find you here!");
    gui_set_status(msg);
}

static void gui_join_selected(HWND wnd)
{
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= 64 || !g_gui_lobbies[sel]) { gui_set_status("Select a game in the list first."); return; }
    g_act = ACT_JOIN_STEAM;
    g_act_lobby = g_gui_lobbies[sel];
    DestroyWindow(wnd);
}

static LRESULT CALLBACK gui_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND h;
        h = CreateWindowExA(0, "STATIC", "SPLINTER CELL: CHAOS THEORY  -  ONLINE CO-OP SERVER BROWSER", WS_CHILD | WS_VISIBLE, 14, 10, 600, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        g_hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  14, 34, 596, 230, wnd, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        SendMessageA(g_hList, WM_SETFONT, (WPARAM)font, TRUE);
        static const char *cols[] = { "Host", "Players", "Version", "Lobby ID" };
        static const int widths[] = { 210, 70, 150, 150 };
        for (int i = 0; i < 4; i++) {
            LVCOLUMNA c;
            memset(&c, 0, sizeof(c));
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (char *)cols[i];
            c.cx = widths[i];
            SendMessageA(g_hList, LVM_INSERTCOLUMNA, i, (LPARAM)&c);
        }
        struct { const char *t; int id; int x, y, w; } btn[] = {
            { "Refresh", ID_REFRESH, 14, 272, 90 },
            { "Join Selected", ID_JOIN, 110, 272, 120 },
            { "Host Public Game", ID_HOSTPUB, 236, 272, 140 },
            { "Host Private Room", ID_HOSTROOM, 382, 272, 140 },
        };
        for (int i = 0; i < 4; i++) {
            h = CreateWindowExA(0, "BUTTON", btn[i].t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, btn[i].x, btn[i].y, btn[i].w, 28, wnd, (HMENU)(INT_PTR)btn[i].id, NULL, NULL);
            SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        }
        h = CreateWindowExA(0, "STATIC", "Join with Room Code / IP:", WS_CHILD | WS_VISIBLE, 14, 314, 150, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        g_hCode = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 164, 311, 220, 22, wnd, (HMENU)(INT_PTR)ID_CODE, NULL, NULL);
        SendMessageA(g_hCode, WM_SETFONT, (WPARAM)font, TRUE);
        h = CreateWindowExA(0, "BUTTON", "Join Room", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 392, 309, 110, 26, wnd, (HMENU)(INT_PTR)ID_JOINCODE, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        h = CreateWindowExA(0, "STATIC", "Voice Chat Mic:", WS_CHILD | WS_VISIBLE, 14, 347, 94, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        g_hMicCombo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      110, 343, 240, 200, wnd, (HMENU)(INT_PTR)ID_MICCOMBO, NULL, NULL);
        SendMessageA(g_hMicCombo, WM_SETFONT, (WPARAM)font, TRUE);
        gui_populate_mics();

        g_hMicBar = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                    356, 345, 100, 18, wnd, (HMENU)(INT_PTR)ID_MICBAR, NULL, NULL);
        SendMessageA(g_hMicBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_hMicPct = CreateWindowExA(0, "STATIC", "0%", WS_CHILD | WS_VISIBLE, 462, 347, 50, 18, wnd, (HMENU)(INT_PTR)ID_MICPCT, NULL, NULL);
        SendMessageA(g_hMicPct, WM_SETFONT, (WPARAM)font, TRUE);
        g_hMicTest = CreateWindowExA(0, "BUTTON", "Test Mic", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     518, 344, 90, 20, wnd, (HMENU)(INT_PTR)ID_MICTEST, NULL, NULL);
        SendMessageA(g_hMicTest, WM_SETFONT, (WPARAM)font, TRUE);

        h = CreateWindowExA(0, "STATIC", "Voice Mode:", WS_CHILD | WS_VISIBLE, 14, 377, 94, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        g_hVoiceMode = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      110, 373, 240, 200, wnd, (HMENU)(INT_PTR)ID_VOICEMODE, NULL, NULL);
        SendMessageA(g_hVoiceMode, WM_SETFONT, (WPARAM)font, TRUE);
        int m0 = (int)SendMessageA(g_hVoiceMode, CB_ADDSTRING, 0, (LPARAM)"Open Mic (Voice Activated)");
        SendMessageA(g_hVoiceMode, CB_SETITEMDATA, m0, (LPARAM)VOICE_MODE_OPEN);
        int m1 = (int)SendMessageA(g_hVoiceMode, CB_ADDSTRING, 0, (LPARAM)"Push-to-Talk (Hold 'V')");
        SendMessageA(g_hVoiceMode, CB_SETITEMDATA, m1, (LPARAM)VOICE_MODE_PTT_V);
        int m2 = (int)SendMessageA(g_hVoiceMode, CB_ADDSTRING, 0, (LPARAM)"Push-to-Talk (Hold 'Caps Lock')");
        SendMessageA(g_hVoiceMode, CB_SETITEMDATA, m2, (LPARAM)VOICE_MODE_PTT_CAPS);
        int m3 = (int)SendMessageA(g_hVoiceMode, CB_ADDSTRING, 0, (LPARAM)"Push-to-Talk (Hold 'Mouse 4')");
        SendMessageA(g_hVoiceMode, CB_SETITEMDATA, m3, (LPARAM)VOICE_MODE_PTT_MOUSE4);
        int m4 = (int)SendMessageA(g_hVoiceMode, CB_ADDSTRING, 0, (LPARAM)"Push-to-Talk (Hold 'T')");
        SendMessageA(g_hVoiceMode, CB_SETITEMDATA, m4, (LPARAM)VOICE_MODE_PTT_T);
        SendMessageA(g_hVoiceMode, CB_SETCURSEL, (WPARAM)g_voice_mode, 0);

        h = CreateWindowExA(0, "STATIC", "PTT works in-game. Guards hear loud talking!", WS_CHILD | WS_VISIBLE,
                            356, 377, 252, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);

        g_hStatus = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE, 14, 403, 596, 18, wnd, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
        SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)font, TRUE);
        h = CreateWindowExA(0, "STATIC", "In game: COOP -> ONLINE -> CREATE GAME (host) or JOIN GAME (guest).  Works with Steam, Ubisoft and Standalone editions.",
                            WS_CHILD | WS_VISIBLE, 14, 425, 596, 18, wnd, NULL, NULL, NULL);
        SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
        g_hSaver = CreateWindowExA(0, "STATIC", g_sav_status, WS_CHILD | WS_VISIBLE, 14, 447, 596, 18, wnd, (HMENU)(INT_PTR)ID_SAVER, NULL, NULL);
        SendMessageA(g_hSaver, WM_SETFONT, (WPARAM)font, TRUE);
        voice_start();
        SetTimer(wnd, ID_TIMER, 50, NULL);
        gui_refresh();
        return 0;
    }
    case WM_TIMER: {
        pump_steam(gui_cb);
        int lvl = (int)g_voice_ui_level;
        SendMessageA(g_hMicBar, PBM_SETPOS, (WPARAM)lvl, 0);
        char pct_buf[32];
        int vmode = (int)g_voice_mode;
        bool in_channel = is_voice_channel_active();
        if (g_voice_loopback) {
            snprintf(pct_buf, sizeof(pct_buf), "[TEST] %d%%", lvl);
        } else if (!in_channel) {
            snprintf(pct_buf, sizeof(pct_buf), "[MENU] %d%%", lvl);
        } else if (vmode != VOICE_MODE_OPEN) {
            if (g_voice_ptt_active) {
                snprintf(pct_buf, sizeof(pct_buf), "[TALK] %d%%", lvl);
            } else {
                snprintf(pct_buf, sizeof(pct_buf), "[MUTE]");
            }
        } else {
            if (lvl >= 10) {
                snprintf(pct_buf, sizeof(pct_buf), "[VOX] %d%%", lvl);
            } else {
                snprintf(pct_buf, sizeof(pct_buf), "%d%%", lvl);
            }
        }
        SetWindowTextA(g_hMicPct, pct_buf);
        if (++g_gui_ticks % 20 == 0) {
            SetWindowTextA(g_hSaver, g_sav_status);
            patch_running_games();
        }
        if (g_gui_ticks % 300 == 0) gui_refresh(); /* auto refresh every 15 s */
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REFRESH: gui_refresh(); break;
        case ID_JOIN: gui_join_selected(wnd); break;
        case ID_HOSTPUB:
            if (!g_steam_available) { gui_set_status("Steam must be running to host a public game. Use 'Host Private Room'."); break; }
            g_act = ACT_HOST_STEAM; DestroyWindow(wnd); break;
        case ID_HOSTROOM: g_act = ACT_HOST_ROOM; DestroyWindow(wnd); break;
        case ID_JOINCODE:
            GetWindowTextA(g_hCode, g_act_code, sizeof(g_act_code));
            if (!g_act_code[0]) { gui_set_status("Type your friend's Room Code or IP first."); break; }
            g_act = ACT_JOIN_ROOM; DestroyWindow(wnd); break;
        case ID_MICCOMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_hMicCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    UINT dev = (UINT)SendMessageA(g_hMicCombo, CB_GETITEMDATA, sel, 0);
                    mic_set_device(dev);
                    char st[128];
                    snprintf(st, sizeof(st), "Voice Chat microphone set to: %s", g_mic_device_name);
                    gui_set_status(st);
                }
            }
            break;
        case ID_VOICEMODE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_hVoiceMode, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    LONG mode = (LONG)SendMessageA(g_hVoiceMode, CB_GETITEMDATA, sel, 0);
                    InterlockedExchange(&g_voice_mode, mode);
                    const char *mode_names[] = { "Open Mic (Voice Activated)", "Push-to-Talk (Hold 'V')", "Push-to-Talk (Hold 'Caps Lock')", "Push-to-Talk (Hold 'Mouse 4')", "Push-to-Talk (Hold 'T')" };
                    char st[128];
                    snprintf(st, sizeof(st), "Voice mode set to: %s", (mode >= 0 && mode < 5) ? mode_names[mode] : "Custom");
                    gui_set_status(st);
                }
            }
            break;
        case ID_MICTEST: {
            LRESULT chk = SendMessageA(g_hMicTest, BM_GETCHECK, 0, 0);
            InterlockedExchange(&g_voice_loopback, (chk == BST_CHECKED) ? 1 : 0);
            break;
        }
        }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_LIST && ((NMHDR *)lp)->code == NM_DBLCLK) gui_join_selected(wnd);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_voice_loopback, 0);
        KillTimer(wnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static GuiAct gui_run(void)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = gui_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SCCTBrowser";
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    HWND wnd = CreateWindowExA(0, "SCCTBrowser", "Chaos Theory Online Co-op - Server Browser",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 638, 520, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) return ACT_NONE;
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(wnd, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    return g_act;
}

/* ---------------------------------------------------------------- Main */

int main(int argc, char **argv)
{
    init_process_apis();
    SetProcessDPIAware();
    scct_log_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    SetConsoleTitleA("Splinter Cell: Chaos Theory - Universal Online Co-op");
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    locate_game();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--restore-menu")) { patch_menu_text(true); return 0; }
    }
    patch_menu_text(false);
    patch_running_games();
    saver_start();

    bool steam_ok = init_steam();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-launch")) g_no_launch = true;
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host-universal")) {
            int port = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : DIRECT_IP_DEFAULT_PORT;
            return run_universal_host(port);
        }
        else if (!strcmp(argv[i], "--join-universal") && i + 1 < argc) {
            const char *ip = argv[++i];
            int port = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : DIRECT_IP_DEFAULT_PORT;
            return run_universal_join(ip, port);
        }
        else if (!strcmp(argv[i], "+connect_lobby") && i + 1 < argc) {
            if (steam_ok) return run_steam_client(strtoull(argv[++i], NULL, 10));
        }
        else if (!strcmp(argv[i], "--host-steam")) {
            if (steam_ok) return run_steam_host();
        }
        else if (!strcmp(argv[i], "--browse-steam")) {
            if (steam_ok) return browse_and_join_steam_lobbies();
        }
        else if (!strcmp(argv[i], "--join-steam") && i + 1 < argc) {
            if (steam_ok) return run_steam_client(strtoull(argv[++i], NULL, 10));
        }
    }

    bool use_console = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--console")) use_console = true;
    if (!use_console) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
        GuiAct act = gui_run();
        if (con) ShowWindow(con, SW_SHOW);
        switch (act) {
        case ACT_JOIN_STEAM: return run_steam_client(g_act_lobby);
        case ACT_HOST_STEAM: return run_steam_host();
        case ACT_HOST_ROOM: return run_universal_host(DIRECT_IP_DEFAULT_PORT);
        case ACT_JOIN_ROOM: return run_universal_join(g_act_code, DIRECT_IP_DEFAULT_PORT);
        default: if (g_steam_available) S.Shutdown(); WSACleanup(); return 0;
        }
    }

    for (;;) {
        system("cls");
        printf("======================================================================\n");
        printf("    Tom Clancy's Splinter Cell: Chaos Theory - Online Co-op Restorer\n");
        printf("     Steam SDR and direct UDP relay %s\n", SCCT_VERSION);
        printf("======================================================================\n\n");

        printf("  IN-GAME MENU INTEGRATION:\n");
        printf("  Menu routing is applied only to executable builds with verified patch signatures.\n");
        printf("           Clicking 'ONLINE' or 'LAN' routes directly to Create / Join!\n\n");

        printf("  WORLDWIDE PUBLIC CO-OP (NO ROOM CODES OR IPs NEEDED):\n");
        if (steam_ok) {
            printf("  1) Browse & Join Public Games  (Worldwide 1-Click Join)\n");
            printf("     -> Lists all active Chaos Theory games around the world.\n");
            printf("  2) Host a Public Online Game   (Worldwide Public Session)\n");
            printf("     -> Signed in as Steam Agent: %s\n\n", S.GetPersonaName(g_friends));
        } else {
            printf("  1) Browse & Join Public Games  [Steam not running - start Steam to enable]\n");
            printf("  2) Host a Public Online Game   [Steam not running - start Steam to enable]\n\n");
        }

        printf("  DIRECT INTERNET / PRIVATE ROOMS (No Steam required):\n");
        printf("  3) Host Universal Room         (Generates Room Code / Direct IP)\n");
        printf("  4) Join Universal Room         (Enter Friend's Room Code / IP)\n\n");

        printf("  DIRECT STEAM LOBBY:\n");
        if (steam_ok) {
            printf("  5) Join Steam Lobby by ID      (Enter specific Lobby ID)\n\n");
        } else {
            printf("  5) Join Steam Lobby by ID      [Steam not running]\n\n");
        }

        printf("  ESC) Exit\n\n");

        printf("Select option (1-5): ");
        int c = _getch();
        printf("%c\n", c);

        if (c == 27) break;
        if (c == '1' && steam_ok) return browse_and_join_steam_lobbies();
        if (c == '2' && steam_ok) return run_steam_host();
        if (c == '3') return run_universal_host(DIRECT_IP_DEFAULT_PORT);
        if (c == '4') {
            char ip_buf[128];
            printf("\nEnter Host Room Code or Public IP: ");
            if (fgets(ip_buf, sizeof(ip_buf), stdin)) {
                ip_buf[strcspn(ip_buf, "\r\n")] = 0;
                if (ip_buf[0]) return run_universal_join(ip_buf, DIRECT_IP_DEFAULT_PORT);
            }
        }
        if (c == '5' && steam_ok) return run_steam_client(0);
    }

    if (g_steam_available) S.Shutdown();
    WSACleanup();
    return 0;
}


