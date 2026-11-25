// steam_api.dll FALSO para pruebas (no va al instalador): cada juego es un usuario de Steam distinto en la misma PC,
// sin cuentas. Implementa solo lo que usa src/steam.h, con la misma forma de las funciones planas.
//   FAKESTEAM_ID   SteamID64 de este juego (por defecto 76561190000000000 + pid)
//   FAKESTEAM_NAME nombre (por defecto Fake<pid>)
//   FAKESTEAM_DROP 1 = los mensajes salen pero nunca llegan (simula una conexion P2P que no se arma)
// Los mensajes van por UDP local; cada juego anota su puerto, nombre y rich presence en D:\CoDCoop\run\fakesteam\<id>.txt
// (los "amigos" son los demas archivos con el proceso vivo). Como Steam: quien recibe de alguien nuevo ve el callback
// 1251 (SteamNetworkingMessagesSessionRequest_t) y sus mensajes esperan hasta AcceptSessionWithUser; quien manda primero
// ya tiene la sesion aceptada.
// Armar: g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -o build\fakesteam\steam_api.dll tools\fakesteam.cpp -lws2_32
#include <winsock2.h>
#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>

#define API extern "C" __declspec(dllexport)

namespace {
#pragma pack(push, 1)  // como el SDK y el Steam real (no copiar de steam.h: si steam.h se equivoca, esto lo tiene que ver)
struct Identity {
    int type;
    int size;
    uint64_t id;
    uint8_t rest[120];
};
#pragma pack(pop)
struct CallbackMsg {
    int user;
    int id;
    uint8_t* param;
    int size;
};
struct Message {
    void* data;
    int size;
    uint32_t conn;
    Identity peer;
};
static_assert(sizeof(Identity) == 136 && offsetof(Message, peer) == 12, "SteamNetworkingMessage_t de 32 bits");

const char* DIR = "D:\\CoDCoop\\run\\fakesteam";
bool g_init, g_drop;
uint64_t g_me;
char g_name[64];
SOCKET g_sock = INVALID_SOCKET;
int g_port;
std::map<std::string, std::string> g_presence;
void (*g_debug)(int, const char*);

struct Peer {
    bool accepted, asked;
    int port;
    std::string name;
    std::map<std::string, std::string> presence;
};
std::map<uint64_t, Peer> g_peers;
struct Pending {
    uint64_t from;
    std::string data;
};
std::deque<Pending> g_inbox;
std::deque<Identity> g_requests;  // callbacks 1251 por entregar
Identity g_cbParam;

void Debug(const char* fmt, ...)
{
    if (!g_debug) return;
    char s[256];
    va_list v;
    va_start(v, fmt);
    vsnprintf(s, sizeof s, fmt, v);
    va_end(v);
    g_debug(5, s);
}

void WriteMe()
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%llu.txt", DIR, g_me);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "pid=%lu\nport=%d\nname=%s\n", GetCurrentProcessId(), g_port, g_name);
    for (auto& kv : g_presence) fprintf(f, "rp.%s=%s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
}

// relee los archivos de los demas: puerto, nombre y rich presence (solo los de procesos vivos)
void ReadPeers()
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*.txt", DIR);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    for (auto& p : g_peers) p.second.port = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t id = _strtoui64(fd.cFileName, nullptr, 10);
        if (!id || id == g_me) continue;
        char path[MAX_PATH], line[512];
        snprintf(path, sizeof path, "%s\\%s", DIR, fd.cFileName);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        Peer tmp{};
        DWORD pid = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (!strcmp(line, "pid")) pid = strtoul(eq + 1, nullptr, 10);
            else if (!strcmp(line, "port")) tmp.port = atoi(eq + 1);
            else if (!strcmp(line, "name")) tmp.name = eq + 1;
            else if (!strncmp(line, "rp.", 3)) tmp.presence[line + 3] = eq + 1;
        }
        fclose(f);
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) CloseHandle(p);
        if (!alive) continue;
        Peer& peer = g_peers[id];
        peer.port = tmp.port;
        peer.name = tmp.name;
        peer.presence = tmp.presence;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void Pump()
{
    if (g_sock == INVALID_SOCKET) return;
    static char buf[65536];
    for (;;) {
        int n = recv(g_sock, buf, sizeof buf, 0);
        if (n < 8) {
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return;
            if (n < 0) continue;  // WSAECONNRESET de un envio viejo
            return;
        }
        uint64_t from;
        memcpy(&from, buf, 8);
        Peer& p = g_peers[from];
        if (!p.accepted && !p.asked) {
            Identity who{16, 8, from, {}};
            g_requests.push_back(who);
            p.asked = true;
            Debug("fakesteam: %llu pide sesion", from);
        }
        g_inbox.push_back({from, std::string(buf + 8, n - 8)});
        static int said;
        if (said++ < 5) Debug("fakesteam: llegan %d bytes de %llu", n - 8, from);
    }
}
}  // namespace

API int SteamInternal_SteamAPI_Init(const char*, char* err)
{
    if (g_init) return 0;
    CreateDirectoryA("D:\\CoDCoop\\run", nullptr);
    CreateDirectoryA(DIR, nullptr);
    char v[64];
    g_me = GetEnvironmentVariableA("FAKESTEAM_ID", v, sizeof v) ? _strtoui64(v, nullptr, 10)
                                                                 : 76561190000000000ull + GetCurrentProcessId();
    if (!GetEnvironmentVariableA("FAKESTEAM_NAME", g_name, sizeof g_name))
        snprintf(g_name, sizeof g_name, "Fake%lu", GetCurrentProcessId());
    g_drop = GetEnvironmentVariableA("FAKESTEAM_DROP", v, sizeof v) && v[0] == '1';
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (g_sock == INVALID_SOCKET || bind(g_sock, (sockaddr*)&a, sizeof a) || getsockname(g_sock, (sockaddr*)&a, &len)) {
        if (err) strcpy(err, "fakesteam: sin socket");
        return 1;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_port = ntohs(a.sin_port);
    WriteMe();
    g_init = true;
    return 0;
}
API int SteamAPI_GetHSteamUser() { return g_init ? 1 : 0; }
API int SteamAPI_GetHSteamPipe() { return 1; }
API void SteamAPI_ManualDispatch_Init() {}
API void SteamAPI_ManualDispatch_RunFrame(int) { Pump(); }
API bool SteamAPI_ManualDispatch_GetNextCallback(int, CallbackMsg* out)
{
    if (g_requests.empty()) return false;
    g_cbParam = g_requests.front();
    g_requests.pop_front();
    *out = {1, 1251, (uint8_t*)&g_cbParam, (int)sizeof g_cbParam};
    return true;
}
API void SteamAPI_ManualDispatch_FreeLastCallback(int) {}

static int g_iface;  // las interfaces solo tienen que ser punteros validos
API void* SteamAPI_SteamUser_v023() { return &g_iface; }
API uint64_t SteamAPI_ISteamUser_GetSteamID(void*) { return g_me; }
API void* SteamAPI_SteamFriends_v017() { return &g_iface; }

static uint64_t g_friends[64];
static int g_friendCount;
API int SteamAPI_ISteamFriends_GetFriendCount(void*, int)
{
    ReadPeers();
    g_friendCount = 0;
    for (auto& p : g_peers)
        if (p.second.port && g_friendCount < 64) g_friends[g_friendCount++] = p.first;
    return g_friendCount;
}
API uint64_t SteamAPI_ISteamFriends_GetFriendByIndex(void*, int i, int) { return i >= 0 && i < g_friendCount ? g_friends[i] : 0; }
API const char* SteamAPI_ISteamFriends_GetFriendPersonaName(void*, uint64_t id)
{
    static std::string s;
    if (id == g_me) return g_name;
    auto it = g_peers.find(id);
    s = it != g_peers.end() && !it->second.name.empty() ? it->second.name : "[unknown]";
    return s.c_str();
}
API const char* SteamAPI_ISteamFriends_GetFriendRichPresence(void*, uint64_t id, const char* key)
{
    static std::string s;
    s.clear();
    auto it = g_peers.find(id);
    if (it != g_peers.end()) {
        auto kv = it->second.presence.find(key);
        if (kv != it->second.presence.end()) s = kv->second;
    }
    return s.c_str();
}
API bool SteamAPI_ISteamFriends_SetRichPresence(void*, const char* key, const char* value)
{
    if (value && *value) g_presence[key] = value;
    else g_presence.erase(key);
    WriteMe();
    return true;
}

API void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002() { return &g_iface; }
API int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void*, const Identity* to, const void* data, uint32_t len,
                                                            int, int)
{
    if (!g_init || !to || to->type != 16) return 2;  // k_EResultFail
    Peer& p = g_peers[to->id];
    p.accepted = true;  // quien manda primero abre la sesion
    if (!p.port) ReadPeers();
    if (!p.port) {
        Debug("fakesteam: %llu no esta (sin archivo o proceso cerrado)", to->id);
        return 1;  // Steam lo encola igual y falla despues
    }
    if (g_drop) return 1;
    static char buf[65536];
    if (len > sizeof buf - 8) return 2;
    memcpy(buf, &g_me, 8);
    memcpy(buf + 8, data, len);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)p.port);
    int r = sendto(g_sock, buf, (int)len + 8, 0, (sockaddr*)&a, sizeof a);
    static int said;
    if (said++ < 5) Debug("fakesteam: mando %u bytes a %llu (puerto %d, sendto %d)", len, to->id, p.port, r);
    return 1;
}
API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void*, int, Message** out, int max)
{
    Pump();
    int n = 0;
    for (auto it = g_inbox.begin(); it != g_inbox.end() && n < max;) {
        if (!g_peers[it->from].accepted) {
            ++it;
            continue;
        }
        Message* m = (Message*)calloc(1, sizeof(Message) + it->data.size());
        m->data = m + 1;
        m->size = (int)it->data.size();
        m->peer = {16, 8, it->from, {}};
        memcpy(m->data, it->data.data(), it->data.size());
        out[n++] = m;
        it = g_inbox.erase(it);
    }
    static int said;
    if (n && said++ < 5) Debug("fakesteam: entrego %d bytes de %llu", out[0]->size, out[0]->peer.id);
    return n;
}
API bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void*, const Identity* who)
{
    if (!who) return false;
    g_peers[who->id].accepted = true;
    Debug("fakesteam: sesion con %llu aceptada", who->id);
    return true;
}
API void SteamAPI_SteamNetworkingMessage_t_Release(Message* m) { free(m); }

API void* SteamAPI_SteamUtils_v010() { return &g_iface; }
API uint32_t SteamAPI_ISteamUtils_GetAppID(void*) { return 480; }
API void* SteamAPI_SteamNetworkingUtils_SteamAPI_v004() { return &g_iface; }
API void SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(void*, int, void (*fn)(int, const char*)) { g_debug = fn; }
API void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(void*) {}
