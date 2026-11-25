#pragma once
// Steam (Spacewar, AppID 480) para unirse sin abrir puertos. steam_api.dll de 32 bits en la carpeta del juego,
// cargado a mano con las funciones planas (cdecl). Sin Steam abierto o sin el dll, Start() da false y el coop
// sigue solo por IP. Las interfaces son las del steam_api.dll 8.33 que trae SteamVR (SDK con
// SteamInternal_SteamAPI_Init, sin SteamAPI_Init exportada).
#include <winsock2.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace steam {

#pragma pack(push, 1)  // como en el SDK (steamnetworkingtypes.h): sin alinear, asi que en 32 bits no salta a 8
struct Identity {  // SteamNetworkingIdentity (136 bytes): tipo 16 = SteamID, 8 bytes, SteamID64
    int type;
    int size;
    uint64_t id;
    uint8_t rest[120];
};
#pragma pack(pop)
struct CallbackMsg {  // CallbackMsg_t
    int user;
    int id;
    uint8_t* param;
    int size;
};
struct Message {  // SteamNetworkingMessage_t, solo el principio. La identidad va en el offset 12 (medido con el Steam
    void* data;    // real: tools/steamselftest.cpp). Leida en el 16 daba siempre 0x01100001 como SteamID: el host le
    int size;      // contestaba a nadie (issue #8)
    uint32_t conn;
    Identity peer;
};
static_assert(sizeof(Identity) == 136 && offsetof(Message, peer) == 12, "estructuras de Steam");

struct Api {
    HMODULE dll;
    int (*init)(const char* versions, char* err);  // 0 = OK
    int (*user)();  // != 0 si Steam ya esta iniciado (el nucleo se recargo)
    int (*pipe)();
    void (*dispatchInit)();
    void (*runFrame)(int pipe);
    bool (*nextCallback)(int pipe, CallbackMsg*);
    void (*freeCallback)(int pipe);
    void* (*userIf)();
    uint64_t (*steamID)(void*);
    void* (*friendsIf)();
    int (*friendCount)(void*, int flags);
    uint64_t (*friendByIndex)(void*, int i, int flags);
    const char* (*friendName)(void*, uint64_t);
    const char* (*friendPresence)(void*, uint64_t, const char* key);
    bool (*setPresence)(void*, const char* key, const char* value);
    void* (*msgIf)();
    int (*send)(void*, const Identity*, const void*, uint32_t len, int flags, int channel);
    int (*receive)(void*, int channel, Message** out, int max);
    bool (*accept)(void*, const Identity*);
    void (*release)(Message*);
    void* (*utilsIf)();
    uint32_t (*appId)(void*);
    void* (*netUtilsIf)();
    void (*debugOutput)(void*, int level, void (*fn)(int, const char*));
    void (*initRelay)(void*);
    void *users, *friends, *msgs;
};
inline Api a;
inline bool up;
inline char error[128] = "";

// Registro del modulo (CoD1 o CoD2): la app con la que arranco Steam, las sesiones aceptadas, los envios que fallan y
// los avisos de la red de Steam (issue #8: unirse por Steam no hacia nada y el registro no decia por que).
inline void (*log)(const char* fmt, ...);
inline CRITICAL_SECTION netLock;
inline char netLines[16][256];
inline int netCount, netLogged;
inline void NetDebug(int, const char* msg)  // desde un hilo de Steam: se guarda y Poll lo escribe
{
    EnterCriticalSection(&netLock);
    if (netCount < 16) snprintf(netLines[netCount++], sizeof netLines[0], "%s", msg);
    LeaveCriticalSection(&netLock);
}

// Steam visto sin steam_api: pid del registro (queda el viejo si Steam se cerro mal), si ese proceso vive y el usuario
// con sesion. Cambia al abrir Steam o al iniciar sesion.
inline uint64_t State()
{
    DWORD pid = 0, user = 0, size;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        size = sizeof pid;
        RegQueryValueExA(k, "pid", nullptr, nullptr, (BYTE*)&pid, &size);
        size = sizeof user;
        RegQueryValueExA(k, "ActiveUser", nullptr, nullptr, (BYTE*)&user, &size);
        RegCloseKey(k);
    }
    bool alive = false;
    if (HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr) {
        alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        CloseHandle(p);
    }
    return alive ? (uint64_t)pid << 32 | user : 0;
}

// Cada SteamAPI_Init que falla deja 32 MB de memoria virtual reservada (CoD2 con Steam cerrado: +32 MB cada 10 s y
// "Out of memory" a los 5 minutos). Se intenta al arrancar y de nuevo solo si Steam cambio (se abrio, inicio sesion).
inline bool Start()
{
    static DWORD lastTry;
    static uint64_t triedState = ~0ull;
    if (up) return true;
    if (lastTry && GetTickCount() - lastTry < 10000) return false;  // Steam se puede abrir despues que el juego
    lastTry = GetTickCount();
    uint64_t state = State();
    if (state == triedState) return false;  // sigue igual que en el intento fallido: steam::error ya lo dice
    triedState = state;
    if (!a.dll && !(a.dll = LoadLibraryA("steam_api.dll"))) return strcpy(error, "falta steam_api.dll"), false;
    struct { void* field; const char* name; } fns[] = {
        {&a.init, "SteamInternal_SteamAPI_Init"},
        {&a.user, "SteamAPI_GetHSteamUser"},
        {&a.pipe, "SteamAPI_GetHSteamPipe"},
        {&a.dispatchInit, "SteamAPI_ManualDispatch_Init"},
        {&a.runFrame, "SteamAPI_ManualDispatch_RunFrame"},
        {&a.nextCallback, "SteamAPI_ManualDispatch_GetNextCallback"},
        {&a.freeCallback, "SteamAPI_ManualDispatch_FreeLastCallback"},
        {&a.userIf, "SteamAPI_SteamUser_v023"},
        {&a.steamID, "SteamAPI_ISteamUser_GetSteamID"},
        {&a.friendsIf, "SteamAPI_SteamFriends_v017"},
        {&a.friendCount, "SteamAPI_ISteamFriends_GetFriendCount"},
        {&a.friendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex"},
        {&a.friendName, "SteamAPI_ISteamFriends_GetFriendPersonaName"},
        {&a.friendPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence"},
        {&a.setPresence, "SteamAPI_ISteamFriends_SetRichPresence"},
        {&a.msgIf, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"},
        {&a.send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"},
        {&a.receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"},
        {&a.accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"},
        {&a.release, "SteamAPI_SteamNetworkingMessage_t_Release"},
        {&a.utilsIf, "SteamAPI_SteamUtils_v010"},
        {&a.appId, "SteamAPI_ISteamUtils_GetAppID"},
        {&a.netUtilsIf, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"},
        {&a.debugOutput, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction"},
        {&a.initRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"},
    };
    for (auto& f : fns)
        if (!(*(FARPROC*)f.field = GetProcAddress(a.dll, f.name))) return strcpy(error, "steam_api.dll de otra version"), false;
    if (!a.user()) {
        SetEnvironmentVariableA("SteamAppId", "480");  // Spacewar, sin steam_appid.txt
        a.dispatchInit();
        char err[1024] = "";
        if (a.init("SteamUser023\0SteamFriends017\0SteamNetworkingMessages002\0", err) != 0)
            return snprintf(error, sizeof error, "Steam cerrado o sin sesion (%s)", err), false;
    }
    a.users = a.userIf();
    a.friends = a.friendsIf();
    a.msgs = a.msgIf();
    if (!(up = a.users && a.friends && a.msgs)) return false;
    static bool lock;
    if (!lock) InitializeCriticalSection(&netLock), lock = true;
    if (void* net = a.netUtilsIf()) {
        a.debugOutput(net, 5, NetDebug);  // k_ESteamNetworkingSocketsDebugOutputType_Msg
        // la red de relés de Steam lista desde ya: si no, la arma la primera conexion (el juego se traba ~1 s justo al unirse)
        a.initRelay(net);
    }
    void* utils = a.utilsIf();
    if (log) log("steam: app %u (480 = Spacewar)", utils ? a.appId(utils) : 0);
    return true;
}

// antes de descargar el modulo (el nucleo de CoD1 se recarga): que Steam no siga llamando a NetDebug
inline void Stop()
{
    if (void* net = up ? a.netUtilsIf() : nullptr) a.debugOutput(net, 0, nullptr);
}

inline uint64_t MyId() { return up ? a.steamID(a.users) : 0; }
inline const char* Name(uint64_t id) { return up ? a.friendName(a.friends, id) : ""; }  // UTF-8
inline void SetPresence(const char* key, const char* value) { if (up) a.setPresence(a.friends, key, value); }

// Callbacks de Steam: aceptar a quien nos manda mensajes (SteamNetworkingMessagesSessionRequest_t) y
// "Unirse a la partida" desde la lista de amigos (GameRichPresenceJoinRequested_t: amigo + texto "connect").
inline void Poll(void (*onJoin)(const char* connect))
{
    if (!up) return;
    int pipe = a.pipe();
    a.runFrame(pipe);
    CallbackMsg m;
    while (a.nextCallback(pipe, &m)) {
        if (m.id == 1251) {
            bool ok = a.accept(a.msgs, (const Identity*)m.param);
            if (log) log("steam: %I64u abre una sesion: %s", ((const Identity*)m.param)->id, ok ? "aceptada" : "NO se pudo aceptar");
        } else if (m.id == 1252 && log) {  // SteamNetworkingMessagesSessionFailed_t: el motivo lo da NetDebug
            log("steam: la sesion con %I64u fallo", ((const Identity*)m.param)->id);
        } else if (m.id == 337 && onJoin) onJoin((const char*)m.param + 8);
        a.freeCallback(pipe);
    }
    EnterCriticalSection(&netLock);
    for (int i = 0; i < netCount; ++i)
        if (log && netLogged < 300) log("steam red: %s", netLines[i]), ++netLogged;  // tope por si Steam habla de mas
    netCount = 0;
    LeaveCriticalSection(&netLock);
}

inline bool Send(uint64_t id, const void* data, int len)
{
    if (!up || !id) return false;
    Identity to{16, 8, id, {}};
    int r = a.send(a.msgs, &to, data, (uint32_t)len, 1 | 32, 0);  // sin Nagle, reabre la sesion si se corto
    static int lastBad;
    if (r != 1 && r != lastBad && log) log("steam: no se pudo mandar a %I64u (EResult %d)", id, r);
    lastBad = r == 1 ? 0 : r;
    return r == 1;  // k_EResultOK
}

// un mensaje recibido o 0; uno mas grande que buf se descarta
inline int Recv(uint8_t* buf, int size, uint64_t* from)
{
    Message* m;
    if (!up || a.receive(a.msgs, 0, &m, 1) != 1) return 0;
    int n = m->size <= size ? m->size : 0;
    memcpy(buf, m->data, n);
    *from = m->peer.id;
    a.release(m);
    return n;
}

// amigos con rich presence codcoop=host (Steam solo da la de amigos en la misma app, 480); CoD2 usa codcoop2
template <class F> void ForEachHost(F fn, const char* key = "codcoop")
{
    if (!up) return;
    int count = a.friendCount(a.friends, 4);  // k_EFriendFlagImmediate
    for (int i = 0; i < count; ++i) {
        uint64_t id = a.friendByIndex(a.friends, i, 4);
        if (!strcmp(a.friendPresence(a.friends, id, key), "host")) fn(id);
    }
}

// Un amigo de Steam se ve en el resto del mod como la direccion 0.83.84.n:480 (0.x.x.x nunca es una IP real):
// asi los slots del host, el netchan del motor y coopnet siguen trabajando con direcciones.
constexpr int MAX_PEERS = 16;
inline bool IsAddr(const sockaddr_in& to)
{
    const uint8_t* b = (const uint8_t*)&to.sin_addr;
    return b[0] == 0 && b[1] == 'S' && b[2] == 'T';
}
inline sockaddr_in Addr(uint64_t (&peers)[MAX_PEERS], uint64_t id)
{
    static int next;
    int i = 0;
    while (i < MAX_PEERS && peers[i] && peers[i] != id) ++i;
    if (i == MAX_PEERS) i = next++ % MAX_PEERS;  // shortcut: con mas de 16 amigos distintos en una sesion se recicla en ronda
    peers[i] = id;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    uint8_t* b = (uint8_t*)&to.sin_addr;
    b[1] = 'S', b[2] = 'T', b[3] = (uint8_t)(i + 1);
    to.sin_port = htons(480);
    return to;
}
inline uint64_t IdOf(const uint64_t (&peers)[MAX_PEERS], const sockaddr_in& to)
{
    int i = ((const uint8_t*)&to.sin_addr)[3] - 1;
    return IsAddr(to) && i >= 0 && i < MAX_PEERS ? peers[i] : 0;
}

// nombres de Steam (UTF-8) para la fuente del juego (Latin-1); lo que no entra queda como '?'
inline void Latin1(const char* s, char* out, int size)
{
    int n = 0;
    for (const uint8_t* p = (const uint8_t*)s; *p && n < size - 1; ++p) {
        if (*p < 0x80) out[n++] = (char)*p;
        else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            unsigned c = (*p & 0x1fu) << 6 | (p[1] & 0x3fu);
            out[n++] = c < 0x100 ? (char)c : '?';
            ++p;
        } else if ((*p & 0xc0) != 0x80) out[n++] = '?';  // principio de un caracter de 3 o 4 bytes
    }
    out[n] = 0;
}

inline bool SelfTest()
{
    uint64_t t[MAX_PEERS] = {};
    sockaddr_in x = Addr(t, 111), y = Addr(t, 222), x2 = Addr(t, 111), ip{};
    ip.sin_addr.s_addr = htonl(0x7f000001);
    char name[16];
    Latin1("Agust\xc3\xadn \xe2\x98\x85", name, sizeof name);
    return IsAddr(x) && !IsAddr(ip) && !memcmp(&x, &x2, sizeof x) && memcmp(&x, &y, sizeof x) && IdOf(t, x) == 111 &&
           IdOf(t, y) == 222 && IdOf(t, ip) == 0 && !strcmp(name, "Agust\xedn ?");
}

}  // namespace steam
