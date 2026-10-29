// Nucleo del coop (codcoop_core.dll, recargable en caliente por ddraw.dll):
// el host corre la campaña y los demas se conectan como clientes 1..3 de su servidor.
// Transporte UDP propio (el exe solo tiene loopback) + canal de control para lo que el SP
// comparte por memoria entre servidor y cliente (gamestate, estados de entidad, DObj, animaciones).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "api.h"
#include "game.h"

static const CoreApi* g_api;

void Log(const char* fmt, ...)
{
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (g_api) g_api->logLine(b);
}

// VM_Call 0x44f270: EAX = vm, 13 argumentos en la pila (ver game.h GameCall)
asm(R"(
.intel_syntax noprefix
.globl _CallVM
_CallVM:
    push esi
    push edi
    mov eax, [esp+12]
    mov esi, [esp+16]
    sub esp, 52
    mov edi, esp
    mov ecx, 13
    rep movsd
    mov edx, 0x44f270
    call edx
    add esp, 52
    pop edi
    pop esi
    ret
.att_syntax
)");

namespace {

constexpr uint32_t CTRL = 0xFFFFFFFE;  // prefijo de nuestros paquetes; el motor usa -1 (sin conexion) o una secuencia
enum : uint8_t { M_JOIN = 'J', M_ACCEPT = 'A', M_FULL = 'F', M_CS = 'C', M_CSDONE = 'G', M_ENTS = 'E', M_DOBJ = 'D', M_ANIM = 'N' };
constexpr int ANIM_STATE = 22;  // estado cliente de un XAnimInfo: +0x1c(4) +0x24(2) +0x28..+0x37(16)
constexpr int MAX_PACKET = 1400;

enum Role { NONE, HOST, JOINER };

// host: por slot remoto
struct Remote {
    bool active;
    sockaddr_in addr;
    uint8_t lastEnt[g::MAX_GENTITIES][g::ENTSTATE_SIZE];
    DWORD lastSent[g::MAX_GENTITIES];
    uint32_t dobjSig[g::MAX_GENTITIES];  // ultima forma de DObj mandada (0 = sin DObj)
    DWORD dobjSent[g::MAX_GENTITIES];
};

// Todo lo que tiene que sobrevivir a una recarga del nucleo. La memoria la guarda ddraw.dll; si cambia
// el tamaño o STATE_VERSION, el cargador la pone en cero (se pierde la conexion).
constexpr uint32_t STATE_VERSION = 1;
struct State {
    int role;
    SOCKET sock;
    Remote remote[g::MAX_CLIENTS];  // host
    bool god[g::MAX_CLIENTS];       // prueba: vida al maximo
    sockaddr_in host;               // joiner
    DWORD lastJoinTry;
    bool joined, frozen;
    int slot;
    int frozenTime;
    char hostMap[64];
    uint8_t* ownTree[g::MAX_GENTITIES];  // arboles de animacion creados por nosotros
    char cs[g::MAX_CONFIGSTRINGS][1024];
};
State* S;

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

bool OpenSocket(int port)
{
    if (S->sock != INVALID_SOCKET) return true;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    S->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(S->sock, FIONBIO, &nb);
    int buf = 1 << 20;
    setsockopt(S->sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf, sizeof buf);
    setsockopt(S->sock, SOL_SOCKET, SO_SNDBUF, (char*)&buf, sizeof buf);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (bind(S->sock, (sockaddr*)&a, sizeof a) != 0) {
        Log("bind %d fallo: %d", port, WSAGetLastError());
        closesocket(S->sock);
        S->sock = INVALID_SOCKET;
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
    sendto(S->sock, (const char*)data, len, 0, (const sockaddr*)&to, sizeof to);
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

const char* SafeStr(const void* p)
{
    return p && !IsBadReadPtr(p, 4) ? (const char*)p : "?";
}

// ---------- host ----------

int FindSlot(const sockaddr_in& from)
{
    for (int n = 1; n < g::MAX_CLIENTS; ++n)
        if (S->remote[n].active && S->remote[n].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
            S->remote[n].addr.sin_port == from.sin_port)
            return n;
    return -1;
}

void SendGamestate(int n)
{
    const sockaddr_in& to = S->remote[n].addr;
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

// El script de la mision solo arma a level.player: si un remoto no tiene armas (al entrar o al
// reaparecer) recibe el mismo equipo que el host. ps: +0xa0 arma actual, +0xf4 ammo[64],
// +0x1f4 cargador[64], +0x2f4 bits de armas, +0x2fc ranuras[6] (ver giveweapon gamex86 RVA 0x15020).
void GiveHostLoadout(int n)
{
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    if (!gameClients) return;
    uint8_t* host = gameClients;
    uint8_t* ps = gameClients + n * *(int*)0x152bc68;
    if (*(int*)(ps + 0xa0) || !*(int*)(host + 0xa0)) return;  // ya tiene un arma en la mano, o el host tampoco
    if (!*(uint64_t*)(ps + 0x2f4)) memcpy(ps + 0xf4, host + 0xf4, 0x304 - 0xf4);
    *(int*)(ps + 0xa0) = *(int*)(host + 0xa0);
    Log("jugador %d: recibe el equipo del host (arma %d)", n, *(int*)(ps + 0xa0));
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
        Remote* rm = &S->remote[n];
        memset(rm, 0, sizeof *rm);
        memset(rm->dobjSig, 0xff, sizeof rm->dobjSig);  // la primera vez se manda todo, incluso "sin DObj"
        rm->active = true;
        rm->addr = from;

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
    Remote* rm = &S->remote[n];
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

// ---------- DObj y animaciones (el SP los comparte por memoria entre servidor y cliente) ----------

uint32_t DObjSig(const uint8_t* d)
{
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, int len) {
        for (int i = 0; i < len; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    };
    uint8_t* tree = *(uint8_t**)d;
    void* xanim = tree ? *(void**)tree : nullptr;
    mix(d + 0x12, 2);
    mix(d + 0x16, 1);
    mix(d + 0x18, 0x20);
    mix(d + 0x38, 0x10);
    mix(d + 0x48, 8);
    mix(&xanim, 4);
    return h ? h : 1;
}

void PutStr(Writer& w, const char* s)
{
    int l = (int)strlen(s);
    if (l > 200) l = 200;
    w.Put<uint8_t>((uint8_t)l);
    w.Put(s, l);
}

bool GetStr(Reader& r, char* out, int size)
{
    int l = r.Get<uint8_t>();
    if (r.p + l > r.end || l >= size) return false;
    memcpy(out, r.p, l);
    out[l] = 0;
    r.p += l;
    return true;
}

// host: forma del DObj (cuando cambia o cada 2 s) y estado de animacion (cada snapshot) de lo visible.
// Se llama justo despues de SV_SendClientSnapshot, que ya copio el estado servidor -> cliente (0x44d8e0).
void SendDObjs(int n)
{
    Remote* rm = &S->remote[n];
    DWORD now = GetTickCount();
    Writer aw(M_ANIM);
    for (int i = 0; i < g::snap_numEntities; ++i) {
        int e = g::snap_entities[i];
        if (e < 0 || e >= g::MAX_GENTITIES) continue;
        uint8_t* d = g::ServerDObj(e);
        uint32_t sig = d ? DObjSig(d) : 0;
        if (sig != rm->dobjSig[e] || (sig && now - rm->dobjSent[e] > 2000)) {
            Writer w(M_DOBJ);
            int nm = d ? d[0x16] : 0;
            if (nm > 8) nm = 8;
            w.Put<uint16_t>((uint16_t)e);
            w.Put<uint8_t>((uint8_t)nm);
            w.Put<uint16_t>(d ? *(uint16_t*)(d + 0x12) : 0);
            uint8_t* tree = d ? *(uint8_t**)d : nullptr;
            PutStr(w, tree ? SafeStr(**(char***)tree) : "");
            for (int k = 0; k < nm; ++k) {
                uint8_t* model = *(uint8_t**)(d + 0x18 + k * 4);
                PutStr(w, model ? SafeStr(*(char**)model) : "");
                w.Put<uint8_t>(d[0x48 + k]);
                w.Put<uint16_t>(*(uint16_t*)(d + 0x38 + k * 2));
            }
            SendTo(rm->addr, w.buf, w.len);
            rm->dobjSig[e] = sig;
            rm->dobjSent[e] = now;
        }
        uint8_t* tree = d ? *(uint8_t**)d : nullptr;
        if (!tree) continue;
        int size = *(int*)(*(uint8_t**)tree + 4);
        int count = 0;
        for (int a = 0; a < size; ++a)
            if (*(uint16_t*)(tree + 8 + a * 2)) ++count;
        if (aw.len + 4 + count * (2 + ANIM_STATE) > MAX_PACKET) {
            SendTo(rm->addr, aw.buf, aw.len);
            aw = Writer(M_ANIM);
        }
        aw.Put<uint16_t>((uint16_t)e);
        aw.Put<uint16_t>((uint16_t)count);
        for (int a = 0; a < size; ++a) {
            uint16_t idx = *(uint16_t*)(tree + 8 + a * 2);
            if (!idx) continue;
            const uint8_t* info = g::xanimInfo + idx * 0x44;
            aw.Put<uint16_t>((uint16_t)a);
            aw.Put(info + 0x1c, 4);
            aw.Put(info + 0x24, 2);
            aw.Put(info + 0x28, 16);
        }
    }
    if (aw.len > 5) SendTo(rm->addr, aw.buf, aw.len);
}

void FreeTreeInfos(uint8_t* tree)
{
    int size = *(int*)(*(uint8_t**)tree + 4);
    for (int a = 0; a < size; ++a) {
        uint16_t* slot = (uint16_t*)(tree + 8 + a * 2);
        if (*slot) g::XAnimFreeInfo(*slot);
        *slot = 0;
    }
}

// El arbol se libera al final: DObjFree (0x477020) todavia lo usa, y el cliente suelta su referencia
// al DObj recien en Com_SyncClientDObjs.
void JoinerFreeDObj(int e)
{
    uint8_t* tree = S->ownTree[e];
    S->ownTree[e] = nullptr;
    if (g::serverObjMap[e]) g::Com_ServerDObjFree(e);
    g::Com_SyncClientDObjs();
    if (tree) {
        FreeTreeInfos(tree);
        g::XAnimFreeTree(tree);
    }
}

uint8_t* FindXAnim(const char* name)
{
    for (int i = 0; i < 256; ++i) {
        uint8_t* x = g::xanimTable[i];
        if (x && !_stricmp(SafeStr(*(char**)x), name)) return x;
    }
    return nullptr;
}

void JoinerApplyDObj(Reader r)
{
    struct ModelEntry {
        void* model;
        const char* tag;
        uint16_t flags;
        uint16_t pad;
    } models[8] = {};
    uint8_t parents[8];
    char name[256], animName[256];
    int e = r.Get<uint16_t>(), nm = r.Get<uint8_t>(), u12 = r.Get<uint16_t>();
    if (e >= g::MAX_GENTITIES || nm > 8 || !GetStr(r, animName, sizeof animName)) return;
    for (int k = 0; k < nm; ++k) {
        if (!GetStr(r, name, sizeof name)) return;
        parents[k] = r.Get<uint8_t>();
        models[k].flags = r.Get<uint16_t>();
        models[k].model = g::XModelPrecache(name);  // si no estaba cargado lo carga
        if (!models[k].model || _stricmp(SafeStr(*(char**)models[k].model), name) != 0) {
            Log("dobj %d: modelo '%s' no resuelto", e, name);
            return;
        }
    }
    if (!r.Ok()) return;
    if (!nm) {
        JoinerFreeDObj(e);
        return;
    }
    uint8_t* xanim = animName[0] ? FindXAnim(animName) : nullptr;
    if (animName[0] && !xanim) Log("dobj %d: no hay arbol de animacion '%s' aca", e, animName);
    uint8_t* d = g::ServerDObj(e);
    bool same = d && d[0x16] == nm && (*(uint8_t**)d ? **(uint8_t***)d : nullptr) == xanim;
    for (int k = 0; same && k < nm; ++k) same = *(void**)(d + 0x18 + k * 4) == models[k].model;
    if (!same) {
        JoinerFreeDObj(e);
        uint8_t* tree = xanim ? g::XAnimCreateTree(xanim) : nullptr;
        S->ownTree[e] = tree;
        g::Com_ServerDObjCreate(models, nm, tree, e, u12);
        d = g::ServerDObj(e);
        if (!d) return;
    }
    for (int k = 0; k < nm; ++k) {
        d[0x48 + k] = parents[k];
        *(uint16_t*)(d + 0x38 + k * 2) = models[k].flags;
    }
}

void JoinerApplyAnims(Reader r)
{
    static uint8_t seen[4096];
    while (r.p + 4 <= r.end) {
        int e = r.Get<uint16_t>(), count = r.Get<uint16_t>();
        if (r.p + count * (2 + ANIM_STATE) > r.end || e >= g::MAX_GENTITIES) return;
        uint8_t* d = g::ServerDObj(e);
        uint8_t* tree = d ? *(uint8_t**)d : nullptr;
        int size = tree ? *(int*)(*(uint8_t**)tree + 4) : 0;
        if (size > (int)sizeof seen) size = 0;
        memset(seen, 0, size);
        for (int k = 0; k < count; ++k) {
            int a = r.Get<uint16_t>();
            const uint8_t* st = r.p;
            r.p += ANIM_STATE;
            if (a >= size) continue;
            seen[a] = 1;
            uint16_t* slot = (uint16_t*)(tree + 8 + a * 2);
            if (!*slot) {  // como XAnimSetGoalWeight (0x47dd90) cuando el nodo no tenia info
                uint8_t* p = g::XAnimAllocInfo(tree, a);
                memset(p + 0xc, 0, 0x38);
                *(uint16_t*)(p + 4) = 0;
                *(uint16_t*)(p + 2) = 0xffff;
                *(uint16_t*)p = 0;
                *(uint16_t*)(p + 6) = 0;
            }
            uint8_t* info = g::xanimInfo + *slot * 0x44;
            memcpy(info + 0x1c, st, 4);
            memcpy(info + 0x24, st + 4, 2);
            memcpy(info + 0x28, st + 6, 16);
        }
        for (int a = 0; a < size; ++a) {
            uint16_t* slot = (uint16_t*)(tree + 8 + a * 2);
            if (*slot && !seen[a]) {
                g::XAnimFreeInfo(*slot);
                *slot = 0;
            }
        }
    }
}

// ---------- joiner ----------

void JoinerSendJoin()
{
    Writer w(M_JOIN);
    w.Put<int>(*(int*)(g::clc_netchan + g::NC_QPORT));
    w.Put<int>(*(int*)(g::clc_netchan + g::NC_IN));
    w.Put<int>(g::clc_serverCommandSequence);
    w.Put<int>(g::clc_reliableSequence);
    SendTo(S->host, w.buf, w.len);
    S->lastJoinTry = GetTickCount();
}

void JoinerHijack()
{
    if (strcmp(S->hostMap, g::CvarString("mapname")) != 0) {
        g::Com_Printf("^1COOP: el host esta en '%s' y vos en '%s'. Carga el mismo mapa y repeti coopjoin.\n",
                      S->hostMap, g::CvarString("mapname"));
        Log("joiner: mapa distinto (host %s, local %s)", S->hostMap, g::CvarString("mapname"));
        S->role = NONE;
        return;
    }
    S->frozen = true;                              // el servidor local queda solo para colision y assets
    *(int*)(g::Client(0) + g::CL_STATE) = 0;      // que no acumule comandos que nadie confirma
    for (int i = 0; i < g::MAX_CONFIGSTRINGS; ++i)
        if (strcmp(g::ConfigString(i), S->cs[i]) != 0) g::SV_SetConfigstring(i, S->cs[i]);
    int state = g::cls_state;
    g::CL_NetchanSetup(ToNet(S->host));
    g::cls_state = state;
    g::CL_ApplyConfigstrings(g::sv_configstrings);
    S->joined = true;
    Log("joiner: conectado al host como slot %d en %s", S->slot, S->hostMap);
    g::Com_Printf("^2COOP: conectado al host (jugador %d)\n", S->slot + 1);
}

void JoinerControl(uint8_t type, Reader r)
{
    switch (type) {
    case M_ACCEPT:
        if (S->joined) return;
        S->slot = r.Get<int>();
        strncpy(S->hostMap, (const char*)r.p, sizeof S->hostMap - 1);
        memset(S->cs, 0, sizeof S->cs);
        break;
    case M_FULL:
        g::Com_Printf("^1COOP: el host esta lleno (4 jugadores)\n");
        S->role = NONE;
        break;
    case M_CS:
        while (r.p + 4 <= r.end) {
            int i = r.Get<uint16_t>(), l = r.Get<uint16_t>();
            if (i >= g::MAX_CONFIGSTRINGS || l > 1000 || r.p + l > r.end) break;
            memcpy(S->cs[i], r.p, l);
            S->cs[i][l] = 0;
            r.p += l;
        }
        break;
    case M_CSDONE:
        if (!S->joined && S->hostMap[0]) JoinerHijack();
        break;
    case M_DOBJ:
        if (S->joined) JoinerApplyDObj(r);
        break;
    case M_ANIM:
        if (S->joined) JoinerApplyAnims(r);
        break;
    case M_ENTS: {
        if (!S->joined) return;
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
    S->role = HOST;
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
    S->host = *(sockaddr_in*)res->ai_addr;
    S->host.sin_port = htons((u_short)port);
    freeaddrinfo(res);
    S->role = JOINER;
    S->joined = false;
    S->hostMap[0] = 0;
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
    if (g::cmd_argc >= 5)  // cooptp-style: coopbring n x y z
        for (int i = 0; i < 3; ++i) dst[i] = (float)atof(g::Argv(2 + i));
    memset(dst + 3, 0, 12);  // velocidad
    Log("coopbring %d -> (%.0f %.0f %.0f)", n, dst[0], dst[1], dst[2]);
}

// Lista los DObj del servidor: modelos, arbol de animacion y nodos activos (para el tramo 2).
void Cmd_CoopDObj()
{
    const int16_t* serverObjMap = (const int16_t*)0x886f10;
    int shown = 0;
    for (int e = 0; e < g::MAX_GENTITIES && shown < 60; ++e) {
        if (!serverObjMap[e]) continue;
        uint8_t* d = (uint8_t*)0x888020 + serverObjMap[e] * 0x58;
        char models[512];
        int len = 0;
        int nm = d[0x16];
        for (int i = 0; i < nm && i < 8; ++i) {
            uint8_t* m = *(uint8_t**)(d + 0x18 + i * 4);
            len += snprintf(models + len, sizeof models - len, " %s(p%d)", m ? SafeStr(*(char**)m) : "null", d[0x48 + i]);
        }
        uint8_t* tree = *(uint8_t**)d;
        int active = 0, size = 0;
        const char* animName = "-";
        if (tree) {
            uint8_t* xanim = *(uint8_t**)tree;
            size = *(int*)(xanim + 4);
            animName = SafeStr(*(char**)xanim);
            for (int i = 0; i < size; ++i)
                if (*(uint16_t*)(tree + 8 + i * 2)) ++active;
        }
        const float* org = (const float*)(g::Entity(e) + 0xe0);  // r.currentOrigin
        Log("dobj ent %d (%.0f %.0f %.0f): modelos %d:%s arbol %p anims '%s' n=%d activos=%d", e, org[0], org[1], org[2], nm,
            models, tree, animName, size, active);
        ++shown;
    }
    Log("dobj: %d infos de animacion en uso", *(int*)0xa6d000);
}

// prueba: coopgod <n> [0|1] -> el host le mantiene la vida al maximo al jugador n
void Cmd_CoopGod()
{
    int n = atoi(g::Argv(1));
    if (n < 0 || n >= g::MAX_CLIENTS) return;
    S->god[n] = g::cmd_argc > 2 ? atoi(g::Argv(2)) != 0 : !S->god[n];
    Log("coopgod %d = %d", n, S->god[n]);
}

// prueba: cooplook <n> <ent> -> jugador n a 150 unidades de la entidad y mirandola.
// La vista es SHORT2ANGLE(cmd.angles + ps.delta_angles): se ajusta delta_angles (ps+0x48, como Q3).
void Cmd_CoopLook()
{
    int n = atoi(g::Argv(1)), e = atoi(g::Argv(2));
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    if (n < 0 || n >= g::MAX_CLIENTS || e < 0 || e >= g::MAX_GENTITIES || !gameClients) return;
    uint8_t* ps = gameClients + n * *(int*)0x152bc68;
    const float* t = (const float*)(g::Entity(e) + 0xe0);  // r.currentOrigin
    float* o = (float*)(ps + 0x14);
    float dx = o[0] - t[0], dy = o[1] - t[1], len = sqrtf(dx * dx + dy * dy);
    if (len < 1) dx = -1, dy = 0, len = 1;
    o[0] = t[0] + dx / len * 150;
    o[1] = t[1] + dy / len * 150;
    o[2] = t[2] + 8;
    memset(ps + 0x20, 0, 12);  // velocidad
    float yaw = atan2f(-dy, -dx) * 180.0f / 3.14159265f;
    const int* cmdAngles = (const int*)(ps + 0x73e4 + 8);  // gclient.pers.cmd.angles
    int* delta = (int*)(ps + 0x48);
    delta[0] = -cmdAngles[0];
    delta[1] = ((int)(yaw * 65536 / 360) & 0xffff) - cmdAngles[1];
    Log("cooplook %d -> ent %d en (%.0f %.0f %.0f), yaw %.0f", n, e, o[0], o[1], o[2], yaw);
}

struct CmdDef {
    const char* name;
    void (*fn)();
};
const CmdDef kCmds[] = {
    {"coophost", Cmd_CoopHost}, {"coopjoin", Cmd_CoopJoin}, {"coopinfo", Cmd_CoopInfo}, {"coopbring", Cmd_CoopBring},
    {"coopdobj", Cmd_CoopDObj}, {"coopgod", Cmd_CoopGod},   {"cooplook", Cmd_CoopLook},
};

}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT uint32_t Coop_StateVersion() { return (uint32_t)sizeof(State) ^ (STATE_VERSION << 28); }
EXPORT int Coop_StateSize() { return (int)sizeof(State); }

EXPORT void Coop_Init(const CoreApi* api, void* state, int fresh)
{
    g_api = api;
    S = (State*)state;
    if (fresh) {
        S->sock = INVALID_SOCKET;
        S->role = NONE;
    }
    Log("nucleo listo (rol %d, %s)", S->role, fresh ? "estado nuevo" : "estado conservado");
}

EXPORT void Coop_Shutdown()
{
    Log("nucleo descargado");
    g_api = nullptr;
}

EXPORT const char* Coop_Commands()
{
    static char list[256];
    if (!list[0])
        for (const CmdDef& c : kCmds) {
            strcat(list, c.name);
            strcat(list, " ");
        }
    return list;
}

EXPORT void Coop_Command(const char* name)
{
    for (const CmdDef& c : kCmds)
        if (!_stricmp(c.name, name)) return c.fn();
}

// ---------- ganchos (los llama ddraw.dll) ----------

EXPORT void __cdecl Coop_SendPacket(int sock, const void* data, int len, const netadr_t* to)
{
    if (S->sock != INVALID_SOCKET) SendTo(ToSock(*to), data, len);
}

// Se llama cuando la cola loopback de `sock` esta vacia: devuelve paquetes UDP del motor y
// procesa los de control.
EXPORT int __cdecl Coop_GetPacket(int sock, msg_t* msg, netadr_t* from)
{
    PollCmdFile();
    StatsTick();
    if (S->sock == INVALID_SOCKET) return 0;
    if (!((S->role == HOST && sock == NS_SERVER) || (S->role == JOINER && sock == NS_CLIENT))) return 0;
    if (S->role == JOINER && !S->joined && GetTickCount() - S->lastJoinTry > 1000) JoinerSendJoin();

    static uint8_t buf[0x10000];
    for (;;) {
        sockaddr_in a;
        int alen = sizeof a;
        int n = recvfrom(S->sock, (char*)buf, sizeof buf, 0, (sockaddr*)&a, &alen);
        if (n <= 0) return 0;
        if (n >= 5 && *(uint32_t*)buf == CTRL) {
            ++g_statCtrl;
            Reader r{buf + 5, buf + n};
            if (S->role == HOST && buf[4] == M_JOIN) HostJoin(a, r);
            else if (S->role == JOINER) JoinerControl(buf[4], r);
            continue;
        }
        if (n > msg->maxsize) continue;
        if (S->role == JOINER && !S->joined) continue;
        memcpy(msg->data, buf, n);
        msg->cursize = n;
        *from = ToNet(a);
        ++g_statRecv;
        return 1;
    }
}

// Despues de SV_SendClientMessages (que solo atiende al slot 0): snapshots + estados a los remotos.
EXPORT void __cdecl Coop_HostFrame()
{
    if (S->role != HOST) return;
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        if (!S->god[n] || !gameClients || *(int*)(g::Client(n) + g::CL_STATE) != 1) continue;
        uint8_t* ps = gameClients + n * *(int*)0x152bc68;
        *(int*)(ps + 0xe4) = *(int*)(ps + 0xec);                // ps.stats vida = maxima
        *(int*)(g::Entity(n) + 0x1d8) = *(int*)(ps + 0xec);     // ent->health (ver ClientSpawn)
    }
    for (int n = 1; n < g::MAX_CLIENTS; ++n) {
        if (!S->remote[n].active) continue;
        uint8_t* cl = g::Client(n);
        if (*(int*)(cl + g::CL_STATE) != 1) { S->remote[n].active = false; continue; }
        FixClientNum(n);
        GiveHostLoadout(n);
        g::SV_SendClientSnapshot(cl);
        ++g_statSnaps;
        SendEntities(n);
        SendDObjs(n);
    }
}

// En el que se une el servidor local no corre, pero el cliente del SP toma su reloj de sv.time
// (CL_Frame 0x40df80): lo avanzamos siguiendo al ultimo snapshot del host, un snapshot atras para interpolar.
EXPORT int __cdecl Coop_FrozenFrame(int msec)
{
    if (!S->frozen) return 0;
    int& t = S->frozenTime;
    int latest = *(int*)0x1426e68;  // cl.snap.serverTime
    int target = latest - 50;
    t += msec;
    if (t < target - 300) t = target;  // al entrar o tras un corte: salto
    else if (t > latest) t = latest;   // nunca adelante del ultimo snapshot
    else if (t < target - 10) t += 1;  // se acerca de a poco
    *(int*)0x152e930 = t;               // sv.time
    g::Com_SyncClientDObjs();           // lo hace un trap del juego, que aca no corre
    return 1;
}
