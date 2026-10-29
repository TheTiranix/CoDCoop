// Coop: el host corre la campaña y los demas se conectan como clientes 1..3 de su servidor.
// Transporte UDP propio (el exe solo tiene loopback) + canal de control para lo que el SP
// comparte por memoria entre servidor y cliente (gamestate y estados de entidad).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "game.h"
#include "hook.h"

void Log(const char* fmt, ...);

namespace {

constexpr uint32_t CTRL = 0xFFFFFFFE;  // prefijo de nuestros paquetes; el motor usa -1 (sin conexion) o una secuencia
enum : uint8_t { M_JOIN = 'J', M_ACCEPT = 'A', M_FULL = 'F', M_CS = 'C', M_CSDONE = 'G', M_ENTS = 'E' };
constexpr int MAX_PACKET = 1400;

enum Role { NONE, HOST, JOINER };
Role g_role = NONE;
SOCKET g_sock = INVALID_SOCKET;

// host: por slot remoto
struct Remote {
    bool active;
    sockaddr_in addr;
    uint8_t lastEnt[g::MAX_GENTITIES][g::ENTSTATE_SIZE];
    DWORD lastSent[g::MAX_GENTITIES];
};
Remote* g_remote[g::MAX_CLIENTS];

// diagnostico: contadores que se vuelcan al log cada 5 s
int g_statSnaps, g_statEnts, g_statRecv, g_statCtrl;
void StatsTick()
{
    static DWORD last;
    if (GetTickCount() - last < 5000) return;
    last = GetTickCount();
    if (g_statSnaps || g_statEnts || g_statRecv || g_statCtrl) {
        Log("stats 5s: snaps=%d ents=%d recv=%d ctrl=%d cls.state=%d serverMsgSeq=%d cl.snap(time=%d num=%d)", g_statSnaps,
            g_statEnts, g_statRecv, g_statCtrl, g::cls_state, *(int*)0x14795c4, *(int*)0x1426e68, *(int*)0x1426e6c);
        if (uint8_t* cg = (uint8_t*)GetModuleHandleA("cgamex86.dll")) {
            uint8_t* snap = *(uint8_t**)(cg + 0x10b8ac);  // cg.snap
            Log("  cgame %p: cg.snap=%p serverTime=%d nextSnap=%p", cg, snap, snap ? *(int*)(snap + 4) : -1,
                *(void**)(cg + 0x10b8a8));
        }
    }
    g_statSnaps = g_statEnts = g_statRecv = g_statCtrl = 0;
}

// joiner
sockaddr_in g_host;
DWORD g_lastJoinTry;
bool g_joined, g_frozen;
int g_slot;
char g_cs[g::MAX_CONFIGSTRINGS][1024];
char g_hostMap[64];

bool OpenSocket(int port)
{
    if (g_sock != INVALID_SOCKET) return true;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    int buf = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf, sizeof buf);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (char*)&buf, sizeof buf);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (bind(g_sock, (sockaddr*)&a, sizeof a) != 0) {
        Log("bind %d fallo: %d", port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    Log("socket UDP en puerto %d", port);
    return true;
}

netadr_t ToNet(const sockaddr_in& a)
{
    netadr_t n{};
    n.type = NA_IP;
    memcpy(n.ip, &a.sin_addr, 4);
    n.port = a.sin_port;
    return n;
}

sockaddr_in ToSock(const netadr_t& n)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, n.ip, 4);
    a.sin_port = n.port;
    return a;
}

void SendTo(const sockaddr_in& to, const void* data, int len)
{
    sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&to, sizeof to);
}

// Paquete de control: [CTRL][tipo][datos]
struct Writer {
    uint8_t buf[MAX_PACKET + 64];
    int len = 0;
    explicit Writer(uint8_t type) { Put(&CTRL, 4); buf[len++] = type; }
    void Put(const void* p, int n) { memcpy(buf + len, p, n); len += n; }
    template <class T> void Put(T v) { Put(&v, sizeof v); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    template <class T> T Get()
    {
        T v{};
        if (p + sizeof v <= end) memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
    bool Ok() const { return p <= end; }
};

// ---------- host ----------

int FindSlot(const sockaddr_in& from)
{
    for (int n = 1; n < g::MAX_CLIENTS; ++n)
        if (g_remote[n] && g_remote[n]->active && g_remote[n]->addr.sin_addr.s_addr == from.sin_addr.s_addr &&
            g_remote[n]->addr.sin_port == from.sin_port)
            return n;
    return -1;
}

void SendGamestate(int n)
{
    const sockaddr_in& to = g_remote[n]->addr;
    Writer w(M_CS);
    for (int i = 0; i < g::MAX_CONFIGSTRINGS; ++i) {
        const char* s = g::ConfigString(i);
        int l = (int)strlen(s);
        if (!l) continue;
        if (l > 1000) l = 1000;  // shortcut: los configstrings del SP no pasan de 1 KB, si aparece uno mas largo se trunca
        if (w.len + 4 + l > MAX_PACKET) {
            SendTo(to, w.buf, w.len);
            w = Writer(M_CS);
        }
        w.Put<uint16_t>((uint16_t)i);
        w.Put<uint16_t>((uint16_t)l);
        w.Put(s, l);
    }
    SendTo(to, w.buf, w.len);
    Writer done(M_CSDONE);
    SendTo(to, done.buf, done.len);
}

// ClientSpawn (gamex86 0x14900) borra el gclient y no vuelve a poner ps.clientNum (+0x9c): en el SP
// siempre era 0. Con 0, Pmove se choca con su propia entidad (allsolid) y el jugador no se mueve.
void FixClientNum(int n)
{
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    if (gameClients) *(int*)(gameClients + n * *(int*)0x152bc68 + 0x9c) = n;
}

void HostJoin(const sockaddr_in& from, Reader r)
{
    int qport = r.Get<int>(), lastIncoming = r.Get<int>(), serverCmdSeq = r.Get<int>(), relSeq = r.Get<int>();
    if (!r.Ok()) return;
    if (g::sv_state != 2) return;  // sin partida no hay a donde unirse
    int n = FindSlot(from);
    if (n < 0) {
        for (int i = 1; i < g::MAX_CLIENTS; ++i)
            if (*(int*)(g::Client(i) + g::CL_STATE) == 0) { n = i; break; }
        if (n < 0) {
            Writer w(M_FULL);
            SendTo(from, w.buf, w.len);
            return;
        }
        if (!g_remote[n]) g_remote[n] = (Remote*)calloc(1, sizeof(Remote));
        memset(g_remote[n], 0, sizeof(Remote));
        g_remote[n]->active = true;
        g_remote[n]->addr = from;

        // como SVC_DirectConnect (0x448d70) pero en el slot n y continuando los contadores del que entra
        uint8_t* cl = g::Client(n);
        memset(cl, 0, g::CLIENT_SIZE);
        uint8_t* nc = cl + g::CL_NETCHAN;
        *(int*)(nc + g::NC_SOCK) = NS_SERVER;
        *(netadr_t*)(nc + g::NC_ADR) = ToNet(from);
        *(int*)(nc + g::NC_QPORT) = qport & 0xffff;
        *(int*)(nc + g::NC_IN) = 0;
        *(int*)(nc + g::NC_OUT) = lastIncoming + 10;  // el cgame del que entra ya vio snapshots hasta ahi
        *(int*)(cl + 4) = 0x18000;
        *(void**)(cl + 8) = g::Z_Malloc(0x18000);
        *(void**)(cl + 0x10) = g::Z_Malloc(0x400);
        *(void**)(cl + 0xc) = g::Z_Malloc(0x400);
        *(void**)(cl + 0x14) = *(void**)(cl + 8);
        *(int*)(cl + g::CL_RELSEQ) = *(int*)(cl + g::CL_RELACK) = *(int*)(cl + g::CL_RELSENT) = serverCmdSeq;
        *(int*)(cl + g::CL_LASTCLIENTCMD) = relSeq;
        *(int*)(cl + g::CL_GAMESTATEMSG) = *(int*)(nc + g::NC_OUT);
        *(int*)(cl + g::CL_STATE) = 1;

        // como SV_ClientEnterWorld (0x448e40), que tiene el ClientConnect fijo en 0
        g::GameCall(2, n);
        *(uint8_t**)(cl + g::CL_GENTITY) = g::Entity(n);
        *(int*)(cl + g::CL_DELTA) = -1;
        g::GameCall(3, n);
        FixClientNum(n);
        Log("host: jugador remoto %s:%d -> slot %d (qport %d)", inet_ntoa(from.sin_addr), ntohs(from.sin_port), n, qport);
        g::Com_Printf("^2COOP: entro un jugador al slot %d\n", n);
    }
    Writer w(M_ACCEPT);
    w.Put<int>(n);
    const char* map = g::CvarString("mapname");
    w.Put(map, (int)strlen(map) + 1);
    SendTo(from, w.buf, w.len);
    SendGamestate(n);
}

// estados de entidad que el SP no manda en el snapshot (el cgame los lee de la memoria del servidor)
void SendEntities(int n)
{
    Remote* rm = g_remote[n];
    DWORD now = GetTickCount();
    Writer w(M_ENTS);
    int countPos = w.len;
    uint16_t count = 0;
    w.Put<uint16_t>(0);
    auto flush = [&] {
        if (!count) return;
        memcpy(w.buf + countPos, &count, 2);
        SendTo(rm->addr, w.buf, w.len);
        w = Writer(M_ENTS);
        countPos = w.len;
        count = 0;
        w.Put<uint16_t>(0);
    };
    for (int i = 0; i < g::snap_numEntities; ++i) {
        int e = g::snap_entities[i];
        if (e < 0 || e >= g::MAX_GENTITIES) continue;
        const uint8_t* st = g::Entity(e);
        // shortcut: sin acks; lo cambiado va siempre y todo se reenvia cada 1 s por si se perdio un paquete
        if (!memcmp(rm->lastEnt[e], st, g::ENTSTATE_SIZE) && now - rm->lastSent[e] < 1000) continue;
        memcpy(rm->lastEnt[e], st, g::ENTSTATE_SIZE);
        rm->lastSent[e] = now;
        if (w.len + 2 + g::ENTSTATE_SIZE > MAX_PACKET) flush();
        w.Put<uint16_t>((uint16_t)e);
        w.Put(st, g::ENTSTATE_SIZE);
        ++count;
        ++g_statEnts;
    }
    flush();
}

// ---------- joiner ----------

void JoinerSendJoin()
{
    Writer w(M_JOIN);
    w.Put<int>(*(int*)(g::clc_netchan + g::NC_QPORT));
    w.Put<int>(*(int*)(g::clc_netchan + g::NC_IN));
    w.Put<int>(g::clc_serverCommandSequence);
    w.Put<int>(g::clc_reliableSequence);
    SendTo(g_host, w.buf, w.len);
    g_lastJoinTry = GetTickCount();
}

void JoinerHijack()
{
    if (strcmp(g_hostMap, g::CvarString("mapname")) != 0) {
        g::Com_Printf("^1COOP: el host esta en '%s' y vos en '%s'. Carga el mismo mapa y repeti coopjoin.\n",
                      g_hostMap, g::CvarString("mapname"));
        Log("joiner: mapa distinto (host %s, local %s)", g_hostMap, g::CvarString("mapname"));
        g_role = NONE;
        return;
    }
    g_frozen = true;                              // el servidor local queda solo para colision y assets
    *(int*)(g::Client(0) + g::CL_STATE) = 0;      // que no acumule comandos que nadie confirma
    for (int i = 0; i < g::MAX_CONFIGSTRINGS; ++i)
        if (strcmp(g::ConfigString(i), g_cs[i]) != 0) g::SV_SetConfigstring(i, g_cs[i]);
    int state = g::cls_state;
    g::CL_NetchanSetup(ToNet(g_host));
    g::cls_state = state;
    g::CL_ApplyConfigstrings(g::sv_configstrings);
    g_joined = true;
    Log("joiner: conectado al host como slot %d en %s", g_slot, g_hostMap);
    g::Com_Printf("^2COOP: conectado al host (jugador %d)\n", g_slot + 1);
}

void JoinerControl(uint8_t type, Reader r)
{
    switch (type) {
    case M_ACCEPT:
        if (g_joined) return;
        g_slot = r.Get<int>();
        strncpy(g_hostMap, (const char*)r.p, sizeof g_hostMap - 1);
        memset(g_cs, 0, sizeof g_cs);
        break;
    case M_FULL:
        g::Com_Printf("^1COOP: el host esta lleno (4 jugadores)\n");
        g_role = NONE;
        break;
    case M_CS:
        while (r.p + 4 <= r.end) {
            int i = r.Get<uint16_t>(), l = r.Get<uint16_t>();
            if (i >= g::MAX_CONFIGSTRINGS || l > 1000 || r.p + l > r.end) break;
            memcpy(g_cs[i], r.p, l);
            g_cs[i][l] = 0;
            r.p += l;
        }
        break;
    case M_CSDONE:
        if (!g_joined && g_hostMap[0]) JoinerHijack();
        break;
    case M_ENTS: {
        if (!g_joined) return;
        int count = r.Get<uint16_t>();
        for (int k = 0; k < count && r.p + 2 + g::ENTSTATE_SIZE <= r.end; ++k) {
            int e = r.Get<uint16_t>();
            if (e < g::MAX_GENTITIES) memcpy(g::Entity(e), r.p, g::ENTSTATE_SIZE);
            r.p += g::ENTSTATE_SIZE;
        }
        break;
    }
    }
}

// ---------- comandos de consola ----------

void Cmd_CoopHost()
{
    int port = g::cmd_argc > 1 ? atoi(g::Argv(1)) : 28960;
    if (!OpenSocket(port)) {
        g::Com_Printf("^1COOP: no se pudo abrir el puerto UDP %d\n", port);
        return;
    }
    g_role = HOST;
    g::Com_Printf("^2COOP: host listo en el puerto %d. Los demas: coopjoin <tu ip>\n", port);
}

void Cmd_CoopJoin()
{
    if (g::cmd_argc < 2) {
        g::Com_Printf("uso: coopjoin <ip>[:puerto]\n");
        return;
    }
    char host[128];
    strncpy(host, g::Argv(1), sizeof host - 1);
    host[sizeof host - 1] = 0;
    int port = 28960;
    if (char* c = strchr(host, ':')) { *c = 0; port = atoi(c + 1); }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (!OpenSocket(0)) return;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        g::Com_Printf("^1COOP: no se encontro '%s'\n", host);
        return;
    }
    g_host = *(sockaddr_in*)res->ai_addr;
    g_host.sin_port = htons((u_short)port);
    freeaddrinfo(res);
    g_role = JOINER;
    g_joined = false;
    g_hostMap[0] = 0;
    JoinerSendJoin();
    g::Com_Printf("COOP: uniendose a %s:%d...\n", host, port);
}

// Pruebas sin teclado: lo que se escriba en codcoop.cmd (o codcoop_join.cmd) va a la consola.
void PollCmdFile()
{
    static DWORD last;
    static char path[MAX_PATH];
    if (GetTickCount() - last < 250) return;
    last = GetTickCount();
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        strcpy(strrchr(path, '\\') + 1, strstr(GetCommandLineA(), "+coopjoin") ? "codcoop_join.cmd" : "codcoop.cmd");
    }
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char text[2048];
    size_t n = fread(text, 1, sizeof text - 2, f);
    fclose(f);
    remove(path);
    text[n] = 0;
    strcat(text, "\n");
    Log("cmd: %s", text);
    g::Cbuf_AddText(text);
}

void Cmd_CoopInfo()
{
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    int gameClientSize = *(int*)0x152bc68;
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        if (!g::svs_clients || !gameClients) break;
        const float* o = (const float*)(gameClients + n * gameClientSize + 0x14);  // ps.origin
        const int* uc = (const int*)(g::Client(n) + 0x30);  // lastUsercmd
        const int* ps = (const int*)(gameClients + n * gameClientSize);
        Log("slot %d: state=%d ps.origin=(%.0f %.0f %.0f) pm_type=%d health=%d commandTime=%d ucmd=%d %08x %08x", n,
            *(int*)(g::Client(n) + g::CL_STATE), o[0], o[1], o[2], ps[1], ps[0x39], ps[0], uc[0], uc[1], uc[5]);
        g::Com_Printf("slot %d: state=%d origin=(%.0f %.0f %.0f)\n", n, *(int*)(g::Client(n) + g::CL_STATE), o[0], o[1], o[2]);
    }
    // que tiene distinto el ps del jugador 2 respecto del host (primeros 0x400 bytes)
    if (gameClients && *(int*)(g::Client(1) + g::CL_STATE) == 1) {
        const int* a = (const int*)gameClients;
        const int* b = (const int*)(gameClients + gameClientSize);
        char line[1024];
        int len = 0;
        for (int i = 0; i < 0x100 && len < 900; ++i)
            if (a[i] != b[i]) len += snprintf(line + len, sizeof line - len, " +%x:%x/%x", i * 4, a[i], b[i]);
        Log("ps distinto (host/remoto):%s", line);
    }
}

// lleva al jugador n al lado del host (ps.origin; el juego lo copia a la entidad en el proximo think)
void Cmd_CoopBring()
{
    int n = atoi(g::Argv(1));
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    int size = *(int*)0x152bc68;
    if (n < 1 || n >= g::MAX_CLIENTS || !gameClients) return;
    float* dst = (float*)(gameClients + n * size + 0x14);
    const float* src = (const float*)(gameClients + 0x14);
    dst[0] = src[0] + 48;
    dst[1] = src[1];
    dst[2] = src[2] + 8;
    memset(dst + 3, 0, 12);  // velocidad
    Log("coopbring %d -> (%.0f %.0f %.0f)", n, dst[0], dst[1], dst[2]);
}

bool g_cmdsReady;
void RegisterCommands()
{
    g_cmdsReady = true;
    g::Cmd_AddCommand("coopbring", Cmd_CoopBring);
    g::Cmd_AddCommand("coopinfo", Cmd_CoopInfo);
    g::Cmd_AddCommand("coophost", Cmd_CoopHost);
    g::Cmd_AddCommand("coopjoin", Cmd_CoopJoin);
    Log("comandos coophost/coopjoin registrados");
}

}  // namespace

// ---------- ganchos (llamados desde los stubs de main.cpp) ----------

extern "C" void __cdecl Coop_SendPacket(int sock, const void* data, int len, const netadr_t* to)
{
    if (g_sock != INVALID_SOCKET) SendTo(ToSock(*to), data, len);
}

// Se llama cuando la cola loopback de `sock` esta vacia: devuelve paquetes UDP del motor y
// procesa los de control.
extern "C" int __cdecl Coop_GetPacket(int sock, msg_t* msg, netadr_t* from)
{
    if (!g_cmdsReady) RegisterCommands();
    PollCmdFile();
    StatsTick();
    if (g_sock == INVALID_SOCKET) return 0;
    if (!((g_role == HOST && sock == NS_SERVER) || (g_role == JOINER && sock == NS_CLIENT))) return 0;
    if (g_role == JOINER && !g_joined && GetTickCount() - g_lastJoinTry > 1000) JoinerSendJoin();

    static uint8_t buf[0x10000];
    for (;;) {
        sockaddr_in a;
        int alen = sizeof a;
        int n = recvfrom(g_sock, (char*)buf, sizeof buf, 0, (sockaddr*)&a, &alen);
        if (n <= 0) return 0;
        if (n >= 5 && *(uint32_t*)buf == CTRL) {
            ++g_statCtrl;
            Reader r{buf + 5, buf + n};
            if (g_role == HOST && buf[4] == M_JOIN) HostJoin(a, r);
            else if (g_role == JOINER) JoinerControl(buf[4], r);
            continue;
        }
        if (n > msg->maxsize) continue;
        if (g_role == JOINER && !g_joined) continue;
        memcpy(msg->data, buf, n);
        msg->cursize = n;
        *from = ToNet(a);
        ++g_statRecv;
        return 1;
    }
}

// Despues de SV_SendClientMessages (que solo atiende al slot 0): snapshots + estados a los remotos.
extern "C" void __cdecl Coop_HostFrame()
{
    if (g_role != HOST) return;
    for (int n = 1; n < g::MAX_CLIENTS; ++n) {
        if (!g_remote[n] || !g_remote[n]->active) continue;
        uint8_t* cl = g::Client(n);
        if (*(int*)(cl + g::CL_STATE) != 1) { g_remote[n]->active = false; continue; }
        FixClientNum(n);
        g::SV_SendClientSnapshot(cl);
        ++g_statSnaps;
        SendEntities(n);
    }
}

// En el que se une el servidor local no corre, pero el cliente del SP toma su reloj de sv.time
// (CL_Frame 0x40df80): lo avanzamos siguiendo al ultimo snapshot del host, un snapshot atras para interpolar.
extern "C" bool Coop_FrozenFrame(int msec)
{
    if (!g_frozen) return false;
    static int t;
    int latest = *(int*)0x1426e68;  // cl.snap.serverTime
    int target = latest - 50;
    t += msec;
    if (t < target - 300) t = target;  // al entrar o tras un corte: salto
    else if (t > latest) t = latest;   // nunca adelante del ultimo snapshot
    else if (t < target - 10) t += 1;  // se acerca de a poco
    *(int*)0x152e930 = t;               // sv.time
    return true;
}
