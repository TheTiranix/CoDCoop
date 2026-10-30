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
#include "hook.h"

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
enum : uint8_t { M_JOIN = 'J', M_ACCEPT = 'A', M_FULL = 'F', M_CS = 'C', M_CSDONE = 'G', M_ENTS = 'E', M_DOBJ = 'D', M_ANIM = 'N',
                 M_DOWNED = 'W', M_RELOAD = 'R' };
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
constexpr uint32_t STATE_VERSION = 3;
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
    uint8_t* bodyTree[g::MAX_CLIENTS];   // host: arbol del cuerpo del jugador n (0 = sin cuerpo)
    bool downed[g::MAX_CLIENTS];         // host: caido esperando que lo levanten
    float reviveTime[g::MAX_CLIENTS];    // host: segundos que un compañero lleva levantandolo
    char cs[g::MAX_CONFIGSTRINGS][1024];
};
State* S;

struct ModelEntry {  // lo que recibe Com_ServerDObjCreate (0x476e00 resuelve tag -> hueso padre)
    void* model;
    const char* tag;
    uint16_t flags;
    uint16_t pad;
};

int BrokenPathLists(int* lists = nullptr);

// diagnostico: contadores que se vuelcan al log cada 5 s
int g_statSnaps, g_statEnts, g_statRecv, g_statCtrl;
void StatsTick()
{
    static DWORD last;
    if (GetTickCount() - last < 5000) return;
    last = GetTickCount();
    if (S->role == HOST) {
        static int lastBad;
        int bad = BrokenPathLists();
        if (bad != lastBad) Log("listas de caminos rotas: %d -> %d", lastBad, bad);
        lastBad = bad;
    }
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

// Brazos en primera persona (ps+0xac viewmodelIndex): el script del nivel hace setviewmodel solo al jugador 0 y el
// remoto queda con el de por defecto (viewmodel_hands_us), que el nivel no cargo y se ve blanco.
void CopyHostViewmodel(int n)
{
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    if (gameClients) *(int*)(gameClients + n * *(int*)0x152bc68 + 0xac) = *(int*)(gameClients + 0xac);
}

// Brujula: el cgame dibuja a los aliados de la IA desde una tabla de 32 lugares (cgame+0x12601c, 16 bytes c/u: hora,
// x, y, rumbo; se ve si la hora tiene menos de 800 ms) indexada por numero de actor. Los compañeros van en los
// ultimos lugares (31 - jugador); solo chocarian con un aliado de la IA con indice de actor 28 o mas.
void DrawTeamOnCompass(uint8_t mask, const float (*pos)[3], int self)
{
    uint8_t* cg = (uint8_t*)GetModuleHandleA("cgamex86.dll");
    if (!cg) return;
    int now = *(int*)(cg + 0x11e0a4);  // cg.time
    for (int p = 0; p < g::MAX_CLIENTS; ++p) {
        if (p == self || !(mask >> p & 1)) continue;
        int32_t* slot = (int32_t*)(cg + 0x12601c) + (31 - p) * 4;
        slot[0] = now;
        memcpy(slot + 1, pos[p], 12);
    }
}
uint8_t g_team;                            // remoto: jugadores en el mapa segun el host (mascara)
float g_teamPos[g::MAX_CLIENTS][3];        // x, y, rumbo de cada uno

// diagnostico: cuantas listas de enlaces de caminos (gentity+0x2b6) estan rotas (ver Cmd_CoopLists)
int BrokenPathLists(int* lists)
{
    uint8_t* game = (uint8_t*)GetModuleHandleA("gamex86.dll");
    if (!game || !g::sv_gentities) return 0;
    uint8_t* nodes = *(uint8_t**)(game + 0x2dc1c0);
    int bad = 0, count = 0;
    for (int e = 0; e < g::sv_numEntities; ++e) {
        uint8_t* ent = g::Entity(e);
        uint16_t head = *(uint16_t*)(ent + 0x2b6), cur = head;
        if (!head) continue;
        ++count;
        int steps = 0;
        do {
            uint16_t next = *(uint16_t*)(nodes + cur * 8 + 6);
            if (!next || *(uint16_t*)(nodes + next * 8 + 4) != cur || ++steps > 4096) { ++bad; break; }
            cur = next;
        } while (cur != head);
    }
    if (lists) *lists = count;
    return bad;
}

void CheckPathLists(const char* when)
{
    int lists, bad = BrokenPathLists(&lists);
    Log("listas de caminos %s: %d, rotas %d", when, lists, bad);
}

void HostJoin(const sockaddr_in& from, Reader r)
{
    int qport = r.Get<int>(), lastIncoming = r.Get<int>(), serverCmdSeq = r.Get<int>(), relSeq = r.Get<int>();
    if (!r.Ok()) return;
    if (g::sv_state != 2) return;  // sin partida no hay a donde unirse
    uint8_t* reloading = g::Cvar_FindVar("g_reloading");
    if (reloading && *(int*)(reloading + 0x20)) return;  // mision fallida: esperar a que cargue el checkpoint
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
        // ClientConnect (gamex86 RVA 0x14800) empieza reiniciando el sistema de caminos de la IA (call 0x6fde0 en
        // RVA 0x14808): en el SP pasa una vez al cargar el mapa. Mas tarde deja a las puertas que bloquean caminos
        // apuntando a nodos liberados, y la carga de partida se cuelga en G_FreeEntity. Para los remotos se saltea.
        uint8_t* game = (uint8_t*)GetModuleHandleA("gamex86.dll");
        uint8_t* call = game ? game + 0x14808 : nullptr;
        uint8_t saved[5] = {};
        bool skip = call && call[0] == 0xE8 && call + 5 + *(int32_t*)(call + 1) == game + 0x6fde0;
        if (skip) {
            memcpy(saved, call, 5);
            hook::Write((uintptr_t)call, "\x0F\x1F\x44\x00\x00", 5);  // nop de 5 bytes
        } else {
            Log("ClientConnect: no se encontro la llamada a la init de caminos; se llama completo");
        }
        g::GameCall(2, n);
        if (skip) hook::Write((uintptr_t)call, saved, 5);
        CheckPathLists("despues de ClientConnect");
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
    ModelEntry models[8] = {};
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

// ---------- cuerpos de los jugadores (el SP no tiene: el jugador nunca se ve a si mismo) ----------

// host: DObj de soldado aliado para la entidad del jugador n. Los modelos salen de character/Airborne*.gsc
// (cuerpo + cabeza + casco, unidos al esqueleto del cuerpo). shortcut: siempre paracaidista de EE.UU.,
// falta elegir britanico/sovietico segun la campaña (fase 5).
// Indice de modelo (configstrings CS_MODELS 31..286) que el cliente usa para encontrar el modelo de dibujo
// de cada parte de un DObj (DObj+0x38 = -indice). Si el mapa no lo precargo, se registra en el primer
// lugar libre (el G_ModelIndex del juego salta los ocupados).
constexpr int CS_MODELS = 31, MAX_MODELS = 256;
int ModelIndex(const char* name)
{
    char full[128];
    snprintf(full, sizeof full, "xmodel/%s", name);
    int empty = 0;
    for (int i = 1; i < MAX_MODELS; ++i) {
        const char* s = g::ConfigString(CS_MODELS + i);
        if (!*s) {
            if (!empty) empty = i;
        } else if (!_stricmp(s, full)) {
            return i;
        }
    }
    if (!empty) return 0;
    g::SV_SetConfigstring(CS_MODELS + empty, full);
    Log("modelo %s registrado en el indice %d", full, empty);
    return empty;
}

// Modelo del arma en la mano del jugador n: ps.weapon (+0xa0) -> WeaponDef (tabla en gamex86 RVA 0x125650,
// ver giveweapon RVA 0x15020) -> worldModel (+0x188, "xmodel/..."). "" si no tiene arma.
const char* WeaponModelName(int n)
{
    uint8_t* game = (uint8_t*)GetModuleHandleA("gamex86.dll");
    uint8_t** defs = game ? *(uint8_t***)(game + 0x125650) : nullptr;
    int weapon = *(int*)(*(uint8_t**)0x152bc64 + n * *(int*)0x152bc68 + 0xa0);
    if (!defs || weapon <= 0 || weapon >= 64 || !defs[weapon]) return "";
    const char* name = *(const char**)(defs[weapon] + 0x188);
    if (!name || IsBadReadPtr(name, 8)) return "";
    return _strnicmp(name, "xmodel/", 7) == 0 ? name + 7 : name;
}

int g_bodyAnim[g::MAX_CLIENTS] = {-1, -1, -1, -1};  // animacion puesta (-1 = ninguna; tras recargar se vuelve a poner)

void HostCreateBody(int n)
{
    g_bodyAnim[n] = -1;  // arbol nuevo (puede caer en la misma direccion que el viejo)
    static const char* const kParts[g::MAX_CLIENTS][3] = {
        {"character_airborne", "basehead4_04", "gear_US_helmet"},
        {"character_airborne", "basehead_01", "gear_US_helmet_net"},
        {"character_airborne_101", "basehead2_02", "gear_US_helmet"},
        {"character_airborneslvup", "basehead3_03", "gear_US_helmet_scrim"},
    };
    const char* names[4] = {kParts[n][0], kParts[n][1], kParts[n][2], WeaponModelName(n)};
    int count = names[3][0] ? 4 : 3;
    ModelEntry models[4] = {};
    for (int k = 0; k < count; ++k) {
        models[k].model = g::XModelPrecache(names[k]);
        int index = ModelIndex(names[k]);
        if (!models[k].model || !index) {
            Log("cuerpo %d: falta el modelo %s", n, names[k]);
            return;
        }
        models[k].flags = (uint16_t)-index;
    }
    models[3].tag = "tag_weapon_right";
    uint8_t* xanim = FindXAnim("generic_human.atr");
    uint8_t* tree = xanim ? g::XAnimCreateTree(xanim) : nullptr;
    g::Com_ServerDObjCreate(models, count, tree, n, 0);
    S->bodyTree[n] = tree;
    Log("cuerpo %d: creado (arbol %p, arma '%s')", n, tree, names[3]);
}

// el DObj del cuerpo tiene el arma que el jugador lleva en la mano?
bool BodyWeaponMatches(int n)
{
    uint8_t* d = g::ServerDObj(n);
    const char* want = WeaponModelName(n);
    if (!d) return true;
    if (!want[0]) return d[0x16] == 3;
    uint8_t* model = d[0x16] == 4 ? *(uint8_t**)(d + 0x18 + 3 * 4) : nullptr;
    return model && !_stricmp(SafeStr(*(char**)model), want);
}

void HostFreeBody(int n)
{
    uint8_t* tree = S->bodyTree[n];
    S->bodyTree[n] = nullptr;
    if (g::serverObjMap[n]) g::Com_ServerDObjFree(n);
    g::Com_SyncClientDObjs();
    if (tree) {
        FreeTreeInfos(tree);
        g::XAnimFreeTree(tree);
    }
    Log("cuerpo %d: borrado", n);
}

bool g_bodyDraw = true;  // prueba: coopbodydraw 0 -> sin cuerpos, los jugadores como en el SP
void Cmd_CoopBodyDraw()
{
    g_bodyDraw = atoi(g::Argv(1)) != 0;
}

// Con algun remoto conectado cada jugador tiene cuerpo; solo, como en el SP.
void HostUpdateBodies()
{
    if (!g::svs_clients) return;
    bool anyRemote = false;
    for (int n = 1; n < g::MAX_CLIENTS; ++n) anyRemote |= S->remote[n].active;
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        bool want = g_bodyDraw && anyRemote && *(int*)(g::Client(n) + g::CL_STATE) == 1;
        bool has = S->bodyTree[n] != nullptr;
        // tambien si el juego borro el DObj, o si cambio el arma (se rearma con la nueva)
        if (has && (!want || !g::serverObjMap[n] || !BodyWeaponMatches(n))) HostFreeBody(n);
        if (want && !S->bodyTree[n] && !g::serverObjMap[n]) HostCreateBody(n);
    }
}

}  // namespace

// cgame (host y remoto): el trap 0xf8 devuelve el entityState de la memoria del servidor. Los jugadores
// (eType 1) no se dibujan; a los demas jugadores con cuerpo se los da como entidad general (eType 0), que
// dibuja su DObj, y sin inclinacion (los angulos son los de la vista).
extern "C" uint8_t* CgEntityState(const int* args)
{
    static uint8_t copy[g::MAX_CLIENTS][g::ENTSTATE_SIZE];
    int e = args[1];
    uint8_t* st = g::Entity(e);
    int self = S->role == JOINER ? S->slot : 0;
    if (!g_bodyDraw || S->role == NONE || e < 0 || e >= g::MAX_CLIENTS || e == self || st[0] != 1 || !g::serverObjMap[e]) return st;
    uint8_t* c = copy[e];
    memcpy(c, st, g::ENTSTATE_SIZE);
    c[0] = 0;
    *(float*)(c + 0x44) = 0;  // apos.trBase: pitch y roll
    *(float*)(c + 0x4c) = 0;
    return c;
}

// XAnim del lado servidor (el lado lo elige inGameCall): las mismas funciones que usa el juego para la IA.
//   BodyAnimKnob: 0x47d8b0 (ECX arbol, EAX anim; goalWeight, blendTime, rate, notify x3) pone la animacion
//                 con su cadena de padres y apaga las hermanas.
//   BodyAnimUpdate: 0x47ccd0 (ECX DObj; dtime, notify) avanza el tiempo.
asm(R"(
.intel_syntax noprefix
.globl _BodyAnimKnob
_BodyAnimKnob:
    mov ecx, [esp+4]
    mov eax, [esp+8]
    push 0
    push 0
    push 0
    push dword ptr [esp+0x20]
    push dword ptr [esp+0x20]
    push dword ptr [esp+0x20]
    mov edx, 0x47d8b0
    call edx
    add esp, 24
    ret
.globl _BodyAnimUpdate
_BodyAnimUpdate:
    mov ecx, [esp+4]
    push 0
    push dword ptr [esp+0xc]
    mov eax, 0x47ccd0
    call eax
    add esp, 8
    ret
.att_syntax
)");
extern "C" void BodyAnimKnob(uint8_t* tree, int anim, float goalWeight, float blendTime, float rate);
extern "C" void BodyAnimUpdate(uint8_t* dobj, float dtime);

asm(R"(
.intel_syntax noprefix
.globl _CgSyscallHook
_CgSyscallHook:
    mov eax, [esp+4]
    cmp dword ptr [eax], 0xf8
    jne 1f
    push eax
    call _CgEntityState
    add esp, 4
    ret
1:  mov eax, 0x401a50
    jmp eax
.att_syntax
)");
extern "C" void CgSyscallHook();

namespace {

// cgvm->systemCall (primer campo del vm_t) se rearma cada vez que el motor recrea el cgame
constexpr uintptr_t CL_CGAME_SYSCALLS = 0x401a50;
void HookCgame(bool on)
{
    void** vm = *(void***)0x14bd620;
    if (!vm) return;
    if (on && *vm == (void*)CL_CGAME_SYSCALLS) *vm = (void*)CgSyscallHook;
    if (!on && *vm == (void*)CgSyscallHook) *vm = (void*)CL_CGAME_SYSCALLS;
}

// ---------- animacion de los cuerpos (1.3) ----------

enum BodyAnim { BA_IDLE, BA_WALK, BA_RUN, BA_BACK, BA_LEFT, BA_RIGHT, BA_CROUCH, BA_CROUCHWALK, BA_CROUCHRUN, BA_PRONE,
                BA_CRAWL, BA_DEATH, BA_FIRE, BA_CROUCHFIRE, BA_PRONEFIRE, BA_DOWNED, BA_COUNT };
const char* const kBodyAnimName[BA_COUNT] = {
    "stand_aim_straight", "stand_walk_combat_loop_01", "combatrun_forward_1", "combatrun_back",
    "combatrun_left",     "combatrun_right",           "crouch_aim_straight", "crouchwalk_loop",
    "crouchrun_loop_forward_1", "prone_aim_idle",      "prone_crawl",         "death_stand_dropinplace",
    "stand_shoot_straight", "crouch_shoot_straight",   "prone_shoot_straight",  "wounded_crawlidle",
};
constexpr int EF_FIRING = 0x400;  // eFlags mientras dispara (visto: 0x18 -> 0x418)

int AnimIndex(uint8_t* xanim, const char* name)
{
    int size = *(int*)(xanim + 4);
    for (int i = 0; i < size; ++i) {
        uint8_t* node = xanim + 8 + i * 8;
        uint8_t* parts = *(uint16_t*)node ? nullptr : *(uint8_t**)(node + 4);
        if (parts && !_stricmp(SafeStr(*(char**)parts), name)) return i;
    }
    return -1;
}

// Postura (ps.pm_flags: 1 cuerpo a tierra, 2 agachado), vida y velocidad del jugador -> animacion
int ChooseBodyAnim(int n)
{
    uint8_t* ps = *(uint8_t**)0x152bc64 + n * *(int*)0x152bc68;
    const float* v = (const float*)(ps + 0x20);
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
    int flags = *(int*)(ps + 0xc);
    bool firing = *(int*)(g::Entity(n) + 0x10) & EF_FIRING;
    if (S->downed[n]) return BA_DOWNED;
    if (*(int*)(ps + 0xe4) <= 0) return BA_DEATH;
    if (flags & 1) return speed > 5 ? BA_CRAWL : firing ? BA_PRONEFIRE : BA_PRONE;
    if (flags & 2) return speed >= 5 ? (speed < 100 ? BA_CROUCHWALK : BA_CROUCHRUN) : firing ? BA_CROUCHFIRE : BA_CROUCH;
    if (speed < 5) return firing ? BA_FIRE : BA_IDLE;
    float yaw = *(const float*)(g::Entity(n) + 0x48);  // apos.trBase[YAW]
    float rel = atan2f(v[1], v[0]) * 180.0f / 3.14159265f - yaw;
    rel -= 360.0f * floorf((rel + 180.0f) / 360.0f);  // a [-180, 180)
    if (fabsf(rel) <= 50) return speed < 120 ? BA_WALK : BA_RUN;
    if (fabsf(rel) >= 130) return BA_BACK;
    return rel > 0 ? BA_LEFT : BA_RIGHT;
}

// hubo un tiro desde la ultima vez? Cada tiro baja un cargador (ps.ammoclip[64] en +0x1f4, indexado por
// cargador, no por arma): se mira la suma.
bool NewShot(int n)
{
    static int lastClip[g::MAX_CLIENTS];
    const int* clips = (const int*)(*(uint8_t**)0x152bc64 + n * *(int*)0x152bc68 + 0x1f4);
    int clip = 0;
    for (int i = 0; i < 64; ++i) clip += clips[i];
    bool shot = clip < lastClip[n];
    lastClip[n] = clip;
    return shot;
}

void HostAnimateBodies(float dt)
{
    uint8_t* xanim = FindXAnim("generic_human.atr");
    if (!xanim) return;
    int prevSide = g::inGameCall;
    g::inGameCall = 1;  // lado servidor; el cliente lo copia en cada snapshot (0x47b010)
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        uint8_t* d = g::ServerDObj(n);
        uint8_t* tree = S->bodyTree[n];
        if (!tree || !d || *(uint8_t**)d != tree) { g_bodyAnim[n] = -1; continue; }
        int want = ChooseBodyAnim(n);
        int size = *(int*)(xanim + 4);
        if (want != g_bodyAnim[n]) {
            // el knob solo apaga las hermanas de la hoja: se apaga todo y se enciende la cadena nueva
            for (int a = 1; a < size; ++a)
                if (uint16_t idx = *(uint16_t*)(tree + 8 + a * 2)) {
                    uint8_t* info = g::xanimInfo + idx * 0x44;
                    *(float*)(info + 0x44) = *(float*)(info + 0x48) = *(float*)(info + 0x4c) = 0;
                }
            int index = AnimIndex(xanim, kBodyAnimName[want]);
            // shortcut: sin mezcla (blendTime 0); 0x47ccd0 sin notify no lleva el peso hacia la meta
            if (index > 0) BodyAnimKnob(tree, index, 1.0f, 0.0f, 1.0f);
            else Log("cuerpo %d: no esta la animacion %s", n, kBodyAnimName[want]);
            g_bodyAnim[n] = want;
        } else if ((want == BA_FIRE || want == BA_CROUCHFIRE || want == BA_PRONEFIRE) && NewShot(n)) {
            // otro tiro con el boton apretado: la animacion de disparo (sin bucle) vuelve a empezar
            int index = AnimIndex(xanim, kBodyAnimName[want]);
            if (uint16_t idx = index > 0 ? *(uint16_t*)(tree + 8 + index * 2) : 0) {
                uint8_t* info = g::xanimInfo + idx * 0x44;
                *(float*)(info + 0x38) = *(float*)(info + 0x3c) = 0;
                *(uint16_t*)(info + 0x40) = *(uint16_t*)(info + 0x42) = 0;
            }
        }
        BodyAnimUpdate(d, dt);
        // 0x47ba90 deja el tiempo nuevo en +0x38 (ciclo +0x40) calculado desde el confirmado +0x3c (+0x42); el
        // servidor lo confirma despues, pero tambien solo desde la entidad 4
        for (int a = 0; a < size; ++a)
            if (uint16_t idx = *(uint16_t*)(tree + 8 + a * 2)) {
                uint8_t* info = g::xanimInfo + idx * 0x44;
                *(float*)(info + 0x3c) = *(float*)(info + 0x38);
                *(uint16_t*)(info + 0x42) = *(uint16_t*)(info + 0x40);
            }
        g::XAnimCloneServerToClient(tree);  // 0x44d8e0 lo hace por snapshot pero desde la entidad 4
    }
    g::inGameCall = prevSide;
}

// ---------- daño entre jugadores (opcion: set coop_friendlyfire 1 en el host) ----------

// G_Damage (gamex86 RVA 0x18800): (targ, inflictor, attacker, dir, point, damage, dflags, mod, hitLoc).
// gentity +0x104 = gclient (solo jugadores). gamex86 se recarga con el mapa: se vuelve a enganchar.
using GDamage_t = void(__cdecl*)(uint8_t*, uint8_t*, uint8_t*, float*, float*, int, int, int, int);
constexpr uintptr_t G_DAMAGE_RVA = 0x18800;
const uint8_t kGDamagePrologue[8] = {0x83, 0xEC, 0x18, 0x55, 0x8B, 0x6C, 0x24, 0x20};
GDamage_t g_origDamage;
uint8_t* g_damageHooked;  // base de gamex86 enganchada

// ---------- caido y revivir (fase 2, decision D1) ----------
// gentity: +0x1d8 health, +0x134 flags (1 godmode, 2 notarget: la IA no lo ve, 0x20 muerto).
// ps: +0xe4 health, +0xec maxhealth.
constexpr int FL_NOTARGET = 0x2;

bool CoopActive()
{
    if (S->role != HOST) return false;
    for (int n = 1; n < g::MAX_CLIENTS; ++n)
        if (S->remote[n].active) return true;
    return false;
}

bool PlayerInGame(int n) { return g::svs_clients && *(int*)(g::Client(n) + g::CL_STATE) == 1; }
uint8_t* PlayerState(int n) { return *(uint8_t**)0x152bc64 + n * *(int*)0x152bc68; }

bool AnyOtherStanding(int n)
{
    for (int m = 0; m < g::MAX_CLIENTS; ++m)
        if (m != n && PlayerInGame(m) && !S->downed[m] && *(int*)(g::Entity(m) + 0x1d8) > 0) return true;
    return false;
}

void SetHealth(int n, int health)
{
    *(int*)(g::Entity(n) + 0x1d8) = health;
    *(int*)(PlayerState(n) + 0xe4) = health;
}

// aviso en la consola (las ultimas lineas se ven arriba de la pantalla). Texto en Latin-1 como la fuente.
void DownedMessage(int n, bool downed, int self)
{
    if (n == self)
        g::Com_Printf(downed ? "^1COOP: est\xe1s ca\xed" "do. Esper\xe1 que un compa\xf1" "ero te levante (Usar 3 s)\n"
                             : "^2COOP: te levantaron\n");
    else
        g::Com_Printf(downed ? "^1COOP: el jugador %d est\xe1 ca\xed" "do: acercate y manten\xe9 Usar 3 s\n"
                             : "^2COOP: el jugador %d se levant\xf3\n", n + 1);
}

// Carteles al centro de la pantalla: el host le manda "cp" a cada jugador (dura cg_centertime). Un cartel fijo
// (caido, barra de levantar) se renueva antes de que se borre; uno suelto (aviso) se manda una vez.
struct Banner {
    char text[128];
    DWORD sent;
    bool keep;
} g_banner[g::MAX_CLIENTS];

void SendBanner(int n, const char* text, bool keep)
{
    if (!PlayerInGame(n)) return;
    char cmd[160];
    snprintf(cmd, sizeof cmd, "cp \"%s\"", text);
    g::SV_SendServerCommand(g::Client(n), cmd);
    Banner& b = g_banner[n];
    snprintf(b.text, sizeof b.text, "%s", text);
    b.sent = GetTickCount();
    b.keep = keep;
}

void KeepBanner(int n, const char* text)  // "" borra el fijo anterior (no un aviso suelto)
{
    Banner& b = g_banner[n];
    if (!*text) {
        if (b.keep) SendBanner(n, "", false);
        return;
    }
    if (b.keep && !strcmp(b.text, text) && GetTickCount() - b.sent < 2000) return;
    SendBanner(n, text, true);
}

void AnnounceDowned(int n, bool downed)
{
    char text[128];
    for (int m = 0; m < g::MAX_CLIENTS; ++m) {
        if (m == n) {
            if (!downed) SendBanner(m, "^2Te levantaron", false);  // caido: el cartel fijo lo pone HostDownedFrame
        } else if (!S->downed[m]) {
            snprintf(text, sizeof text,
                     downed ? "^1El jugador %d est\xe1 ca\xed" "do^7\\nAcercate y manten\xe9 Usar 3 segundos"
                            : "^2El jugador %d se levant\xf3",
                     n + 1);
            SendBanner(m, text, false);
        }
    }
}

void SetDowned(int n, bool downed)
{
    S->downed[n] = downed;
    S->reviveTime[n] = 0;
    int& flags = *(int*)(g::Entity(n) + 0x134);
    flags = downed ? flags | FL_NOTARGET : flags & ~FL_NOTARGET;
    Log("jugador %d: %s", n, downed ? "caido" : "levantado");
    DownedMessage(n, downed, 0);
}

void __cdecl Hook_GDamage(uint8_t* targ, uint8_t* inflictor, uint8_t* attacker, float* dir, float* point, int damage,
                          int dflags, int mod, int hitLoc)
{
    uint8_t* ff = g::Cvar_FindVar("coop_friendlyfire");
    bool friendlyFire = ff && *(int*)(ff + 0x20);
    bool targPlayer = targ && *(void**)(targ + 0x104);
    if (!friendlyFire && targPlayer && attacker && targ != attacker && *(void**)(attacker + 0x104)) {
        Log("dano entre jugadores bloqueado: %d -> %d (%d)", *(uint16_t*)(attacker + 6), *(uint16_t*)(targ + 6), damage);
        return;  // un jugador no lastima a otro (si a si mismo, como en el SP)
    }
    int n = targPlayer ? *(uint16_t*)(targ + 6) : -1;
    if (n < 0 || n >= g::MAX_CLIENTS || !CoopActive()) {
        g_origDamage(targ, inflictor, attacker, dir, point, damage, dflags, mod, hitLoc);
        return;
    }
    if (S->downed[n]) return;  // caido: ya no recibe daño
    // El daño final depende de la zona del golpe (G_Damage lo escala): se aplica con la vida inflada para que
    // no muera adentro, se mide cuanto bajo y recien ahi se decide entre herido, caido o muerto.
    constexpr int BOOST = 100000;
    int health = *(int*)(targ + 0x1d8);
    SetHealth(n, health + BOOST);
    g_origDamage(targ, inflictor, attacker, dir, point, damage, dflags, mod, hitLoc);
    int left = *(int*)(targ + 0x1d8) - BOOST;
    if (left > 0) {
        SetHealth(n, left);
    } else if (AnyOtherStanding(n)) {
        SetHealth(n, 1);
        SetDowned(n, true);
        AnnounceDowned(n, true);
    } else {
        // Nadie mas en pie: muere el host de verdad, que es lo que dispara la mision fallida del SP y la vuelta
        // al ultimo checkpoint para todos. Un remoto no muere nunca (su muerte recarga la mision de otra forma).
        Log("jugador %d: cayo el ultimo en pie -> muere el host", n);
        if (n != 0) {
            SetHealth(n, 1);
            SetDowned(n, true);
        }
        if (S->downed[0]) SetDowned(0, false);
        SetHealth(0, 1);
        g_origDamage(g::Entity(0), inflictor, attacker, dir, point, BOOST, dflags, mod, hitLoc);
    }
}

// prueba: coopkill <n> <daño> -> G_Damage del mundo sobre el jugador n (con el modulo de juego como VM actual)
void Cmd_CoopKill()
{
    int n = atoi(g::Argv(1)), damage = g::cmd_argc > 2 ? atoi(g::Argv(2)) : 100000;
    if (n < 0 || n >= g::MAX_CLIENTS || !PlayerInGame(n) || !g_origDamage) return;
    uint8_t** currentVM = (uint8_t**)0x13f9db4;
    uint8_t* prevVM = *currentVM;
    int prevIn = g::inGameCall;
    *currentVM = g::gvm;
    g::inGameCall = 1;
    Hook_GDamage(g::Entity(n), nullptr, nullptr, nullptr, nullptr, damage, 0, 0, 0);
    *currentVM = prevVM;
    g::inGameCall = prevIn;
    Log("coopkill %d %d -> vida %d", n, damage, *(int*)(g::Entity(n) + 0x1d8));
}

// Caido: solo puede mirar. Se filtra el usercmd que el juego lee con SV_GetUsercmd (0x449a10, EAX = cliente).
// usercmd: +4 botones (1 disparar, 0x40 usar, 0x4000 cuerpo a tierra, 0x8000 agachado; el byte alto es el
// arma), +0x14 adelante, +0x15 costado, +0x16 arriba (-127 abajo).
}  // namespace
extern "C" void __cdecl FilterUsercmd(int n, uint8_t* cmd)
{
    if (!S || n < 0 || n >= g::MAX_CLIENTS || !S->downed[n]) return;
    uint32_t& buttons = *(uint32_t*)(cmd + 4);
    buttons = (buttons & 0xffff0000) | 0x4000;
    cmd[0x14] = cmd[0x15] = 0;
    cmd[0x16] = 0x81;
}
// CL_GetUserCmd (0x401050): lo que el cgame usa para predecir al jugador local; sin esto el caido se
// veria parado y disparando hasta que llega la correccion del servidor
extern "C" void __cdecl FilterLocalUsercmd(uint8_t* cmd)
{
    if (S && S->role != NONE) FilterUsercmd(S->role == JOINER ? S->slot : 0, cmd);
}
namespace {

// Levantar: un compañero en pie a menos de REVIVE_DIST manteniendo Usar REVIVE_SECONDS.
constexpr float REVIVE_DIST = 80, REVIVE_SECONDS = 3;

// prueba: coophealth <n> <vida>
void Cmd_CoopHealth()
{
    int n = atoi(g::Argv(1));
    if (n < 0 || n >= g::MAX_CLIENTS || !PlayerInGame(n)) return;
    SetHealth(n, atoi(g::Argv(2)));
    Log("coophealth %d = %d", n, atoi(g::Argv(2)));
}

// "texto\n||||||||||" con la parte hecha en verde
void ProgressBanner(char* out, size_t size, const char* label, float t)
{
    constexpr int W = 20;
    int done = (int)(t / REVIVE_SECONDS * W);
    done = done < 0 ? 0 : done > W ? W : done;
    snprintf(out, size, "%s\\n^2%.*s^7%.*s", label, done, "||||||||||||||||||||", W - done, "||||||||||||||||||||");
}

void HostDownedFrame(float dt)
{
    int reviving[g::MAX_CLIENTS] = {-1, -1, -1, -1};  // a quien esta levantando cada jugador
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        if (!S->downed[n]) continue;
        if (!PlayerInGame(n) || !CoopActive()) { SetDowned(n, false); continue; }
        SetHealth(n, 1);
        const float* o = (const float*)(PlayerState(n) + 0x14);
        int reviver = -1;
        for (int m = 0; m < g::MAX_CLIENTS && reviver < 0; ++m) {
            if (m == n || !PlayerInGame(m) || S->downed[m]) continue;
            const float* p = (const float*)(PlayerState(m) + 0x14);
            float dx = p[0] - o[0], dy = p[1] - o[1], dz = p[2] - o[2];
            bool use = *(uint32_t*)(g::Client(m) + 0x30 + 4) & 0x40;
            if (use && dx * dx + dy * dy + dz * dz < REVIVE_DIST * REVIVE_DIST) reviver = m;
        }
        float before = S->reviveTime[n];
        S->reviveTime[n] = reviver >= 0 ? before + dt : 0;
        if (reviver >= 0 && (int)S->reviveTime[n] != (int)before)
            Log("jugador %d: %d levantandolo (%.0f s)", n, reviver, S->reviveTime[n]);
        if (S->reviveTime[n] >= REVIVE_SECONDS) {
            SetDowned(n, false);
            SetHealth(n, *(int*)(PlayerState(n) + 0xec) / 2);
            AnnounceDowned(n, false);
        } else if (reviver >= 0) {
            reviving[reviver] = n;
        }
    }
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        char text[128] = "", label[64];
        if (S->downed[n] && S->reviveTime[n] > 0) {
            ProgressBanner(text, sizeof text, "^2Te est\xe1n levantando", S->reviveTime[n]);
        } else if (S->downed[n]) {
            snprintf(text, sizeof text, "^1EST\xc1S CA\xcd" "DO^7\\nEsper\xe1 que un compa\xf1" "ero te levante");
        } else if (reviving[n] >= 0) {
            snprintf(label, sizeof label, "Levantando al jugador %d", reviving[n] + 1);
            ProgressBanner(text, sizeof text, label, S->reviveTime[reviving[n]]);
        }
        KeepBanner(n, text);
    }
}

// SV_GetUsercmd: llama a la original (EAX = cliente, [esp+4] = destino) y despues a FilterUsercmd(n, cmd)
}  // namespace
extern "C" void* g_getUsercmdTramp;
void* g_getUsercmdTramp;
asm(R"(
.intel_syntax noprefix
.globl _Hook_GetUsercmd
_Hook_GetUsercmd:
    push eax
    push dword ptr [esp+8]
    call dword ptr [_g_getUsercmdTramp]
    add esp, 4
    push dword ptr [esp+8]
    push dword ptr [esp+4]
    call _FilterUsercmd
    add esp, 8
    pop eax
    ret
.globl _Hook_CLGetUserCmd
_Hook_CLGetUserCmd:
    push dword ptr [esp+4]
    call dword ptr [_g_clGetUserCmdTramp]
    add esp, 4
    test eax, eax
    jz 1f
    push eax
    push dword ptr [esp+8]
    call _FilterLocalUsercmd
    add esp, 4
    pop eax
1:  ret
.att_syntax
)");
extern "C" void Hook_GetUsercmd();
extern "C" void Hook_CLGetUserCmd();
extern "C" void* g_clGetUserCmdTramp;
void* g_clGetUserCmdTramp;
namespace {
constexpr uintptr_t SV_GET_USERCMD = 0x449a10;
const uint8_t kGetUsercmdPrologue[6] = {0x56, 0x8B, 0xF0, 0x85, 0xF6, 0x57};

constexpr uintptr_t CL_GET_USERCMD = 0x401050;
const uint8_t kCLGetUserCmdPrologue[8] = {0x56, 0x8B, 0xF0, 0xA1, 0x18, 0xFE, 0x42, 0x01};

// engancha (o restaura) una funcion del exe; el detour vive en esta DLL
void HookExe(bool on, uintptr_t addr, const uint8_t* prologue, int len, void* detour, void*& tramp, const char* name)
{
    bool hooked = *(uint8_t*)addr == 0xE9;
    if (on && !hooked) {
        if (memcmp((void*)addr, prologue, len) != 0) return;
        tramp = hook::Inline(addr, detour, len);
        Log("%s enganchado", name);
    }
    if (!on && hooked && tramp) hook::Write(addr, prologue, len);
}

void HookUsercmd(bool on)
{
    if (S->role == HOST || !on)
        HookExe(on, SV_GET_USERCMD, kGetUsercmdPrologue, sizeof kGetUsercmdPrologue, (void*)Hook_GetUsercmd,
                g_getUsercmdTramp, "SV_GetUsercmd");
    HookExe(on, CL_GET_USERCMD, kCLGetUserCmdPrologue, sizeof kCLGetUserCmdPrologue, (void*)Hook_CLGetUserCmd,
            g_clGetUserCmdTramp, "CL_GetUserCmd");
}

void HookDamage(bool on)
{
    uint8_t* game = (uint8_t*)GetModuleHandleA("gamex86.dll");
    if (on && game && game != g_damageHooked) {
        uint8_t* fn = game + G_DAMAGE_RVA;
        if (memcmp(fn, kGDamagePrologue, sizeof kGDamagePrologue) != 0) {
            Log("G_Damage: prologo inesperado, sin opcion de dano entre jugadores");
            g_damageHooked = game;  // no reintentar en cada frame
            return;
        }
        g_origDamage = (GDamage_t)hook::Inline((uintptr_t)fn, (void*)Hook_GDamage, sizeof kGDamagePrologue);
        g_damageHooked = game;
        Log("G_Damage enganchado en %p", fn);
    }
    if (!on && g_damageHooked && g_damageHooked == game && g_origDamage &&
        *(game + G_DAMAGE_RVA) == 0xE9) {  // el detour vive en esta DLL: se restaura antes de descargarla
        hook::Write((uintptr_t)(game + G_DAMAGE_RVA), kGDamagePrologue, sizeof kGDamagePrologue);
        g_damageHooked = nullptr;
    }
}

// Pausas (tarea 2.5). El SP se pausa con cl_paused 1: SV_Frame no avanza y el cliente no manda usercmds
// (0x40b3e0). Lo pone la UI al abrir un menu y SV_Frame mismo si la UI esta en pantalla completa (0x44d0a3; el
// JNZ de 0x44d0a6 saltea ese paso).
// "Haz clic para iniciar" (ui/pregame.menu): pausa con el mundo sin dibujar; cg_norender queda en 1 desde la carga
// hasta el clic (uiScript playerstart). El ultimo menu de la UI (uix86+0x1bd394) no sirve: tras un devmap en
// juego queda en 7 aunque la pantalla sea la misma.
bool PregameOpen()
{
    uint8_t* norender = g::Cvar_FindVar("cg_norender");
    return norender && *(int*)(norender + 0x20) && g::cl_paused && *(int*)(g::cl_paused + 0x20);
}

void SkipUiPauseCheck(bool skip)
{
    uint8_t* jnz = (uint8_t*)0x44d0a6;
    uint8_t want = skip ? 0xEB : 0x75;  // JMP / JNZ original
    if (*jnz != want && (*jnz == 0xEB || *jnz == 0x75)) hook::Write((uintptr_t)jnz, &want, 1);
}

// Host: con remotos conectados el menu no pausa (como en multijugador). La pantalla de inicio tras cargar si.
void HostNoPause()
{
    bool on = CoopActive() && !PregameOpen();
    SkipUiPauseCheck(on);
    if (on && *(int*)(g::cl_paused + 0x20)) {
        g::Cvar_Set("cl_paused", "0");
        Log("host: menu abierto con remotos conectados, el juego sigue");
    }
}

// Remoto: "Haz clic para iniciar" se saltea solo, haciendo lo mismo que el clic
void JoinerSkipPregame()
{
    if (!S->joined || !g::uivm || !PregameOpen()) return;
    int args[13] = {7, 0};  // UI_SET_ACTIVE_MENU, ninguno: cierra los menus y suelta el teclado
    g::CallVM(g::uivm, args);
    g::Cvar_Set("cl_paused", "0");
    g::Cvar_Set("cg_norender", "0");
    g::Cbuf_AddText("fade 0 0 0 0 3\n");
    Log("joiner: pantalla de inicio salteada");
}

// Mision fallida (g_reloading 1): antes de que se cargue el checkpoint se sueltan los remotos y los cuerpos.
// Si quedaran conectados, la carga les manda comandos que nadie confirma ("Reliable command buffer overflow");
// los arboles se liberan ahora, mientras su memoria sigue siendo valida. Los remotos se vuelven a unir solos.
void HostCheckReload()
{
    uint8_t* reloading = g::Cvar_FindVar("g_reloading");
    if (!reloading || !*(int*)(reloading + 0x20) || !CoopActive()) return;
    Log("host: mision fallida, se sueltan los remotos hasta que cargue el checkpoint");
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        if (S->bodyTree[n]) HostFreeBody(n);
        S->downed[n] = false;
    }
    for (int n = 1; n < g::MAX_CLIENTS; ++n) {
        if (!S->remote[n].active) continue;
        Writer w(M_RELOAD);
        SendTo(S->remote[n].addr, w.buf, w.len);
        *(int*)(g::Client(n) + g::CL_STATE) = 0;
        S->remote[n].active = false;
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
    case M_RELOAD: {
        // El host vuelve al checkpoint y su reloj retrocede: el cgame de aca cortaria con "Server time went
        // backwards". Se hace como al entrar: mapa local de nuevo y despues unirse (cooprejoin). Los DObj propios
        // se liberan ahora, mientras su memoria sigue valida.
        if (!S->joined) return;
        for (int e = 0; e < g::MAX_GENTITIES; ++e)
            if (S->ownTree[e]) JoinerFreeDObj(e);
        S->joined = S->frozen = false;
        S->role = NONE;  // hasta cooprejoin: no se manda nada mientras carga
        memset(S->downed, 0, sizeof S->downed);
        Log("joiner: el host recarga el checkpoint, recargo %s y vuelvo a entrar", S->hostMap);
        g::Com_Printf("^3COOP: el host vuelve al \xfaltimo checkpoint, reconectando...\n");
        char cmd[128];
        snprintf(cmd, sizeof cmd, "devmap %s\ncooprejoin\n", S->hostMap);
        g::Cbuf_AddText(cmd);
        break;
    }
    case M_DOWNED: {
        uint8_t mask = r.Get<uint8_t>();
        for (int m = 0; m < g::MAX_CLIENTS; ++m) {
            bool d = mask >> m & 1;
            if (d == S->downed[m]) continue;
            S->downed[m] = d;
            DownedMessage(m, d, S->slot);
        }
        g_team = r.Get<uint8_t>();
        for (auto& p : g_teamPos)
            for (float& v : p) v = r.Get<float>();
        if (!r.Ok()) g_team = 0;
        break;
    }
    case M_ENTS: {
        if (!S->joined || !g::sv_gentities) return;  // sin servidor local (se cerro el mapa) no hay donde copiar
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

// despues de recargar el mapa por M_RELOAD: volver a unirse al mismo host (lo reintenta cada 1 s)
void Cmd_CoopRejoin()
{
    if (S->sock == INVALID_SOCKET || S->role != NONE || !S->host.sin_port) return;
    S->role = JOINER;
    S->joined = false;
    S->hostMap[0] = 0;
    JoinerSendJoin();
    g::Com_Printf("COOP: volviendo a entrar...\n");
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
            len += snprintf(models + len, sizeof models - len, " %s(p%d f%x o%d)", m ? SafeStr(*(char**)m) : "null", d[0x48 + i],
                            *(uint16_t*)(d + 0x38 + i * 2), d[0x50 + i]);
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
        Log("dobj ent %d (%.0f %.0f %.0f): modelos %d:%s arbol %p anims '%s' n=%d activos=%d u12=%d", e, org[0], org[1], org[2],
            nm, models, tree, animName, size, active, *(uint16_t*)(d + 0x12));
        // estado de cada nodo activo: cliente (tiempo, goalTime, goalWeight, weight, rate) / servidor (lo mismo)
        for (int i = 0; tree && e < g::MAX_CLIENTS && i < size; ++i) {
            uint16_t idx = *(uint16_t*)(tree + 8 + i * 2);
            if (!idx) continue;
            const float* f = (const float*)(g::xanimInfo + idx * 0x44);
            Log("  nodo %d: cli t=%.2f %.2f gt=%.2f gw=%.2f w=%.2f r=%.2f | srv t=%.2f %.2f gt=%.2f gw=%.2f w=%.2f r=%.2f", i, f[7],
                f[8], f[10], f[11], f[12], f[13], f[14], f[15], f[17], f[18], f[19], f[20]);
        }
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
    float dist = g::cmd_argc > 3 ? (float)atof(g::Argv(3)) : 150;
    uint8_t* gameClients = *(uint8_t**)0x152bc64;
    if (n < 0 || n >= g::MAX_CLIENTS || e < 0 || e >= g::MAX_GENTITIES || !gameClients) return;
    uint8_t* ps = gameClients + n * *(int*)0x152bc68;
    const float* t = (const float*)(g::Entity(e) + 0xe0);  // r.currentOrigin
    float* o = (float*)(ps + 0x14);
    float dx = o[0] - t[0], dy = o[1] - t[1], len = sqrtf(dx * dx + dy * dy);
    if (len < 1) dx = -1, dy = 0, len = 1;
    o[0] = t[0] + dx / len * dist;
    o[1] = t[1] + dy / len * dist;
    o[2] = t[2] + 8;
    memset(ps + 0x20, 0, 12);  // velocidad
    float yaw = atan2f(-dy, -dx) * 180.0f / 3.14159265f;
    const int* cmdAngles = (const int*)(ps + 0x73e4 + 8);  // gclient.pers.cmd.angles
    int* delta = (int*)(ps + 0x48);
    float pitch = g::cmd_argc > 4 ? (float)atof(g::Argv(4)) : 0;  // positivo = hacia abajo
    delta[0] = ((int)(pitch * 65536 / 360) & 0xffff) - cmdAngles[0];
    delta[1] = ((int)(yaw * 65536 / 360) & 0xffff) - cmdAngles[1];
    Log("cooplook %d -> ent %d en (%.0f %.0f %.0f), yaw %.0f", n, e, o[0], o[1], o[2], yaw);
}

// prueba: coopstate <ent> -> vuelca el entityState (0x9c bytes) como enteros y floats
void Cmd_CoopState()
{
    int e = atoi(g::Argv(1));
    if (e < 0 || e >= g::MAX_GENTITIES || !g::sv_gentities) return;
    const uint8_t* st = g::Entity(e);
    const float* org = (const float*)(st + 0xe0);
    Log("state %d (r.currentOrigin %.1f %.1f %.1f):", e, org[0], org[1], org[2]);
    for (int i = 0; i < g::ENTSTATE_SIZE; i += 16) {
        char line[256];
        int len = snprintf(line, sizeof line, "  +%02x:", i);
        for (int k = i; k < i + 16 && k < g::ENTSTATE_SIZE; k += 4)
            len += snprintf(line + len, sizeof line - len, " %08x(%.1f)", *(const uint32_t*)(st + k), *(const float*)(st + k));
        Log("%s", line);
    }
    // centity del cgame (cg_entities 0x3012adc0, 0x178 c/u): +0 currentState, +0x138 currentValid, +0x150 lerpOrigin
    if (uint8_t* cg = (uint8_t*)GetModuleHandleA("cgamex86.dll")) {
        const uint8_t* cent = cg + 0x12adc0 + e * 0x178;
        const float* lo = (const float*)(cent + 0x150);
        Log("cent %d: eType %d eFlags %x valid %d lerpOrigin (%.1f %.1f %.1f) lerpAngles (%.1f %.1f %.1f) dobjCliente %d", e,
            cent[0], *(const int*)(cent + 0x10), *(const int*)(cent + 0x138), lo[0], lo[1], lo[2], lo[3], lo[4], lo[5],
            ((const int16_t*)0x89fc88)[e]);
    }
}

// prueba: coopcmd <n> -> ultimo usercmd del cliente n (client_t+0x30, 11 dwords)
// prueba: coopent <n> -> gentity n entera (0x2c8 bytes) en una linea, para comparar antes/despues
void Cmd_CoopCmd()
{
    int n = atoi(g::Argv(1));
    if (n < 0 || n >= g::MAX_CLIENTS || !g::svs_clients) return;
    const uint32_t* c = (const uint32_t*)(g::Client(n) + 0x30);
    Log("usercmd %d: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", n, c[0], c[1], c[2], c[3], c[4], c[5], c[6],
        c[7], c[8], c[9], c[10]);
}

void Cmd_CoopEnt()
{
    int e = atoi(g::Argv(1));
    if (e < 0 || e >= g::MAX_GENTITIES || !g::sv_gentities) return;
    const uint32_t* p = (const uint32_t*)g::Entity(e);
    char line[4096];
    int len = 0;
    for (int i = 0; i < g::sv_gentitySize / 4; ++i) len += snprintf(line + len, sizeof line - len, " %x:%x", i * 4, p[i]);
    Log("ent %d:%s", e, line);
}

// prueba: cooplists -> revisa las listas circulares de nodos de cada entidad (gentity+0x2b6 cabeza, flag 0x1000;
// nodos de 8 bytes {u16 tipo, u16 clave, u16 anterior, u16 siguiente} en *(gamex86 + 0x2dc1c0)). Si una queda
// rota, G_FreeEntity (al cargar partida) da vueltas para siempre.
void Cmd_CoopLists()
{
    uint8_t* game = (uint8_t*)GetModuleHandleA("gamex86.dll");
    if (!game || !g::sv_gentities) return;
    uint8_t* nodes = *(uint8_t**)(game + 0x2dc1c0);
    int bad = 0, lists = 0;
    for (int e = 0; e < g::sv_numEntities; ++e) {
        uint8_t* ent = g::Entity(e);
        uint16_t head = *(uint16_t*)(ent + 0x2b6);
        if (!head) continue;
        ++lists;
        uint16_t cur = head;
        int steps = 0;
        bool ok = true;
        do {
            uint16_t next = *(uint16_t*)(nodes + cur * 8 + 6);
            if (!next || *(uint16_t*)(nodes + next * 8 + 4) != cur || ++steps > 4096) { ok = false; break; }
            cur = next;
        } while (cur != head);
        if (!ok) {
            ++bad;
            Log("lista rota: ent %d (eType %d flags %x) cabeza %x en el nodo %x tras %d pasos", e, ent[0],
                *(int*)(ent + 0x134), head, cur, steps);
        }
    }
    Log("cooplists: %d listas, %d rotas", lists, bad);
}

// prueba: coopanims <texto> -> hojas del arbol generic_human cuyo nombre contiene el texto.
// XAnim: +0 nombre, +4 cantidad, +8 nodos de 8 bytes {u16 hijos, u16 padre, hoja: XAnimParts* (nombre en +0)}
void Cmd_CoopAnims()
{
    uint8_t* x = FindXAnim("generic_human.atr");
    if (!x) return;
    const char* filter = g::Argv(1);
    int size = *(int*)(x + 4), shown = 0;
    for (int i = 0; i < size && shown < 200; ++i) {
        uint8_t* node = x + 8 + i * 8;
        if (*(uint16_t*)node) continue;  // no es hoja
        uint8_t* parts = *(uint8_t**)(node + 4);
        const char* name = parts ? SafeStr(*(char**)parts) : "?";
        if (*filter && !strstr(name, filter)) continue;
        Log("anim %d (padre %d): %s", i, *(uint16_t*)(node + 2), name);
        ++shown;
    }
}

struct CmdDef {
    const char* name;
    void (*fn)();
};
const CmdDef kCmds[] = {
    {"coophost", Cmd_CoopHost}, {"coopjoin", Cmd_CoopJoin}, {"coopinfo", Cmd_CoopInfo}, {"coopbring", Cmd_CoopBring},
    {"coopdobj", Cmd_CoopDObj}, {"coopgod", Cmd_CoopGod},   {"cooplook", Cmd_CoopLook}, {"coopstate", Cmd_CoopState},
    {"coopbodydraw", Cmd_CoopBodyDraw}, {"coopanims", Cmd_CoopAnims}, {"coopcmd", Cmd_CoopCmd}, {"coopent", Cmd_CoopEnt},
    {"coophealth", Cmd_CoopHealth}, {"coopkill", Cmd_CoopKill}, {"cooplists", Cmd_CoopLists},
    {"cooprejoin", Cmd_CoopRejoin},
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
    HookCgame(false);  // el puntero apunta a esta DLL, que se va a descargar
    HookDamage(false);
    HookUsercmd(false);
    SkipUiPauseCheck(false);
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
    if (S->role == HOST && sock == NS_SERVER) {  // corre aunque el juego este en pausa
        HostCheckReload();
        HostNoPause();
    }
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
    HookCgame(true);
    HookDamage(true);
    HookUsercmd(true);
    static int lastTime;
    int now = *(int*)0x152e930;  // sv.time
    float dt = (now - lastTime) / 1000.0f;
    lastTime = now;
    if (dt < 0 || dt > 0.2f) dt = 0;
    HostUpdateBodies();
    HostAnimateBodies(dt);
    HostDownedFrame(dt);
    uint8_t team = 0;  // quien esta en el mapa y donde (brujula de todos)
    float teamPos[g::MAX_CLIENTS][3] = {};
    for (int n = 0; n < g::MAX_CLIENTS; ++n) {
        if (!PlayerInGame(n)) continue;
        team |= 1 << n;
        memcpy(teamPos[n], PlayerState(n) + 0x14, 8);
        teamPos[n][2] = *(float*)(g::Entity(n) + 0x48);  // apos.trBase yaw
    }
    if (CoopActive()) DrawTeamOnCompass(team, teamPos, 0);
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
        CopyHostViewmodel(n);
        g::SV_SendClientSnapshot(cl);
        ++g_statSnaps;
        Writer w(M_DOWNED);  // quien esta caido (el remoto filtra su prediccion y avisa) y la brujula
        uint8_t mask = 0;
        for (int m = 0; m < g::MAX_CLIENTS; ++m) mask |= S->downed[m] << m;
        w.Put<uint8_t>(mask);
        w.Put<uint8_t>(team);
        w.Put(teamPos, sizeof teamPos);
        SendTo(S->remote[n].addr, w.buf, w.len);
        SendEntities(n);
        SendDObjs(n);
    }
}

// En el que se une el servidor local no corre, pero el cliente del SP toma su reloj de sv.time
// (CL_Frame 0x40df80): lo avanzamos siguiendo al ultimo snapshot del host, un snapshot atras para interpolar.
EXPORT int __cdecl Coop_FrozenFrame(int msec)
{
    if (!S->frozen) return 0;
    HookCgame(true);
    HookUsercmd(true);
    JoinerSkipPregame();
    DrawTeamOnCompass(g_team, g_teamPos, S->slot);
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
