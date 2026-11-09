// CoD2 (CoD2SP_s.exe 1.3), en prueba. El servidor de un jugador reserva un solo cliente: esto lo amplia a 4 y
// agrega una red UDP propia. Lo carga ddraw.dll (loader.cpp) desde la carpeta del juego.
//   coop2host            el que tiene la partida (puerto UDP 28962)
//   coop2join <IP>       se une a esa IP (127.0.0.1 si no hay; mismo mapa cargado)
// El cliente de CoD2 no recibe paquetes: el servidor le arma el snapshot como mensaje (SV_SendClientSnapshot) y se lo
// pasa directo al parser (CL_ParseServerMessage). El host arma ese mismo mensaje para cada remoto y lo manda por UDP;
// el que se une congela su juego local y le pasa a su parser lo que llega.
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>
#include "hook.h"

namespace {
constexpr int MAX_CLIENTS = 4;
constexpr int CLIENT_SIZE = 0xaa08;   // client_t (svs.clients, SV_Startup 0x448eb0)
constexpr int GCLIENT_SIZE = 0x86b8;  // gclient_t (level.clients)
constexpr int GENTITY_SIZE = 0x1fc;   // g_entities en 0x1b15f00
constexpr uintptr_t G_ENTITIES = 0x1b15f00, GE_CLIENT = 0xfc, GC_CONNECTED = 0x85c0;
constexpr uintptr_t LEVEL_CLIENTS = 0x1c60500, LEVEL_NUM_ENTITIES = 0x1c6050c, LEVEL_MAXCLIENTS = 0x1c6052c;
constexpr uintptr_t SV_GENTITIES_NUM = 0x82242c, SV_GAMECLIENTS = 0x822430;
constexpr uintptr_t SVS_CLIENTS = 0x825508;
// client_t: +4 bloque de comandos del servidor {largo, secuencia, ack, texto[0x2000], offsets[256]} (0x240c bytes),
// +0x2410 ultimo usercmd (0x30), +0x2440 gentity, +0x2444 playerState del snapshot, +0xaa04 mensajes mandados
constexpr int CL_BLOCK = 4, BLOCK_SIZE = 0x240c, BLOCK_TEXT = 0xc, BLOCK_OFFSETS = 0x200c;
constexpr int CL_LASTUSERCMD = 0x2410, CL_GENTITY = 0x2440, CL_MESSAGES = 0xaa04, USERCMD_SIZE = 0x30;
constexpr int SNAP_MAX = 0x4000;  // msg_t de SV_SendClientSnapshot (0x44b180)

// G_InitGame: `mov [sv.num_entities], ebp` (ebp = 1) despues de armar level.clients y antes de cargar el mapa
constexpr uintptr_t INIT_CLIENTS_AT = 0x4d9be9;
constexpr uintptr_t CLIENT_THINK = 0x4e7ee0, LOCAL_THINK_CALL = 0x409f76;  // ClientThink: EAX = cliente
constexpr uintptr_t CLIENT_BEGIN = 0x4e8fe0;                               // EAX = cliente
constexpr uintptr_t PATH_INIT_CALL = 0x4e8f48;                             // en ClientConnect
// SV_FinishFrame (0x44a900): comandos del servidor al cliente local y su snapshot
constexpr uintptr_t RELIABLE_CALL = 0x44a93a, CL_DELIVER_COMMANDS = 0x40f580;  // EBX = bloque
constexpr uintptr_t SNAPSHOT_CALL = 0x44a951, SV_SEND_SNAPSHOT = 0x44b180;     // EDI = client_t
constexpr uintptr_t SV_BUILD_SNAPSHOT = 0x44b0c0;  // EAX = client_t
constexpr uintptr_t CGAME_SNAP_CALL = 0x44a96e;    // el cliente procesa el snapshot (0x4a4330) y copia los estados
constexpr uintptr_t SV_WRITE_SNAPSHOT = 0x44af00;  // ESI = msg, EDX = client_t
constexpr uintptr_t CL_PARSE_MESSAGE = 0x40cf00;   // ESI = msg, EAX = numero de mensaje
constexpr uintptr_t G_RUN_FRAME = 0x4da970;        // int (int parcial, int limite); 1 = frame terminado

auto Com_Printf = (void(__cdecl*)(const char*, ...))0x42c130;
auto Cmd_AddCommand = (void(__cdecl*)(const char*, void (*)()))0x41bb00;
auto ClientConnect = (int(__cdecl*)(int))0x4e8f40;
auto Dvar_FindVar = (uint8_t*(__cdecl*)(const char*))0x431260;  // valor en +8

const char* MapName()
{
    uint8_t* d = Dvar_FindVar("mapname");
    const char* name = d ? *(const char**)(d + 8) : nullptr;
    return name ? name : "";
}

// el mapa del nivel que corre aca, o "" sin nivel (menu): mapname queda con el ultimo mapa despues de salir
const char* LevelMap()
{
    uint8_t* d = Dvar_FindVar("sv_running");
    return d && d[8] ? MapName() : "";
}

void Cbuf_AddText(const char* text)
{
    asm volatile("call *%1" : "+a"(text) : "r"((uintptr_t)0x41afc0) : "ecx", "edx", "memory");
}

// linea de estado de la ventana Co-op (ui/cod2_coop_popmenu.menu); tambien va a la consola
void Status(const char* fmt, ...)
{
    char text[200], cmd[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    for (char* c = text; *c; ++c)
        if (*c == '"' || *c == ';' || *c == '\n') *c = ' ';
    snprintf(cmd, sizeof cmd, "set ui_coop2_status \"%s\"\n", text);
    Cbuf_AddText(cmd);
    Com_Printf("^2COOP: %s\n", text);
}

struct Msg {  // msg_t
    int overflowed;
    uint8_t* data;
    int maxsize, cursize, readcount, bit;
};

alignas(16) uint8_t g_clients[MAX_CLIENTS * GCLIENT_SIZE];
FILE* g_log;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

uint8_t* Entity(int n) { return (uint8_t*)G_ENTITIES + n * GENTITY_SIZE; }
uint8_t* Client(int n) { return *(uint8_t**)SVS_CLIENTS + n * CLIENT_SIZE; }
uint8_t* GClient(int n) { return g_clients + n * GCLIENT_SIZE; }

// Los jugadores tienen CONTENTS_BODY (ent+0xc0) y el clipmask del Pmove lo incluye: dos jugadores en el mismo lugar
// (el remoto aparece donde esta el host) quedaban trabados. Mientras uno se mueve, los demas jugadores no son solidos.
constexpr int GE_CONTENTS = 0xc0;
void ClientThink(int n)
{
    uint32_t saved[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        saved[i] = *(uint32_t*)(Entity(i) + GE_CONTENTS);
        if (i != n) *(uint32_t*)(Entity(i) + GE_CONTENTS) = 0;
    }
    int num = n;
    asm volatile("call *%1" : "+a"(num) : "r"(CLIENT_THINK) : "ecx", "edx", "memory");
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (i != n) *(uint32_t*)(Entity(i) + GE_CONTENTS) = saved[i];
}
void ClientBegin(int n) { asm volatile("call *%1" : "+a"(n) : "r"(CLIENT_BEGIN) : "ecx", "edx", "memory"); }

// ---------- red ----------
constexpr uint16_t PORT = 28962;
constexpr uint32_t MAGIC = 0x504f4332;  // "2COP"
enum : uint8_t { M_JOIN = 'J', M_ACCEPT = 'A', M_FULL = 'F', M_CMD = 'C', M_SNAP = 'S', M_REJOIN = 'R', M_PING = 'P',
                 M_BYE = 'B' };
// Si no llega nada de un remoto en 15 s el host lo saca; si no llega nada del host en 20 s el que se une vuelve al
// menu. Sin nivel corriendo (carga, pausa) cada lado manda M_PING cada segundo para que no lo den por perdido.
constexpr DWORD REMOTE_TIMEOUT = 15000, HOST_TIMEOUT = 20000;
enum Role { NONE, HOST, JOINER } g_role;
SOCKET g_sock = INVALID_SOCKET;

struct Remote {
    bool active;
    sockaddr_in addr;
} g_remote[MAX_CLIENTS];
DWORD g_joinedAt[MAX_CLIENTS];  // host: cuando entro cada remoto
DWORD g_lastHeard[MAX_CLIENTS];  // host: ultimo paquete de cada remoto
DWORD g_hostHeard;               // joiner: ultimo paquete del host
DWORD g_levelStart;              // host: G_InitGame (nivel nuevo o partida cargada)
bool g_downed[MAX_CLIENTS];      // host: caido esperando que lo levanten
float g_reviveTime[MAX_CLIENTS]; // host: segundos que un companero lleva levantandolo

sockaddr_in g_host;
bool g_joined;
DWORD g_lastLevelFrame;  // ultimo frame con nivel corriendo (Cod2_Snapshot)
DWORD g_mapLoadSince;  // joiner: cargando el mapa del host (no pide entrar hasta G_InitGame)
int g_slot;
DWORD g_lastJoinTry;

// host: ultimo bloque de comandos del jugador 0 (los comandos "a todos" van solo a el), repetido en 3 snapshots
uint8_t g_block[BLOCK_SIZE];
int g_blockSends;
// joiner: lo ultimo que llego
uint8_t g_rxSnap[SNAP_MAX], g_rxBlock[BLOCK_SIZE];
int g_rxSnapLen;
bool g_rxSnapNew, g_rxBlockNew;
int g_rxBlockSeq = -1;

bool OpenSocket(uint16_t port)
{
    if (g_sock != INVALID_SOCKET) return true;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    int big = 1 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(g_sock, (sockaddr*)&a, sizeof a) != 0) {
        Log("no se pudo abrir el puerto UDP %d (%d)", port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

struct Packet {
    uint8_t buf[SNAP_MAX + BLOCK_SIZE + 64];
    int len = 0;
    explicit Packet(uint8_t type)
    {
        Put(MAGIC);
        buf[len++] = type;
    }
    void Put(const void* p, int n)
    {
        memcpy(buf + len, p, n);
        len += n;
    }
    template <class T> void Put(const T& v) { Put(&v, sizeof v); }
    void Send(const sockaddr_in& to) const { sendto(g_sock, (const char*)buf, len, 0, (const sockaddr*)&to, sizeof to); }
};

bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

// ---------- DObj y animaciones ----------
// El juego arma el DObj de cada entidad (G_DObjUpdate 0x4d6110) en la tabla que dibuja el cliente: 0x74a980[entidad]
// = indice en el pool de DObj (0x74c200, 100 bytes). En el que se une su juego esta congelado, asi que el host le
// manda la forma de cada DObj visible (indices de la tabla de modelos 0x1b15ad0, que coincide en los dos con el mismo
// mapa, y el XAnim del arbol, que queda en la misma direccion) y el estado de cada nodo de animacion.
// DObj: +0 arbol, +0x18 cantidad de modelos, +0x1a sin colision (bits), +0x1c modelos, +0x3c padres.
// Arbol: +0 XAnim (+4 cantidad de nodos), +4 entidad+1, +8 u16 por nodo = indice en el pool de infos (0xa0a200, 0x28).
// Info: +0xc..+0x27 hora, hora anterior, vueltas, tiempo de mezcla, peso objetivo, peso, velocidad.
constexpr uintptr_t DOBJ_INDEX = 0x74a980, DOBJ_POOL = 0x74c200, G_MODELS = 0x1b15ad0, XANIM_INFOS = 0xa0a200;
constexpr int DOBJ_SIZE = 100, INFO_SIZE = 0x28, INFO_STATE = 0xc, INFO_STATE_SIZE = 0x1c, MAX_ENTS = 1024, MAX_MODELS_PER = 8;
constexpr uintptr_t CL_DOBJ_CREATE = 0x42f630;     // cdecl (modelos {XModel*, tag, sin colision}[], cantidad, arbol), EAX = entidad
constexpr uintptr_t CL_DOBJ_FREE = 0x42f760;       // ECX = entidad
constexpr uintptr_t XANIM_CREATE_TREE = 0x42f280;  // EBX = XAnim -> arbol
constexpr uintptr_t XANIM_ALLOC_INFO = 0x478830;   // ESI = arbol, cdecl (nodo) -> info
enum : uint8_t { M_DOBJ = 'D', M_ANIM = 'N', M_ENTS = 'E' };

uint8_t* ClientDObj(int e)
{
    uint16_t idx = *(uint16_t*)(DOBJ_INDEX + e * 2);
    return idx ? (uint8_t*)DOBJ_POOL + idx * DOBJ_SIZE : nullptr;
}

int ModelIndex(void* model)
{
    for (int i = 1; i < 256; ++i)
        if (((void**)G_MODELS)[i] == model) return i;
    return -1;
}

uint32_t g_dobjSig[MAX_CLIENTS][MAX_ENTS];
DWORD g_dobjSent[MAX_CLIENTS][MAX_ENTS];

uint32_t DObjSig(const uint8_t* d)
{
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, int len) {
        for (int i = 0; i < len; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    };
    uint8_t* tree = *(uint8_t**)d;
    void* xanim = tree ? *(void**)tree : nullptr;
    mix(d + 0x18, 1);
    mix(d + 0x1a, 1);
    mix(d + 0x1c, 4 * MAX_MODELS_PER);
    mix(d + 0x3c, MAX_MODELS_PER);
    mix(&xanim, 4);
    return h ? h : 1;
}

// host, despues de armar el snapshot del jugador n (lista de entidades en 0x8224a4, cantidad en 0x8224a0)
void HostSendDObjs(int n)
{
    const int count = *(int*)0x8224a0;
    const int* list = (const int*)0x8224a4;
    DWORD now = GetTickCount();
    static Packet dp(M_DOBJ), ap(M_ANIM);
    dp = Packet(M_DOBJ);
    ap = Packet(M_ANIM);
    auto flush = [&](Packet& p, uint8_t type) {
        if (p.len > 5) p.Send(g_remote[n].addr);
        p = Packet(type);
    };
    for (int i = 0; i < count; ++i) {
        int e = list[i];
        if (e < 0 || e >= MAX_ENTS) continue;
        uint8_t* d = ClientDObj(e);
        uint32_t sig = d ? DObjSig(d) : 0;
        if (sig != g_dobjSig[n][e] || (sig && now - g_dobjSent[n][e] > 2000)) {
            int nm = d ? d[0x18] : 0;
            if (nm > MAX_MODELS_PER) nm = MAX_MODELS_PER;
            uint8_t idx[MAX_MODELS_PER];
            bool ok = true;
            for (int k = 0; k < nm && ok; ++k) {
                int m = ModelIndex(*(void**)(d + 0x1c + k * 4));
                ok = m > 0;
                idx[k] = (uint8_t)m;
            }
            if (ok) {
                if (dp.len + 64 > (int)sizeof dp.buf) flush(dp, M_DOBJ);
                uint8_t* tree = d ? *(uint8_t**)d : nullptr;
                uint8_t* xanim = tree ? *(uint8_t**)tree : nullptr;
                dp.Put<uint16_t>((uint16_t)e);
                dp.Put<uint8_t>((uint8_t)nm);
                dp.Put<uint8_t>(d ? d[0x1a] : 0);
                dp.Put(xanim);
                dp.Put<int>(xanim ? *(int*)(xanim + 4) : 0);
                dp.Put(idx, nm);
                if (nm) dp.Put(d + 0x3c, nm);
                g_dobjSig[n][e] = sig;
                g_dobjSent[n][e] = now;
            }
        }
        uint8_t* tree = d ? *(uint8_t**)d : nullptr;
        if (!tree) continue;
        int size = *(int*)(*(uint8_t**)tree + 4);
        int used = 0;
        for (int a = 0; a < size; ++a) used += *(uint16_t*)(tree + 8 + a * 2) != 0;
        if (ap.len + 4 + used * (2 + INFO_STATE_SIZE) > (int)sizeof ap.buf - 64) flush(ap, M_ANIM);
        ap.Put<uint16_t>((uint16_t)e);
        ap.Put<uint16_t>((uint16_t)used);
        for (int a = 0; a < size; ++a) {
            uint16_t info = *(uint16_t*)(tree + 8 + a * 2);
            if (!info) continue;
            ap.Put<uint16_t>((uint16_t)a);
            ap.Put((uint8_t*)XANIM_INFOS + info * INFO_SIZE + INFO_STATE, INFO_STATE_SIZE);
        }
    }
    flush(dp, M_DOBJ);
    flush(ap, M_ANIM);
}

bool g_ourFree;  // Cod2_KeepDObj deja borrar los cuerpos solo desde aca
// El snapshot de CoD2 lleva solo los numeros de las entidades: el cliente copia el estado de cada una (los primeros
// 0xa0 bytes de la gentity, 0x4a4330) directo de la memoria del juego. En el que se une esa memoria es la de su juego
// congelado, asi que el host le manda el estado de las entidades de su snapshot: solo las palabras que cambiaron desde
// lo ultimo que le mando, y cada tanto una entera (por si se perdio un paquete).
constexpr int ES_WORDS = 0xa0 / 4, ENT_PACKET = 1200, REFRESH_PER_SNAP = 8;
uint32_t g_entSent[MAX_CLIENTS][MAX_ENTS][ES_WORDS];
bool g_entKnown[MAX_CLIENTS][MAX_ENTS];
int g_entRefresh[MAX_CLIENTS];

void HostSendEntities(int n)
{
    const int count = *(int*)0x8224a0;
    const int* list = (const int*)0x8224a4;
    static Packet ep(M_ENTS);
    ep = Packet(M_ENTS);
    int refresh = count ? g_entRefresh[n] % count : 0, refreshed = 0;
    for (int i = 0; i < count; ++i) {
        int e = list[i];
        if (e < 0 || e >= MAX_ENTS) continue;
        const uint32_t* now = (const uint32_t*)Entity(e);
        uint32_t* sent = g_entSent[n][e];
        bool full = !g_entKnown[n][e] || (refreshed < REFRESH_PER_SNAP && i >= refresh && ++refreshed);
        uint8_t mask[5] = {};
        int words = 0;
        for (int w = 0; w < ES_WORDS; ++w)
            if (full || now[w] != sent[w]) mask[w >> 3] |= 1 << (w & 7), ++words;
        if (!words) continue;
        if (ep.len + 7 + words * 4 > ENT_PACKET) {
            ep.Send(g_remote[n].addr);
            ep = Packet(M_ENTS);
        }
        ep.Put<uint16_t>((uint16_t)e);
        ep.Put(mask, 5);
        for (int w = 0; w < ES_WORDS; ++w)
            if (mask[w >> 3] & (1 << (w & 7))) ep.Put(now[w]), sent[w] = now[w];
        g_entKnown[n][e] = true;
    }
    if (ep.len > 5) ep.Send(g_remote[n].addr);
    g_entRefresh[n] = refresh + REFRESH_PER_SNAP;
}

// joiner: se escribe en sus gentities (su juego no corre); el cliente lo copia al procesar el snapshot
void JoinerApplyEntities(const uint8_t* p, int len)
{
    const uint8_t* end = p + len;
    while (p + 7 <= end) {
        int e = *(const uint16_t*)p;
        const uint8_t* mask = p + 2;
        p += 7;
        if (e >= MAX_ENTS) return;
        uint32_t* st = (uint32_t*)Entity(e);
        for (int w = 0; w < ES_WORDS; ++w) {
            if (!(mask[w >> 3] & (1 << (w & 7)))) continue;
            if (p + 4 > end) return;
            st[w] = *(const uint32_t*)p;
            p += 4;
        }
    }
}

// Los arboles de animacion salen de la memoria de los scripts (XAnimCreateTree 0x42f280 pide 5 * nodos + 9 bytes a
// MT_AllocIndex). Como G_FreeEntity (0x4d7d41..0x4d7d77): XAnimClearTree 0x478db0 (arbol, 0), XAnimFreeTree 0x478e10
// (arbol) y MT_Free 0x45ee80 (arbol, tamano). Sin esto cada DObj rearmado dejaba su arbol (5 KB un soldado) y en
// niveles con muchos soldados (duhoc_assault) el que se une se quedaba sin memoria en un minuto ("failed memory
// allocation for script usage"). Solo los arboles armados aca: los de las entidades del juego (ent+0x1f4) los suelta el.
constexpr int GE_TREE = 0x1f4;
uint8_t* g_ownTree[MAX_ENTS];
void FreeTree(uint8_t* tree)
{
    int size = *(int*)(*(uint8_t**)tree + 4) * 5 + 9;
    ((void(__cdecl*)(uint8_t*, int))0x478db0)(tree, 0);
    ((void(__cdecl*)(uint8_t*))0x478e10)(tree);
    ((void(__cdecl*)(uint8_t*, int))0x45ee80)(tree, size);
}

void FreeClientDObj(int e)
{
    uint8_t* d = ClientDObj(e);
    uint8_t* tree = d ? *(uint8_t**)d : nullptr;
    if (tree && tree == g_ownTree[e]) FreeTree(tree);
    g_ownTree[e] = nullptr;
    int ent = e;
    g_ourFree = true;
    asm volatile("call *%1" : "+c"(ent) : "r"(CL_DOBJ_FREE) : "eax", "edx", "memory");
    g_ourFree = false;
}

uint8_t* CreateTree(uint8_t* xanim)
{
    uint8_t* tree;
    asm volatile("call *%2" : "=a"(tree), "+b"(xanim) : "r"(XANIM_CREATE_TREE) : "ecx", "edx", "memory");
    return tree;
}

void CreateClientDObj(int e, const void* models, int count, uint8_t* tree)
{
    int ent = e;
    asm volatile("push %3\n\tpush %2\n\tpush %1\n\tcall *%4\n\tadd $12, %%esp"
                 : "+a"(ent)
                 : "r"(models), "r"(count), "r"(tree), "r"(CL_DOBJ_CREATE)
                 : "ecx", "edx", "memory");
}

void JoinerApplyDObjs(const uint8_t* p, int len)
{
    const uint8_t* end = p + len;
    while (p + 12 <= end) {
        int e = *(const uint16_t*)p, nm = p[2], noCollide = p[3];
        uint8_t* xanim = *(uint8_t* const*)(p + 4);
        int size = *(const int*)(p + 8);
        p += 12;
        if (e >= MAX_ENTS || nm > MAX_MODELS_PER || p + nm * 2 > end) return;
        const uint8_t *idx = p, *parents = p + nm;
        p += nm * 2;
        if (!nm) {
            if (ClientDObj(e)) FreeClientDObj(e);
            continue;
        }
        // el XAnim se usa por direccion (el mismo mapa lo carga en el mismo lugar): se comprueba antes de usarlo
        if (xanim && (IsBadReadPtr(xanim, 8) || *(int*)(xanim + 4) != size)) {
            static int warned;
            if (warned++ < 5) Log("joiner: el XAnim %p del host no coincide aca (entidad %d)", (void*)xanim, e);
            xanim = nullptr;
        }
        struct { void* model; const char* tag; int noCollide; } models[MAX_MODELS_PER];
        for (int k = 0; k < nm; ++k) models[k] = {((void**)G_MODELS)[idx[k]], nullptr, (noCollide >> k) & 1};
        uint8_t* d = ClientDObj(e);
        bool same = d && d[0x18] == nm && (*(uint8_t**)d ? **(uint8_t***)d : nullptr) == xanim;
        for (int k = 0; same && k < nm; ++k) same = *(void**)(d + 0x1c + k * 4) == models[k].model;
        if (!same) {
            if (d) FreeClientDObj(e);
            uint8_t* tree = xanim ? CreateTree(xanim) : nullptr;
            CreateClientDObj(e, models, nm, tree);
            d = ClientDObj(e);
            if (tree && !d) FreeTree(tree);
            g_ownTree[e] = d ? tree : nullptr;
            static int created;
            if (created++ < 20) Log("joiner: DObj %d con %d modelos (arbol %p)", e, nm, (void*)tree);
            if (!d) continue;
        }
        memcpy(d + 0x3c, parents, nm);
    }
}

void JoinerApplyAnims(const uint8_t* p, int len)
{
    const uint8_t* end = p + len;
    static uint8_t seen[4096];
    while (p + 4 <= end) {
        int e = *(const uint16_t*)p, count = *(const uint16_t*)(p + 2);
        p += 4;
        if (p + count * (2 + INFO_STATE_SIZE) > end || e >= MAX_ENTS) return;
        uint8_t* d = ClientDObj(e);
        uint8_t* tree = d ? *(uint8_t**)d : nullptr;
        int size = tree ? *(int*)(*(uint8_t**)tree + 4) : 0;
        if (size > (int)sizeof seen) size = 0;
        memset(seen, 0, size);
        for (int k = 0; k < count; ++k, p += 2 + INFO_STATE_SIZE) {
            int a = *(const uint16_t*)p;
            if (a >= size) continue;
            seen[a] = 1;
            uint16_t* slot = (uint16_t*)(tree + 8 + a * 2);
            if (!*slot) {  // como XAnimSetGoalWeight (0x478f10) cuando el nodo no tenia info
                int node = a;
                uint8_t* t = tree;
                uint8_t* info;
                asm volatile("push %2\n\tcall *%3\n\tadd $4, %%esp" : "=a"(info), "+S"(t) : "r"(node), "r"(XANIM_ALLOC_INFO)
                             : "ecx", "edx", "memory");
                if (!info) continue;
                memset(info + 0xc, 0, 0x1c);
                *(uint16_t*)(info + 2) = 0xffff;
                *(uint16_t*)info = *(uint16_t*)(info + 4) = *(uint16_t*)(info + 6) = 0;
            }
            memcpy((uint8_t*)XANIM_INFOS + *slot * INFO_SIZE + INFO_STATE, p + 2, INFO_STATE_SIZE);
        }
        for (int a = 0; a < size; ++a) {  // lo que el host ya no tiene queda sin peso
            uint16_t info = *(uint16_t*)(tree + 8 + a * 2);
            if (info && !seen[a]) memset((uint8_t*)XANIM_INFOS + info * INFO_SIZE + 0x1c, 0, 8);
        }
    }
}

// ---------- cuerpos de los jugadores (en el SP el jugador no tiene modelo) ----------
// Con algun remoto, el host le arma a cada jugador un DObj de soldado (cuerpo, cabeza y casco de algun soldado aliado
// del nivel, con el XAnim de los humanos) y lo anima segun como se mueve. Mientras se arman los snapshots la entidad
// del jugador pasa a "general" (eType 0, sin cabeceo): el cliente dibuja su DObj; despues vuelve a ser jugador.
// XAnim: nodo i en +0xc + i*8 {u16 hijos, u16 padre, ..., XAnimParts* si es hoja}; XAnimParts +8 frecuencia
// (vueltas por segundo), +0x24 nombre.
constexpr int PS_CLIENTNUM = 0xa8;
constexpr int PS_PMFLAGS = 0xc, PS_ORIGIN = 0x14, PS_VELOCITY = 0x20, PS_YAW = 0xc8, ES_PITCH = 0x44, ES_ROLL = 0x4c;
enum BodyAnim { BA_IDLE, BA_WALK, BA_RUN, BA_BACK, BA_LEFT, BA_RIGHT, BA_DOWNED, BA_COUNT };
const char* const kBodyAnimName[BA_COUNT] = {"stand_aim_straight", "stand_walk_combat_loop_01", "combatrun_forward_1",
                                             "combatrun_back",     "combatrun_left",            "combatrun_right",
                                             "wounded_crawlidle"};
uint8_t* g_bodyTree[MAX_CLIENTS];
int g_bodyAnim[MAX_CLIENTS] = {-1, -1, -1, -1};
uint8_t* g_humanXAnim;
int g_animIndex[BA_COUNT];

uint8_t* XAnimNode(uint8_t* xanim, int i) { return xanim + 0xc + i * 8; }
uint8_t* XAnimLeafParts(uint8_t* xanim, int i)
{
    uint8_t* node = XAnimNode(xanim, i);
    return *(uint16_t*)node ? nullptr : *(uint8_t**)(node + 4);
}

int AnimIndex(uint8_t* xanim, const char* name)
{
    int size = *(int*)(xanim + 4);
    for (int i = 0; i < size; ++i) {
        uint8_t* parts = XAnimLeafParts(xanim, i);
        const char* n = parts && !IsBadReadPtr(parts, 0x28) ? *(const char**)(parts + 0x24) : nullptr;
        if (n && !IsBadReadPtr(n, 1) && !_stricmp(n, name)) return i;
    }
    return -1;
}

const char* ModelName(void* model) { return model ? *(const char**)((uint8_t*)model + 0x88) : ""; }

// Arma enganchada a una entidad (ent+0x1d4 indices de modelo, ent+0x1dc el tag de cada uno como string del juego,
// como los arma G_DObjUpdate 0x4d6110): el primer modelo "weapon_*" y su tag.
constexpr uintptr_t SL_STRINGS = 0x8aed00;  // base de los strings del script; el string i esta en base + 4 + i * 8
bool EntityWeapon(int e, void** model, const char** tag)
{
    uint8_t* ent = Entity(e);
    for (int j = 0; j < 7; ++j) {
        int m = ent[0x1d4 + j], t = *(uint16_t*)(ent + 0x1dc + j * 2);
        void* xm = m ? ((void**)G_MODELS)[m] : nullptr;
        if (!xm || !t || _strnicmp(ModelName(xm), "weapon_", 7)) continue;
        *model = xm;
        *tag = (const char*)(*(uintptr_t*)SL_STRINGS + 4 + t * 8);
        return true;
    }
    return false;
}

void* LevelModel(const char* name)  // modelo del nivel por nombre (sin "xmodel/")
{
    for (int i = 1; i < 256; ++i) {
        void* m = ((void**)G_MODELS)[i];
        if (m && !_stricmp(ModelName(m), name)) return m;
    }
    return nullptr;
}

// En los niveles sin soldados aliados (libya, los de tanques) no hay de donde copiar un cuerpo aliado: al terminar la
// precarga del nivel (0x4da470 corre el primer frame con level.initializing 0x1c60528 en 1) se precargan cuerpo, cabeza
// y casco de la campana del mapa, como la elige maps\_loadout.gsc. Pasa igual en el host y en el que se une (mismo mapa,
// misma precarga), asi que quedan con el mismo indice de modelo en los dos.
// G_ModelIndex (0x4d59f0, EDI = "xmodel/...") corta el juego si no hay lugar: el modelo i esta libre si su configstring
// (u16 en 0x81545a + i*2, string del script) es "".
constexpr uintptr_t G_MODEL_INDEX = 0x4d59f0, PRECACHE_END = 0x4da52c, MODEL_CONFIGSTRINGS = 0x81545a;
constexpr uintptr_t SHUTDOWN_GAME_CALL = 0x4489b1;  // SV_ShutdownGameProgs (0x4489a0) -> G_ShutdownGame (0x4d9ce0)
const char* const* CampaignBody()
{
    static const char* const russian[] = {"xmodel/character_russian_trench_b", "xmodel/head_russian_winter_jon",
                                          "xmodel/helmet_russian_padded_b"};
    static const char* const africa[] = {"xmodel/character_british_afrca_body", "xmodel/head_british_paul",
                                         "xmodel/helmet_british_afrca"};
    static const char* const normandy[] = {"xmodel/character_british_normandy_a", "xmodel/head_british_paul",
                                           "xmodel/helmet_british_normandy_a"};
    static const char* const american[] = {"xmodel/character_us_ranger_pvt_a", "xmodel/head_us_ranger_david",
                                           "xmodel/helmet_us_ranger_generic"};
    const char* map = MapName();
    for (const char* m : {"moscow", "trainyard", "downtown_sniper", "downtown_assault", "cityhall", "tankhunt", "demolition"})
        if (!_stricmp(map, m)) return russian;
    for (const char* m : {"decoytrenches", "decoytown", "elalamein", "eldaba", "libya", "88ridge", "toujane",
                          "toujane_ride", "matmata"})
        if (!_stricmp(map, m)) return africa;
    for (const char* m : {"beltot", "crossroads", "newvillers", "breakout"})
        if (!_stricmp(map, m)) return normandy;
    return american;
}

extern "C" void __cdecl Cod2_PrecacheEnd()
{
    int firstFree = 1;
    uintptr_t base = *(uintptr_t*)SL_STRINGS;
    for (; firstFree < 256; ++firstFree) {
        uint16_t s = *(uint16_t*)(MODEL_CONFIGSTRINGS + firstFree * 2);
        if (!s || !*(const char*)(base + 4 + s * 8)) break;
    }
    const char* const* names = CampaignBody();
    if (firstFree + 3 > 256) {
        Log("precarga: sin lugar para el cuerpo aliado (%d modelos)", firstFree - 1);
        return;
    }
    int idx[3];
    for (int k = 0; k < 3; ++k) {
        const char* name = names[k];
        asm volatile("call *%2" : "=a"(idx[k]), "+D"(name) : "r"(G_MODEL_INDEX) : "ecx", "edx", "memory");
    }
    Log("precarga: cuerpo aliado %s (modelos %d %d %d)", names[0] + 7, idx[0], idx[1], idx[2]);
}

// Partes de un soldado aliado del nivel (de algun DObj con el XAnim de los humanos): cuerpo, cabeza, casco/gorra y
// su arma. Primero uno con arma (los personajes con nombre no la tienen en el DObj). Si en el nivel no hay aliados, el
// precargado de la campana con el XAnim de cualquier humano.
int FindBodyModels(void** out, void** weapon, const char** tag)
{
    for (int pass = 0; pass < 3; ++pass)
        for (int e = MAX_CLIENTS; e < MAX_ENTS; ++e) {
            uint8_t* d = ClientDObj(e);
            uint8_t* tree = d ? *(uint8_t**)d : nullptr;
            if (!tree || *(int*)(*(uint8_t**)tree + 4) < 500 || d[0x18] < 2) continue;
            const char* body = ModelName(*(void**)(d + 0x1c));
            if (_strnicmp(body, "character_", 10)) continue;
            bool allied = strstr(body, "russian") || strstr(body, "british") || strstr(body, "american") || strstr(body, "us_");
            bool armed = EntityWeapon(e, weapon, tag);
            if ((pass == 0 && !(allied && armed)) || (pass == 1 && !allied)) continue;
            if (!armed) *weapon = nullptr;
            int count = 0;
            if (pass == 2) {
                const char* const* names = CampaignBody();
                while (count < 3 && (out[count] = LevelModel(names[count] + 7))) ++count;
                if (count < 3) count = 0;
            }
            for (int k = 0; k < d[0x18] && count < 3; ++k) {
                void* m = *(void**)(d + 0x1c + k * 4);
                const char* name = ModelName(m);
                if (k < 2 || !_strnicmp(name, "helmet", 6) || !_strnicmp(name, "gear_", 5)) out[count++] = m;
            }
            g_humanXAnim = *(uint8_t**)tree;
            return count;
        }
    return 0;
}

// El arma que el jugador tiene en la mano (playerState +0xb0): su definicion (0x1c64548[arma], como la usa
// ClientEndFrame) trae en +0x1b4 el nombre del modelo ("xmodel/weapon_svt40"), que se busca entre los modelos del
// nivel. Si no esta cargado queda el arma del soldado copiado.
constexpr uintptr_t WEAPON_DEFS = 0x1c64548;
constexpr int PS_WEAPON = 0xb0, WD_WORLD_MODEL = 0x1b4;
int g_bodyWeapon[MAX_CLIENTS];

void* PlayerWeaponModel(int n)
{
    int w = *(int*)(GClient(n) + PS_WEAPON);
    uint8_t* def = w > 0 && w < 128 ? ((uint8_t**)WEAPON_DEFS)[w] : nullptr;
    const char* name = def && !IsBadReadPtr(def + WD_WORLD_MODEL, 4) ? *(const char**)(def + WD_WORLD_MODEL) : nullptr;
    if (!name || IsBadReadPtr(name, 8)) return nullptr;
    if (!_strnicmp(name, "xmodel/", 7)) name += 7;
    return LevelModel(name);
}

// "tag_weapon_right" como string del juego, cuando ningun soldado del nivel tiene arma (niveles de tanques)
const char* SlString(const char* text)
{
    uintptr_t base = *(uintptr_t*)SL_STRINGS;
    size_t len = strlen(text) + 1;
    for (int i = 1; i < 0x10000; ++i) {
        const char* p = (const char*)(base + 4 + i * 8);
        if (IsBadReadPtr(p, len)) break;
        if (!memcmp(p, text, len)) return p;
    }
    return nullptr;
}

void CreateBody(int n)
{
    void* parts[3];
    void* weapon = nullptr;
    const char* tag = nullptr;
    int count = FindBodyModels(parts, &weapon, &tag);
    if (count < 2 || !g_humanXAnim) return;
    g_bodyWeapon[n] = *(int*)(GClient(n) + PS_WEAPON);
    if (void* mine = PlayerWeaponModel(n)) {
        weapon = mine;
        if (!tag) tag = SlString("tag_weapon_right");
        if (!tag) weapon = nullptr;
    }
    struct { void* model; const char* tag; int noCollide; } models[4];
    int nm = 0;
    for (int k = 0; k < count; ++k) models[nm++] = {parts[k], nullptr, 1};
    if (weapon) models[nm++] = {weapon, tag, 1};
    if (ClientDObj(n)) FreeClientDObj(n);
    uint8_t* tree = CreateTree(g_humanXAnim);
    CreateClientDObj(n, models, nm, tree);
    g_ownTree[n] = tree;
    g_bodyTree[n] = tree;
    g_bodyAnim[n] = -1;
    for (int a = 0; a < BA_COUNT; ++a) g_animIndex[a] = AnimIndex(g_humanXAnim, kBodyAnimName[a]);
    Log("cuerpo %d: %s + %s%s%s, arma %s (%s)", n + 1, ModelName(parts[0]), ModelName(parts[1]), count > 2 ? " + " : "",
        count > 2 ? ModelName(parts[2]) : "", weapon ? ModelName(weapon) : "-", tag ? tag : "-");
}

int ChooseBodyAnim(int n)
{
    if (g_downed[n]) return BA_DOWNED;
    uint8_t* ps = GClient(n);
    const float* v = (const float*)(ps + PS_VELOCITY);
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
    if (speed < 5) return BA_IDLE;
    float rel = atan2f(v[1], v[0]) * 180.0f / 3.14159265f - *(float*)(ps + PS_YAW);
    rel -= 360.0f * floorf((rel + 180.0f) / 360.0f);  // a [-180, 180)
    if (fabsf(rel) <= 50) return speed < 120 ? BA_WALK : BA_RUN;
    if (fabsf(rel) >= 130) return BA_BACK;
    return rel > 0 ? BA_LEFT : BA_RIGHT;
}

uint8_t* TreeInfo(uint8_t* tree, int node, bool alloc)
{
    uint16_t* slot = (uint16_t*)(tree + 8 + node * 2);
    if (!*slot && alloc) {
        uint8_t* t = tree;
        uint8_t* info;
        asm volatile("push %2\n\tcall *%3\n\tadd $4, %%esp" : "=a"(info), "+S"(t) : "r"(node), "r"(XANIM_ALLOC_INFO)
                     : "ecx", "edx", "memory");
        if (!info) return nullptr;
        memset(info + 0xc, 0, 0x1c);
        *(uint16_t*)(info + 2) = 0xffff;
        *(uint16_t*)info = *(uint16_t*)(info + 4) = *(uint16_t*)(info + 6) = 0;
    }
    return *slot ? (uint8_t*)XANIM_INFOS + *slot * INFO_SIZE : nullptr;
}

void AnimateBody(int n, float dt)
{
    uint8_t* tree = g_bodyTree[n];
    uint8_t* xanim = *(uint8_t**)tree;
    int size = *(int*)(xanim + 4);
    int want = ChooseBodyAnim(n), leaf = g_animIndex[want];
    if (leaf <= 0) return;
    if (want != g_bodyAnim[n]) {  // todo apagado y la cadena hoja -> raiz con peso 1
        for (int a = 0; a < size; ++a)
            if (uint8_t* info = TreeInfo(tree, a, false)) *(float*)(info + 0x1c) = *(float*)(info + 0x20) = 0;
        for (int node = leaf;; node = *(uint16_t*)(XAnimNode(xanim, node) + 2)) {
            uint8_t* info = TreeInfo(tree, node, true);
            if (info) *(float*)(info + 0x1c) = *(float*)(info + 0x20) = *(float*)(info + 0x24) = 1;
            if (!node) break;
        }
        g_bodyAnim[n] = want;
    }
    uint8_t* info = TreeInfo(tree, leaf, false);
    float freq = *(float*)(XAnimLeafParts(xanim, leaf) + 8);
    if (!info) return;
    float& time = *(float*)(info + 0xc);
    *(float*)(info + 0x10) = time;  // hora anterior
    time += dt * freq;
    if (time >= 1) time -= floorf(time);
}

// host, cada frame antes de los snapshots
void HostBodies(float dt)
{
    bool anyRemote = false;
    for (int n = 1; n < MAX_CLIENTS; ++n) anyRemote |= g_remote[n].active;
    for (int n = 0; n < MAX_CLIENTS; ++n) {
        bool want = anyRemote && *(int*)(GClient(n) + GC_CONNECTED) == 2;
        uint8_t* d = ClientDObj(n);
        bool has = g_bodyTree[n] && d && *(uint8_t**)d == g_bodyTree[n];
        if (!want) {
            if (has) FreeClientDObj(n);
            g_bodyTree[n] = nullptr;
            continue;
        }
        if (!has || g_bodyWeapon[n] != *(int*)(GClient(n) + PS_WEAPON)) CreateBody(n);  // nuevo o cambio de arma
        if (g_bodyTree[n]) AnimateBody(n, dt);
    }
}

// desde los snapshots hasta que el cliente local copia los estados (0x4a4330), los jugadores con cuerpo son entidades
// generales (el cliente, 0x4acf30, no dibuja jugadores y dibuja las generales con su DObj de la tabla del cliente
// 0x77f480) y sin cabeceo
constexpr uint8_t ET_BODY = 0;  // 9 (como soldado) no: el juego lo trata como IA y se cae (0x4e804a)
struct SavedPlayer { uint8_t eType; float pitch, roll; } g_savedPlayer[MAX_CLIENTS];
void PlayersAsBodies(bool on)
{
    for (int n = 0; n < MAX_CLIENTS; ++n) {
        if (!g_bodyTree[n]) continue;
        uint8_t* s = Entity(n);
        if (on) {
            g_savedPlayer[n] = {s[0], *(float*)(s + ES_PITCH), *(float*)(s + ES_ROLL)};
            s[0] = ET_BODY;
            *(float*)(s + ES_PITCH) = *(float*)(s + ES_ROLL) = 0;
        } else {
            s[0] = g_savedPlayer[n].eType;
            *(float*)(s + ES_PITCH) = g_savedPlayer[n].pitch;
            *(float*)(s + ES_ROLL) = g_savedPlayer[n].roll;
        }
    }
}

// ---------- caidos ----------
// Como en CoD1: con algun remoto, un jugador que se queda sin vida cae (no se mueve ni dispara, no recibe dano) y un
// companero lo levanta acercandose y manteniendo Usar 3 s. Si no queda nadie en pie muere el host de verdad: eso es lo
// que hace fallar la mision en el SP y volver al ultimo punto de control con todos. Un remoto no muere nunca (su
// muerte hacia que el host recargue el nivel).
// G_Damage (0x528050, cdecl): targ, inflictor, attacker, dir, point, dano, flags, medio, parte del cuerpo.
// gentity: +0x13c vida, +0x140 vida maxima; gclient +0x108 la vida que muestra el HUD.
constexpr uintptr_t G_DAMAGE = 0x528050;
constexpr int GE_HEALTH = 0x13c, GE_MAXHEALTH = 0x140, GC_HEALTH = 0x108;
constexpr int BUTTON_USE = 8, REVIVE_DIST = 80;
constexpr float REVIVE_SECONDS = 3;
using GDamageFn = void(__cdecl*)(uint8_t*, uint8_t*, uint8_t*, float*, float*, int, int, int, int);
GDamageFn g_gDamage;

bool AnyRemote()
{
    for (int n = 1; n < MAX_CLIENTS; ++n)
        if (g_remote[n].active) return true;
    return false;
}

bool PlayerIn(int n) { return n == 0 || (g_remote[n].active && *(int*)(GClient(n) + GC_CONNECTED) == 2); }

int PlayerOf(const uint8_t* ent)  // numero de jugador de una gentity, o -1
{
    if (!ent) return -1;
    int off = (int)(ent - (const uint8_t*)G_ENTITIES);
    return off >= 0 && off % GENTITY_SIZE == 0 && off / GENTITY_SIZE < MAX_CLIENTS ? off / GENTITY_SIZE : -1;
}

void SetHealth(int n, int h)
{
    *(int*)(Entity(n) + GE_HEALTH) = h;
    *(int*)(GClient(n) + GC_HEALTH) = h;
}

bool AnyOtherStanding(int n)
{
    for (int m = 0; m < MAX_CLIENTS; ++m)
        if (m != n && PlayerIn(m) && !g_downed[m] && *(int*)(Entity(m) + GE_HEALTH) > 0) return true;
    return false;
}

// Mensaje en pantalla para todos, como iprintln/iprintlnbold (0x4ebc00): comando "gm"/"gmb" del servidor al cliente
// local (SV_SendServerCommand 0x44a3b0, ECX = cliente, 0 = el local); su bloque de comandos les llega a los remotos.
void Announce(bool bold, const char* text)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s \"%s\"", bold ? "gmb" : "gm", text);
    const char* fmt = "%s";
    int cl = 0;
    asm volatile("push %1\n\tpush %2\n\tcall *%3\n\tadd $8, %%esp" : "+c"(cl) : "r"(cmd), "r"(fmt), "r"((uintptr_t)0x44a3b0)
                 : "eax", "edx", "memory");
}

void SetDowned(int n, bool down)
{
    g_downed[n] = down;
    g_reviveTime[n] = 0;
    Log("jugador %d: %s", n + 1, down ? "caido" : "levantado");
    char text[128];
    snprintf(text, sizeof text, down ? "^1Player %d is down^7: get close and hold Use for 3 seconds"
                                     : "^2Player %d was revived", n + 1);
    Announce(down, text);
}

extern "C" void __cdecl Cod2_GDamage(uint8_t* targ, uint8_t* inflictor, uint8_t* attacker, float* dir, float* point,
                                     int damage, int dflags, int mod, int part)
{
    int n = PlayerOf(targ);
    if (g_role != HOST || !AnyRemote() || n < 0 || !PlayerIn(n)) {
        g_gDamage(targ, inflictor, attacker, dir, point, damage, dflags, mod, part);
        return;
    }
    int a = PlayerOf(attacker);
    if (a >= 0 && a != n) return;  // un jugador no lastima a otro (a si mismo si, como en el SP)
    if (g_downed[n]) return;       // caido: ya no recibe dano
    // El dano final depende de la parte del cuerpo y la dificultad (G_Damage lo escala): se aplica con la vida
    // inflada para que no muera adentro, se mide cuanto bajo y recien ahi se decide entre herido, caido o muerto.
    constexpr int BOOST = 100000;
    int health = *(int*)(targ + GE_HEALTH);
    SetHealth(n, health + BOOST);
    g_gDamage(targ, inflictor, attacker, dir, point, damage, dflags, mod, part);
    int left = *(int*)(targ + GE_HEALTH) - BOOST;
    Log("dano al jugador %d: %d (medio %d, atacante %d) -> vida %d", n + 1, damage, mod,
        attacker ? (int)(attacker - (uint8_t*)G_ENTITIES) / GENTITY_SIZE : -1, left);
    if (left > 0) {
        SetHealth(n, left);
    } else if (AnyOtherStanding(n)) {
        SetHealth(n, 1);
        SetDowned(n, true);
    } else {
        Log("jugador %d: cayo el ultimo en pie -> muere el host", n + 1);
        if (n != 0) {
            SetHealth(n, 1);
            SetDowned(n, true);
        }
        if (g_downed[0]) g_downed[0] = false;
        SetHealth(0, 1);
        g_gDamage(Entity(0), inflictor, attacker, dir, point, BOOST, dflags, mod, part);
    }
}

// caido: el usercmd queda sin movimiento ni botones (sigue mirando)
void DownedCmd(uint8_t* cmd)
{
    *(int*)(cmd + 4) = 0;
    cmd[0x18] = cmd[0x19] = cmd[0x1a] = 0;
}

// host, cada frame: un companero en pie cerca manteniendo Usar lo levanta
void HostDowned(float dt)
{
    for (int n = 0; n < MAX_CLIENTS; ++n) {
        if (!g_downed[n]) continue;
        if (!PlayerIn(n)) {
            g_downed[n] = false;
            continue;
        }
        const float* o = (const float*)(GClient(n) + PS_ORIGIN);
        int helper = -1;
        for (int m = 0; m < MAX_CLIENTS && helper < 0; ++m) {
            if (m == n || !PlayerIn(m) || g_downed[m]) continue;
            const float* q = (const float*)(GClient(m) + PS_ORIGIN);
            float dx = o[0] - q[0], dy = o[1] - q[1], dz = o[2] - q[2];
            if (dx * dx + dy * dy + dz * dz < REVIVE_DIST * REVIVE_DIST &&
                (*(int*)(Client(m) + CL_LASTUSERCMD + 4) & BUTTON_USE))
                helper = m;
        }
        bool helped = helper >= 0;
        if (helped && g_reviveTime[n] == 0)
            Log("jugador %d levanta al %d (botones %x)", helper + 1, n + 1, *(int*)(Client(helper) + CL_LASTUSERCMD + 4));
        g_reviveTime[n] = helped ? g_reviveTime[n] + dt : 0;
        if (g_reviveTime[n] >= REVIVE_SECONDS) {
            int max = *(int*)(Entity(n) + GE_MAXHEALTH);
            SetHealth(n, max > 1 ? max / 2 : 50);
            SetDowned(n, false);
        }
    }
}

// prueba: coop2hurt <jugador 1-4> [dano] -> G_Damage del mundo sobre ese jugador
void Cmd_Hurt()
{
    if (*(int*)0x6c0280 < 2) return;
    const char** argv = (const char**)0x6bb680;
    int n = atoi(argv[1]) - 1, damage = *(int*)0x6c0280 > 2 ? atoi(argv[2]) : 100000;
    if (n < 0 || n >= MAX_CLIENTS || !PlayerIn(n)) return;
    Cod2_GDamage(Entity(n), nullptr, nullptr, nullptr, nullptr, damage, 0, 0, 0);
    Log("coop2hurt %d %d -> vida %d", n + 1, damage, *(int*)(Entity(n) + GE_HEALTH));
}

// prueba: coop2save -> punto de control como el savegame() de los scripts (0x4f3a30): autosave/<mapa> por
// SV_AddPendingSave (0x44a140: EDI = 1 autoguardado, EBX = archivo; descripcion, captura, 2)
void Cmd_Save()
{
    static char file[64];
    snprintf(file, sizeof file, "autosave/%s", MapName());
    const char *desc = "", *shot = "_default";
    int r, autosave = 1;
    char* f = file;
    asm volatile("push $2\n\tpush %4\n\tpush %3\n\tcall *%5\n\tadd $12, %%esp"
                 : "=a"(r), "+D"(autosave), "+b"(f) : "r"(desc), "r"(shot), "r"((uintptr_t)0x44a140) : "ecx", "edx", "memory");
    Log("coop2save %s -> %d", file, r);
}

// ---------- host ----------
// El script del nivel arma solo al jugador 1 (level.player): el remoto entra con el mismo equipo que el host.
// playerState: +0xac granada (offHandIndex), +0xb0 arma, +0xc0 modelo de la vista, +0xf8 municion de reserva[128],
// +0x2f8 cargador[128], +0x4f8..+0x537 bits de armas que tiene
void CopyLoadout(int n)
{
    uint8_t *from = GClient(0), *to = GClient(n);
    struct { int at, len; } parts[] = {{0xac, 8}, {0xc0, 4}, {0xf8, 0x538 - 0xf8}};
    for (auto& r : parts) memcpy(to + r.at, from + r.at, r.len);
}

// "carga este mapa y volve a entrar": al que se une desde otro mapa o el menu, y a los que el host ya no conoce
// (cargo un punto de control o cambio de nivel)
void SendRejoin(const sockaddr_in& to, bool now = false)
{
    static DWORD last;
    if (!now && GetTickCount() - last < 500) return;
    last = GetTickCount();
    Packet p(M_REJOIN);
    const char* map = MapName();
    p.Put(map, (int)strlen(map) + 1);
    p.Send(to);
}

void HostJoin(const sockaddr_in& from, const uint8_t* body, int len)
{
    int n = 0;
    for (int i = 1; i < MAX_CLIENTS && !n; ++i)
        if (g_remote[i].active && SameAddr(g_remote[i].addr, from)) n = i;
    if (!n) {
        const char* theirs = len > 0 && !body[len - 1] ? (const char*)body : "";
        if (_stricmp(theirs, MapName())) {
            Log("host: %s:%d pide entrar desde \"%s\": que cargue %s", inet_ntoa(from.sin_addr), ntohs(from.sin_port),
                theirs, MapName());
            SendRejoin(from);
            return;
        }
        // Primero un lugar que trajo la partida cargada (punto de control grabado con ese jugador): lo retoma tal cual,
        // con su posicion, vida y armas. Si no, uno libre.
        bool restored = false;
        for (int i = 1; i < MAX_CLIENTS && !n; ++i)
            if (!g_remote[i].active && *(int*)(GClient(i) + GC_CONNECTED) == 2) n = i, restored = true;
        for (int i = 1; i < MAX_CLIENTS && !n; ++i)
            if (!g_remote[i].active && !*(int*)(GClient(i) + GC_CONNECTED)) n = i;
        if (!n) {
            Packet(M_FULL).Send(from);
            return;
        }
        g_remote[n] = {true, from};
        g_joinedAt[n] = g_lastHeard[n] = GetTickCount();
        memset(g_dobjSig[n], 0xff, sizeof g_dobjSig[n]);  // la primera vez va todo, incluso "sin DObj"
        memset(g_entKnown[n], 0, sizeof g_entKnown[n]);
        *(uint8_t**)(Client(n) + CL_GENTITY) = Entity(n);
        *(int*)(GClient(n) + PS_CLIENTNUM) = n;  // el SP lo deja en 0: el cliente remoto se tomaria por el host
        float* o = (float*)(GClient(n) + PS_ORIGIN);
        if (restored) {
            Log("host: %s:%d retoma al jugador %d de la partida cargada en %.0f %.0f %.0f", inet_ntoa(from.sin_addr),
                ntohs(from.sin_port), n + 1, o[0], o[1], o[2]);
            Status("Player %d is back.", n + 1);
            Packet p(M_ACCEPT);
            p.Put<int>(n);
            p.Send(from);
            return;
        }
        *(uint8_t**)(Entity(n) + GE_CLIENT) = GClient(n);  // G_FreeEntity lo borra si el lugar quedo vacio
        // ClientConnect empieza con la init de caminos de la IA (call 0x516bc0), pensada para el inicio del nivel: a
        // mitad de partida deja una lista circular y el juego se cuelga (igual que en CoD1). Se saltea.
        uint8_t saved[5];
        memcpy(saved, (void*)PATH_INIT_CALL, 5);
        bool skip = !memcmp(saved, "\xe8\x73\xdc\x02\x00", 5);
        if (skip) hook::Write(PATH_INIT_CALL, "\x0F\x1F\x44\x00\x00", 5);
        ClientConnect(n);
        if (skip) hook::Write(PATH_INIT_CALL, saved, 5);
        ClientBegin(n);
        *(int*)(GClient(n) + PS_CLIENTNUM) = n;
        CopyLoadout(n);
        // aparece donde esta el host (el punto de inicio del mapa puede quedar lejos o fuera de la zona de juego)
        memcpy(o, GClient(0) + PS_ORIGIN, 12);
        memset(GClient(n) + 0x20, 0, 12);  // velocidad
        *(int*)(Entity(n) + GE_MAXHEALTH) = *(int*)(Entity(0) + GE_MAXHEALTH);  // el SP solo se la pone al jugador 1
        SetHealth(n, *(int*)(Entity(n) + GE_MAXHEALTH));
        g_downed[n] = false;
        Log("host: %s:%d entra como jugador %d en %.0f %.0f %.0f", inet_ntoa(from.sin_addr), ntohs(from.sin_port), n + 1,
            o[0], o[1], o[2]);
        Status("Player %d joined.", n + 1);
    }
    Packet p(M_ACCEPT);
    p.Put<int>(n);
    p.Send(from);
}

void ApplyRide(int n);

void HostUsercmd(const sockaddr_in& from, const uint8_t* cmd, int len)
{
    bool known = false;
    for (int n = 1; n < MAX_CLIENTS; ++n) known |= g_remote[n].active && SameAddr(g_remote[n].addr, from);
    if (!known) {  // el host cargo una partida o cambio de nivel: los remotos entran de nuevo
        SendRejoin(from);
        return;
    }
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active || !SameAddr(g_remote[n].addr, from)) continue;
        if (len < USERCMD_SIZE || *(int*)(GClient(n) + GC_CONNECTED) != 2) return;
        memcpy(Client(n) + CL_LASTUSERCMD, cmd, USERCMD_SIZE);
        if (g_downed[n]) DownedCmd(Client(n) + CL_LASTUSERCMD);
        ClientThink(n);
        ApplyRide(n);  // en un vehiculo: su lugar sobre el
        static DWORD lastLog;
        static int count;
        ++count;
        if (GetTickCount() - lastLog > 2000) {
            lastLog = GetTickCount();
            Log("host: %d usercmds del jugador %d (cmd hora %d, ps commandTime %d, botones %x, adelante %d)", count, n + 1,
                *(const int*)cmd, *(int*)GClient(n), *(const int*)(cmd + 4), (int)(int8_t)cmd[0x18]);
            count = 0;
        }
        return;
    }
}

// Alcanzar al host (como en CoD1): un remoto a mas de CATCHUP_DIST durante CATCHUP_MS aparece en el rastro del host,
// los ultimos lugares donde piso (ps+0x60 groundEntityNum != 0x7ff), uno cada 32 unidades. Tambien cubre el inicio de
// niveles como moscow, donde el punto de inicio queda encerrado y el script sube al host a un camion.
constexpr float CATCHUP_DIST = 800;
constexpr DWORD CATCHUP_MS = 5000;
constexpr int TRAIL = 40, PS_GROUNDENT = 0x60, GE_CURRENT_ORIGIN = 0xdc;
float g_trail[TRAIL][3];
int g_trailCount, g_trailNext;
DWORD g_farSince[MAX_CLIENTS];

float Dist2(const float* a, const float* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// ---------- vehiculos ----------
// En los viajes (camion, lancha) y en los tanques que maneja el jugador, el script engancha al jugador 1 al vehiculo
// (gentity +0x1c8 -> tagInfo, +0 el padre). Los remotos se enganchan al mismo vehiculo como playerlinktodelta
// (0x4eda80): G_EntLinkTo (0x4d6540: EBX = entidad; padre, tag) y 0x4d6ca0 (entidad, 0). Enganchado, el juego no les
// aplica fisica, pero a un jugador lo pone siempre en el punto del vehiculo (0x4d6ec0, desde ClientEndFrame): su lugar
// (atras del host, a un costado) se guarda respecto del vehiculo y se vuelve a poner despues. Cuando el host se
// suelta, se sueltan (unlink 0x4ede30 -> G_EntUnlink 0x4d6720, EAX = entidad).
constexpr int GE_TAGINFO = 0x1c8, GE_ANGLES = 0xe8, GC_LINKCLAMP = 0x8694, GC_LINKFLAG = 0x8698;
constexpr uint32_t PMF_LINKVIEW = 0x10000000;
uint8_t* g_rideParent[MAX_CLIENTS];
float g_rideOffset[MAX_CLIENTS][3];  // adelante, derecha, arriba del vehiculo

void AngleAxis(const float* angles, float* f, float* r, float* u)  // AngleVectors de Quake 3
{
    const float k = 3.14159265f / 180;
    float sp = sinf(angles[0] * k), cp = cosf(angles[0] * k), sy = sinf(angles[1] * k), cy = cosf(angles[1] * k),
          sr = sinf(angles[2] * k), cr = cosf(angles[2] * k);
    f[0] = cp * cy, f[1] = cp * sy, f[2] = -sp;
    r[0] = -sr * sp * cy + cr * sy, r[1] = -sr * sp * sy - cr * cy, r[2] = -sr * cp;
    u[0] = cr * sp * cy + sr * sy, u[1] = cr * sp * sy - sr * cy, u[2] = cr * cp;
}

// pone al remoto n en su lugar sobre el vehiculo (posicion del jugador, de la entidad y del estado que se manda)
void ApplyRide(int n)
{
    uint8_t* parent = g_rideParent[n];
    if (!parent) return;
    float f[3], r[3], u[3], pos[3];
    AngleAxis((const float*)(parent + GE_ANGLES), f, r, u);
    const float* o = (const float*)(parent + GE_CURRENT_ORIGIN);
    const float* d = g_rideOffset[n];
    for (int i = 0; i < 3; ++i) pos[i] = o[i] + f[i] * d[0] + r[i] * d[1] + u[i] * d[2];
    memcpy(GClient(n) + PS_ORIGIN, pos, 12);
    memset(GClient(n) + PS_VELOCITY, 0, 12);
    memcpy(Entity(n) + GE_CURRENT_ORIGIN, pos, 12);
    memcpy(Entity(n) + 0x20, pos, 12);  // es.pos.trBase
}

uint8_t* LinkParent(int n)
{
    uint8_t* ti = *(uint8_t**)(Entity(n) + GE_TAGINFO);
    return ti ? *(uint8_t**)ti : nullptr;
}

void ClearLinkView(int n)  // lo que limpian playerlinktodelta y unlink en el gclient
{
    *(float*)(GClient(n) + GC_LINKCLAMP) = 0;
    GClient(n)[GC_LINKFLAG] = 0;
    *(uint32_t*)(GClient(n) + PS_PMFLAGS) &= ~PMF_LINKVIEW;
}

void Unlink(int n)
{
    uint8_t* ent = Entity(n);
    asm volatile("call *%1" : "+a"(ent) : "r"((uintptr_t)0x4d6720) : "ecx", "edx", "memory");
    ClearLinkView(n);
}

bool LinkTo(int n, uint8_t* parent)
{
    ClearLinkView(n);
    uint8_t* ent = Entity(n);
    int ok;
    asm volatile("push $0\n\tpush %2\n\tcall *%3\n\tadd $8, %%esp"
                 : "=a"(ok), "+b"(ent) : "r"(parent), "r"((uintptr_t)0x4d6540) : "ecx", "edx", "memory");
    if (!ok) return false;
    ((void(__cdecl*)(uint8_t*, int))0x4d6ca0)(Entity(n), 0);
    return true;
}

void HostRides()
{
    uint8_t* parent = LinkParent(0);
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2) {
            g_rideParent[n] = nullptr;
            continue;
        }
        uint8_t* mine = LinkParent(n);
        if (parent && mine != parent) {
            if (mine) Unlink(n);
            // sobre el vehiculo, atras del host y a un costado (segun hacia donde mira el vehiculo)
            const float* host = (const float*)(GClient(0) + PS_ORIGIN);
            float yaw = *(float*)(parent + GE_ANGLES + 4) * 3.14159265f / 180;
            float side = (n == 2 ? -1.0f : 1.0f) * (n == 3 ? 50.0f : 25.0f), back = -45;
            float spot[3] = {host[0] + back * cosf(yaw) - side * sinf(yaw), host[1] + back * sinf(yaw) + side * cosf(yaw),
                             host[2]};
            float f[3], r[3], u[3], rel[3];
            AngleAxis((const float*)(parent + GE_ANGLES), f, r, u);
            for (int i = 0; i < 3; ++i) rel[i] = spot[i] - ((const float*)(parent + GE_CURRENT_ORIGIN))[i];
            g_rideOffset[n][0] = rel[0] * f[0] + rel[1] * f[1] + rel[2] * f[2];
            g_rideOffset[n][1] = rel[0] * r[0] + rel[1] * r[1] + rel[2] * r[2];
            g_rideOffset[n][2] = rel[0] * u[0] + rel[1] * u[1] + rel[2] * u[2];
            memcpy(Entity(n) + GE_CURRENT_ORIGIN, spot, 12);
            bool ok = LinkTo(n, parent);
            Log("jugador %d: %s al vehiculo %d", n + 1, ok ? "enganchado" : "no se pudo enganchar",
                (int)(parent - (uint8_t*)G_ENTITIES) / GENTITY_SIZE);
            g_rideParent[n] = ok ? parent : nullptr;
            ApplyRide(n);
        } else if (!parent && mine && mine == g_rideParent[n]) {
            Unlink(n);
            g_rideParent[n] = nullptr;
            Log("jugador %d: se suelta del vehiculo", n + 1);
        } else if (mine && mine == g_rideParent[n]) {
            ApplyRide(n);
        }
    }
}

void HostCatchUp()
{
    const float* host = (const float*)(GClient(0) + PS_ORIGIN);
    const float* last = g_trailCount ? g_trail[(g_trailNext - 1 + TRAIL) % TRAIL] : nullptr;
    if (*(int*)(GClient(0) + PS_GROUNDENT) != 0x7ff && (!last || Dist2(last, host) > 32 * 32)) {
        memcpy(g_trail[g_trailNext], host, 12);
        g_trailNext = (g_trailNext + 1) % TRAIL;
        g_trailCount += g_trailCount < TRAIL;
    }
    DWORD now = GetTickCount();
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        float* o = (float*)(GClient(n) + PS_ORIGIN);
        // recien entrado (20 s): el script del inicio del nivel suele mover al host enseguida; se lo sigue de cerca
        bool fresh = now - g_joinedAt[n] < 20000;
        float dist = fresh ? 300 : CATCHUP_DIST;
        DWORD wait = fresh ? 1000 : CATCHUP_MS;
        if (!g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2 || Dist2(o, host) < dist * dist ||
            LinkParent(n)) {  // enganchado a un vehiculo: viaja con el
            g_farSince[n] = 0;
            continue;
        }
        if (!g_farSince[n]) g_farSince[n] = now;
        if (now - g_farSince[n] < wait) continue;
        // el mas nuevo cerca del host sin quedar encima; si no hay, en el lugar del host (los jugadores se atraviesan)
        const float* spot = host;
        for (int k = 1; k <= g_trailCount; ++k) {
            const float* p = g_trail[(g_trailNext - k + TRAIL) % TRAIL];
            float d2 = Dist2(p, host);
            if (d2 > 48 * 48 && d2 < 200 * 200) {
                spot = p;
                break;
            }
        }
        Log("host: jugador %d lejos del host, aparece en su rastro (%.0f %.0f %.0f)", n + 1, spot[0], spot[1], spot[2]);
        memcpy(o, spot, 12);
        memset(GClient(n) + PS_VELOCITY, 0, 12);
        memcpy(Entity(n) + GE_CURRENT_ORIGIN, spot, 12);
        g_farSince[n] = 0;
    }
}

// Un jugador de la partida cargada que no vuelve en 90 s se saca (si no, queda su cuerpo quieto en el mapa)
constexpr DWORD GHOST_MS = 90000;  // el que se une recarga el mapa para volver
constexpr uintptr_t G_FREE_ENTITY = 0x4d7c30;  // cdecl (gentity)
// Sentients (a quien apuntan los soldados): level.sentients (0x1c60518), 33 de 0x44 bytes, +0 entidad, +0x3e en uso;
// Sentient_Free 0x514d50 (cdecl) los suelta. G_FreeEntity suelta el de ent+0x104, pero tras cargar una partida y volver
// a entrar el remoto puede quedar con otro en uso apuntando a su entidad: un soldado que lo tenia de enemigo leia la
// entidad ya borrada y el host se caia (0x4e8b76, ent->client nulo).
constexpr uintptr_t LEVEL_SENTIENTS = 0x1c60518, SENTIENT_FREE = 0x514d50;
constexpr int GE_INUSE = 0xa4;  // r.inUse (como lo mira getent, 0x4fa0e0)
void FreeSentientsOf(int n)  // los que siguen en uso apuntando a la entidad n, que ya esta libre
{
    uint8_t* sentients = *(uint8_t**)LEVEL_SENTIENTS;
    for (int i = 0; i < 33; ++i) {
        uint8_t* s = sentients + i * 0x44;
        if (!s[0x3e] || *(uint8_t**)s != Entity(n)) continue;
        Log("host: el sentient %d seguia apuntando al jugador %d: se suelta", i, n + 1);
        ((void(__cdecl*)(uint8_t*))SENTIENT_FREE)(s);
        s[0x3e] = 0;
    }
}

void FreePlayerEntity(int n)
{
    ((void(__cdecl*)(uint8_t*))G_FREE_ENTITY)(Entity(n));
    *(int*)(GClient(n) + GC_CONNECTED) = 0;
    *(uint8_t**)(Client(n) + CL_GENTITY) = nullptr;
    FreeSentientsOf(n);
}

void HostDropGhosts()
{
    if (GetTickCount() - g_levelStart < GHOST_MS) return;
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2) continue;
        Log("host: el jugador %d de la partida cargada no volvio, se saca", n + 1);
        g_downed[n] = false;
        FreePlayerEntity(n);
    }
}

// el remoto se fue (coop2leave), dejo de responder o el host deja de crear partida
void DropRemote(int n, const char* why)
{
    Log("host: jugador %d fuera (%s)", n + 1, why);
    g_remote[n].active = false;
    g_downed[n] = false;
    g_rideParent[n] = nullptr;
    if (*(int*)(GClient(n) + GC_CONNECTED) == 2) FreePlayerEntity(n);
    char text[64];
    snprintf(text, sizeof text, "^3Player %d left", n + 1);
    Announce(false, text);
    if (g_downed[0] && !AnyRemote()) {  // el host caido se quedo solo: nadie lo puede levantar
        int max = *(int*)(Entity(0) + GE_MAXHEALTH);
        SetHealth(0, max > 1 ? max / 2 : 50);
        SetDowned(0, false);
    }
}

void HostTimeouts()
{
    DWORD now = GetTickCount();
    for (int n = 1; n < MAX_CLIENTS; ++n)
        if (g_remote[n].active && now - g_lastHeard[n] > REMOTE_TIMEOUT) DropRemote(n, "no responde");
}

void HostSendSnapshots()
{
    HostTimeouts();
    HostDropGhosts();
    HostRides();
    HostCatchUp();
    static DWORD lastLog;
    bool log = GetTickCount() - lastLog > 2000;
    if (log) lastLog = GetTickCount();
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2) continue;
        uint8_t* cl = Client(n);
        *(int*)(GClient(n) + PS_CLIENTNUM) = n;
        static uint8_t data[SNAP_MAX];
        Msg m{0, data, SNAP_MAX, 0, 0, 0};
        uint8_t* a = cl;
        asm volatile("call *%1" : "+a"(a) : "r"(SV_BUILD_SNAPSHOT) : "ecx", "edx", "memory");
        Msg* pm = &m;
        uint8_t *d = cl, *c = data;
        asm volatile("call *%3" : "+S"(pm), "+d"(d), "+c"(c) : "r"(SV_WRITE_SNAPSHOT) : "eax", "memory");
        if (m.overflowed || m.cursize >= SNAP_MAX) {
            Log("host: snapshot del jugador %d desbordado", n + 1);
            continue;
        }
        data[m.cursize++] = 4;  // svc_EOF
        Packet p(M_SNAP);
        int blockLen = g_blockSends > 0 ? BLOCK_SIZE : 0;
        p.Put(blockLen);
        if (blockLen) p.Put(g_block, BLOCK_SIZE);
        p.Put(m.cursize);
        p.Put(data, m.cursize);
        p.Send(g_remote[n].addr);
        HostSendEntities(n);
        HostSendDObjs(n);
        if (log) {
            const float* o = (const float*)(GClient(n) + 0x14);
            Log("host: jugador %d en %.0f %.0f %.0f, snapshot %d bytes", n + 1, o[0], o[1], o[2], m.cursize);
        }
    }
    if (g_blockSends > 0) --g_blockSends;
}

// ---------- joiner ----------
void JoinerSendJoin()
{
    Packet p(M_JOIN);
    const char* map = LevelMap();
    p.Put(map, (int)strlen(map) + 1);
    p.Send(g_host);
    g_lastJoinTry = GetTickCount();
}

void JoinerSnapshot(const uint8_t* p, int len)
{
    if (len < 4) return;
    int blockLen = *(const int*)p;
    p += 4, len -= 4;
    if (blockLen) {
        if (blockLen != BLOCK_SIZE || len < BLOCK_SIZE) return;
        int seq = *(const int*)(p + 4);
        if (seq != g_rxBlockSeq) {  // el host repite el bloque en 3 snapshots
            memcpy(g_rxBlock, p, BLOCK_SIZE);
            g_rxBlockSeq = seq;
            g_rxBlockNew = true;
        }
        p += BLOCK_SIZE, len -= BLOCK_SIZE;
    }
    if (len < 4) return;
    int snapLen = *(const int*)p;
    if (snapLen <= 0 || snapLen > SNAP_MAX || snapLen > len - 4) return;
    memcpy(g_rxSnap, p + 4, snapLen);
    g_rxSnapLen = snapLen;
    g_rxSnapNew = true;
}

void JoinerLost(const char* why);

void Poll()
{
    if (g_sock == INVALID_SOCKET) return;
    static uint8_t buf[SNAP_MAX + BLOCK_SIZE + 64];
    for (;;) {
        sockaddr_in from;
        int fl = sizeof from;
        int len = recvfrom(g_sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (len < 5) break;
        if (*(uint32_t*)buf != MAGIC) continue;
        uint8_t type = buf[4];
        const uint8_t* body = buf + 5;
        int bodyLen = len - 5;
        if (g_role == HOST) {
            int who = 0;
            for (int n = 1; n < MAX_CLIENTS && !who; ++n)
                if (g_remote[n].active && SameAddr(g_remote[n].addr, from)) who = n;
            if (who) g_lastHeard[who] = GetTickCount();
            if (type == M_JOIN) HostJoin(from, body, bodyLen);
            else if (type == M_CMD) HostUsercmd(from, body, bodyLen);
            else if (type == M_BYE && who) DropRemote(who, "se fue");
        } else if (g_role == JOINER && SameAddr(from, g_host)) {
            g_hostHeard = GetTickCount();
            if (type == M_BYE) {
                JoinerLost("The host stopped the co-op game.");
                break;
            }
            if (type == M_ACCEPT && bodyLen >= 4 && !g_joined) {
                g_joined = true;
                g_slot = *(const int*)body;
                // el nivel de aca queda congelado: sus DObj no se dibujan (se dibujan los del host) y sus arboles de
                // animacion ocupan la memoria de los scripts que necesitan los del host
                int freed = 0, trees = 0;
                for (int e = 0; e < MAX_ENTS; ++e) {
                    if (ClientDObj(e)) FreeClientDObj(e), ++freed;
                    uint8_t*& tree = *(uint8_t**)(Entity(e) + GE_TREE);  // queda en 0: G_FreeEntity no lo suelta de nuevo
                    if (Entity(e)[GE_INUSE] && tree) FreeTree(tree), tree = nullptr, ++trees;
                }
                Log("joiner: el host me acepto como jugador %d (nivel local: %d DObj y %d arboles liberados)", g_slot + 1,
                    freed, trees);
                Status("Connected to the host as player %d.", g_slot + 1);
            } else if (type == M_REJOIN && !g_mapLoadSince && bodyLen > 1 && !buf[len - 1] &&
                       (g_joined || _stricmp((const char*)body, LevelMap()))) {
                // El host cargo un punto de control o paso a otro nivel (su hora vuelve atras o empieza de nuevo y el
                // cliente de aca no acepta snapshots mas viejos que los que ya mostro), o este juego esta en otro mapa
                // o en el menu: se carga el mapa del host y, al terminar (G_InitGame), se vuelve a pedir entrar.
                g_joined = false;
                g_mapLoadSince = GetTickCount();  // si pidiera entrar ya, el host lo haria aparecer quieto durante la carga
                buf[len - 1] = 0;
                char cmd[96];
                snprintf(cmd, sizeof cmd, "map %.63s\n", (const char*)body);
                Log("joiner: el host cargo otra partida (%s), cargo el mapa y vuelvo a entrar", (const char*)body);
                Status("Loading the host's mission (%s)...", (const char*)body);
                Cbuf_AddText(cmd);
            } else if (type == M_FULL) {
                Status("The game is full (4 players).");
                g_role = NONE;
            } else if (type == M_SNAP && g_joined) {
                JoinerSnapshot(body, bodyLen);
            } else if (type == M_DOBJ && g_joined) {
                JoinerApplyDObjs(body, bodyLen);
            } else if (type == M_ANIM && g_joined) {
                JoinerApplyAnims(body, bodyLen);
            } else if (type == M_ENTS && g_joined) {
                JoinerApplyEntities(body, bodyLen);
            }
        }
    }
}

// el que se une deja la partida: vuelve al menu si estaba en un nivel del host
void JoinerLost(const char* why)
{
    Log("joiner: fuera (%s)", why);
    bool inLevel = g_joined;
    g_role = NONE;
    g_joined = false;
    g_mapLoadSince = 0;
    Status("%s", why);
    if (inLevel) Cbuf_AddText("disconnect\n");
}

// Los comandos del host llevan la numeracion de su cliente local: bloque +4 el ultimo, +8 el anterior al bloque. El
// cliente de aca guarda el ultimo recibido (0x61569c) y el cgame el ultimo ejecutado (0xf5bd14), en un anillo de 256.
// Si no siguen a lo que llego antes (recien entra, el host volvio a un punto de control, se perdio un bloque), el cgame
// pedia numeros que nunca llegaron: "CL_GetServerCommand: a reliable command was cycled out" y vuelta al menu. Se
// sigue desde el anterior al bloque.
constexpr uintptr_t CL_CMD_SEQ = 0x61569c, CG_CMD_SEQ = 0xf5bd14;
void DeliverCommands(uint8_t* block)
{
    int seq = *(int*)(block + 4), prev = *(int*)(block + 8);
    int &clSeq = *(int*)CL_CMD_SEQ, &cgSeq = *(int*)CG_CMD_SEQ;
    if (clSeq < prev || clSeq > seq || cgSeq > seq || cgSeq <= seq - 256) {
        Log("joiner: comandos del host desde %d (aca iba en %d/%d)", prev + 1, clSeq, cgSeq);
        clSeq = cgSeq = prev;
    }
    uintptr_t f = CL_DELIVER_COMMANDS;
    asm volatile("call *%%eax" : "+b"(block), "+a"(f) : : "ecx", "edx", "esi", "edi", "memory");
}

void ParseSnapshot()
{
    // La hora del cliente (y la de sus usercmds) sale del reloj del servidor local (svs.time, 0x82243c). Con el
    // juego local congelado se lo lleva a la hora del host, que viene en el snapshot (svc_snapshot, int hora): si no,
    // el host descarta los usercmds por viejos.
    if (g_rxSnapLen > 5 && g_rxSnap[0] == 2) {
        int hostTime = *(const int*)(g_rxSnap + 1);
        int& svsTime = *(int*)0x82243c;
        if (hostTime - svsTime > 100 || svsTime - hostTime > 100) {
            Log("joiner: reloj local %d -> %d (hora del host)", svsTime, hostTime);
            svsTime = hostTime;
        }
    }
    Msg m{0, g_rxSnap, SNAP_MAX, g_rxSnapLen, 0, 0};
    Msg* pm = &m;
    int seq = (*(int*)(Client(0) + CL_MESSAGES))++;
    asm volatile("call *%2" : "+S"(pm), "+a"(seq) : "r"(CL_PARSE_MESSAGE) : "ecx", "edx", "edi", "memory");
}

// ---------- comandos ----------
void Cmd_Host()
{
    if (g_role == JOINER) return;
    if (!OpenSocket(PORT)) {
        Com_Printf("^1COOP: could not open UDP port %d\n", PORT);
        return;
    }
    g_role = HOST;
    Log("host: esperando jugadores en el puerto %d", PORT);
    Status("Hosting on UDP port %d. Start or load a mission; the others join with your IP.", PORT);
}

// coop2leave: el que se une se va de la partida; el host deja de crear partida (los demas vuelven al menu)
void Cmd_Leave()
{
    if (g_role == JOINER) {
        Packet(M_BYE).Send(g_host);
        JoinerLost("Disconnected.");
    } else if (g_role == HOST) {
        for (int n = 1; n < MAX_CLIENTS; ++n) {
            if (!g_remote[n].active) continue;
            Packet(M_BYE).Send(g_remote[n].addr);
            DropRemote(n, "el host dejo de crear partida");
        }
        g_role = NONE;
        Log("host: deja de crear partida");
        Status("Stopped hosting. The game goes on single player.");
    }
}

void Cmd_Join()
{
    // coop2join <IP o nombre> (cmd_argc 0x6c0280, cmd_argv 0x6bb680, como los lee writeconfig 0x42ea40); sin
    // argumento, la del campo de la ventana Co-op (dvar coop2_ip)
    char ip[64] = "";
    if (*(int*)0x6c0280 >= 2) {
        sscanf(((const char**)0x6bb680)[1], "%63s", ip);
        char cmd[96];  // el campo de la ventana Co-op la recuerda
        snprintf(cmd, sizeof cmd, "seta coop2_ip \"%s\"\n", ip);
        if (!strpbrk(ip, "\";")) Cbuf_AddText(cmd);
    } else if (uint8_t* d = Dvar_FindVar("coop2_ip")) {
        const char* v = *(const char**)(d + 8);
        if (v) sscanf(v, "%63s", ip);
    }
    if (!ip[0]) {
        Status("Type the host's IP first.");
        return;
    }
    if (!OpenSocket(0)) return;
    g_host = {};
    g_host.sin_family = AF_INET;
    g_host.sin_port = htons(PORT);
    g_host.sin_addr.s_addr = inet_addr(ip);
    if (g_host.sin_addr.s_addr == INADDR_NONE) {
        hostent* h = gethostbyname(ip);
        if (!h) {
            Status("Could not find %s.", ip);
            return;
        }
        memcpy(&g_host.sin_addr, h->h_addr_list[0], 4);
    }
    g_role = JOINER;
    g_joined = false;
    g_hostHeard = GetTickCount();
    Log("joiner: uniendome a %s:%d", ip, PORT);
    Status("Joining %s...", ip);
    JoinerSendJoin();
}
}  // namespace

// ---------- ganchos ----------
// El cliente local llama a ClientThink(0) directo (0x409f76) con el usercmd ya copiado en svs.clients[0]: el que se
// une no lo aplica en su juego (congelado), se lo manda al host.
extern "C" void __cdecl Cod2_LocalThink()
{
    if (g_role == JOINER && g_joined) {
        // A cientos de FPS cada usercmd mueve ~1 ms y el Pmove redondea la posicion en cada paso: el jugador casi no
        // avanza en el host. Se manda el ultimo cada 15 ms (o antes si cambian los botones).
        const uint8_t* cmd = Client(0) + CL_LASTUSERCMD;
        static int lastTime, lastButtons;
        int time = *(const int*)cmd, buttons = *(const int*)(cmd + 4);
        if (time - lastTime < 15 && time >= lastTime && buttons == lastButtons) return;
        lastTime = time, lastButtons = buttons;
        Packet p(M_CMD);
        p.Put(cmd, USERCMD_SIZE);
        p.Send(g_host);
        static DWORD lastLog;
        static int count;
        ++count;
        if (GetTickCount() - lastLog > 2000) {
            lastLog = GetTickCount();
            Log("joiner: %d usercmds mandados (hora %d)", count, *(int*)(Client(0) + CL_LASTUSERCMD));
            count = 0;
        }
        return;
    }
    if (g_role == HOST && g_downed[0]) DownedCmd(Client(0) + CL_LASTUSERCMD);
    ClientThink(0);
}

// Bloque: +0 largo del texto, +4 ultimo comando, +8 el anterior al bloque, +0xc texto, +0x200c posicion de cada
// comando en el texto (anillo de 256). Si el bloque anterior todavia se esta mandando, los comandos nuevos se le
// agregan: si no, el que se une se perdia los de ese frame.
bool AppendCommands(const uint8_t* block)
{
    int prev = *(const int*)(block + 8), seq = *(const int*)(block + 4);
    if (prev != *(int*)(g_block + 4)) return false;
    int& len = *(int*)g_block;
    const int* from = (const int*)(block + BLOCK_OFFSETS);
    int* to = (int*)(g_block + BLOCK_OFFSETS);
    for (int i = prev + 1; i <= seq; ++i) {
        int at = from[i & 0xff], room = BLOCK_OFFSETS - BLOCK_TEXT;
        if (at < 0 || at >= room) return false;
        const char* text = (const char*)block + BLOCK_TEXT + at;
        int n = (int)strnlen(text, room - at) + 1;
        if (len + n > room) return false;
        memcpy(g_block + BLOCK_TEXT + len, text, n);
        to[i & 0xff] = len;
        len += n;
    }
    *(int*)(g_block + 4) = seq;
    return true;
}

// SV_FinishFrame, antes de pasarle al cliente local los comandos del servidor (EBX = bloque). Devuelve el bloque a
// pasar o 0 para no pasar nada.
extern "C" uint8_t* __cdecl Cod2_Reliable(uint8_t* block)
{
    if (g_role == HOST && *(int*)(block + 4) != *(int*)(block + 8)) {  // comandos nuevos este frame
        if (g_blockSends <= 0 || !AppendCommands(block)) memcpy(g_block, block, BLOCK_SIZE);
        g_blockSends = 3;
    }
    if (g_role == JOINER && g_joined) return nullptr;  // los del juego local no; los del host en Cod2_Snapshot
    return block;
}

// pruebas: un comando de consola escrito en cod2coop.cmd (cod2coop_join.cmd en el que se une) se ejecuta y el
// archivo se borra (tools/run2.ps1 cmd host|join "<comando>")
char g_cmdFile[MAX_PATH];
void PollCommandFile()
{
    FILE* f = g_cmdFile[0] ? fopen(g_cmdFile, "rb") : nullptr;
    if (!f) return;
    char text[1024];
    size_t n = fread(text, 1, sizeof text - 2, f);
    fclose(f);
    DeleteFileA(g_cmdFile);
    text[n] = '\n';
    text[n + 1] = 0;
    Log("cmd: %s", text);
    Cbuf_AddText(text);
}

// SV_FinishFrame, en lugar del snapshot del cliente local. Devuelve 1 para mandar el snapshot local.
extern "C" int __cdecl Cod2_Snapshot()
{
    PollCommandFile();
    g_lastLevelFrame = GetTickCount();
    Poll();
    if (g_role == HOST) {
        static DWORD last;
        DWORD now = GetTickCount();
        float dt = last ? (now - last) / 1000.0f : 0.05f;
        last = now;
        HostDowned(dt > 0.2f ? 0.2f : dt);
        HostBodies(dt > 0.2f ? 0.2f : dt);
        PlayersAsBodies(true);  // Cod2_AfterSnapshot lo deshace despues del snapshot local
        HostSendSnapshots();
        return 1;
    }
    if (g_role == JOINER) {
        if (!g_joined) {
            if (g_mapLoadSince && GetTickCount() - g_mapLoadSince > 120000) g_mapLoadSince = 0;  // el mapa no cargo
            if (!g_mapLoadSince && GetTickCount() - g_lastJoinTry > 1000) JoinerSendJoin();
            return 1;
        }
        if (GetTickCount() - g_hostHeard > HOST_TIMEOUT) {
            JoinerLost("Lost the connection to the host.");
            return 1;
        }
        if (g_rxBlockNew) {
            g_rxBlockNew = false;
            DeliverCommands(g_rxBlock);
        }
        if (g_rxSnapNew) {
            g_rxSnapNew = false;
            ParseSnapshot();
        }
        return 0;
    }
    return 1;
}

// DObj_Free (0x42f760, ECX = entidad): el juego borra el DObj de los jugadores (no tienen modelo) en cada frame y el
// cliente nunca llega a copiar el cuerpo. Devuelve 1 para no borrarlo. G_FreeEntity (fin del nivel, cargar partida)
// si lo borra.
constexpr uintptr_t G_FREE_ENTITY_FREE = 0x4d7d41;
extern "C" int __cdecl Cod2_KeepDObj(int e, uintptr_t caller)
{
    if (g_ourFree || e < 0 || e >= MAX_CLIENTS || !g_bodyTree[e] || caller == G_FREE_ENTITY_FREE) return 0;
    uint8_t* d = ClientDObj(e);
    if (!d || *(uint8_t**)d != g_bodyTree[e]) return 0;
    static uintptr_t seen[8];
    for (auto& s : seen) {
        if (s == caller) break;
        if (!s) {
            s = caller;
            Log("cuerpo %d: el juego lo quiere borrar desde %08x", e + 1, (unsigned)caller);
            break;
        }
    }
    return 1;
}

void RegisterCommands()
{
    static bool done;
    if (done) return;
    done = true;
    Cmd_AddCommand("coop2host", Cmd_Host);
    Cmd_AddCommand("coop2join", Cmd_Join);
    Cmd_AddCommand("coop2leave", Cmd_Leave);
    Cmd_AddCommand("coop2hurt", Cmd_Hurt);
    Cmd_AddCommand("coop2save", Cmd_Save);
    if (!Dvar_FindVar("coop2_ip")) Cbuf_AddText("seta coop2_ip \"\"\n");
}

// Com_Frame (0x42ed20), en cada frame del juego: sin nivel corriendo (menu, carga) Cod2_Snapshot no se llama; el que
// se une igual pide entrar y recibe el mapa a cargar. El host solo atiende dentro del nivel.
extern "C" void __cdecl Cod2_ComFrame()
{
    RegisterCommands();  // coop2join tambien desde el menu principal
    if (GetTickCount() - g_lastLevelFrame < 500) return;  // en un nivel atiende Cod2_Snapshot
    PollCommandFile();
    static DWORD lastPing;
    bool ping = GetTickCount() - lastPing > 1000;
    if (ping) lastPing = GetTickCount();
    if (g_role == HOST) {  // en pausa o cargando: el host no atiende, pero avisa que sigue ahi
        if (ping)
            for (int n = 1; n < MAX_CLIENTS; ++n)
                if (g_remote[n].active) Packet(M_PING).Send(g_remote[n].addr);
        return;
    }
    if (g_role != JOINER) return;
    Poll();
    if (g_role != JOINER) return;
    if (g_joined && ping) Packet(M_PING).Send(g_host);  // esperando "clic para empezar"
    if (g_joined && GetTickCount() - g_hostHeard > HOST_TIMEOUT) {
        JoinerLost("Lost the connection to the host.");
        return;
    }
    if (g_mapLoadSince && GetTickCount() - g_mapLoadSince > 120000) g_mapLoadSince = 0;
    if (!g_joined && !g_mapLoadSince && GetTickCount() - g_lastJoinTry > 1000) JoinerSendJoin();
}

extern "C" void __cdecl Cod2_AfterSnapshot()
{
    if (g_role == HOST) PlayersAsBodies(false);
}

// el que se une no corre su juego local
int(__cdecl* g_runFrame)(int, int);
extern "C" int __cdecl Cod2_RunFrame(int partial, int limit)
{
    if (g_role == JOINER && g_joined) {
        // el frame del juego es el que toma los usercmds del cliente local (0x409ed0 -> ClientThink(0), que aca
        // termina en Cod2_LocalThink): sin frame hay que pedirlos igual
        ((void(__cdecl*)())0x409ed0)();
        return 1;
    }
    int r = g_runFrame(partial, limit);
    for (int n = 1; n < MAX_CLIENTS; ++n)
        if (!Entity(n)[GE_INUSE]) FreeSentientsOf(n);  // lugar de jugador libre (tambien tras cargar una partida)
    static uint8_t* lastParent;  // para el log: a que esta enganchado el host (scripts de vehiculos y cinematicas)
    uint8_t* parent = LinkParent(0);
    if (parent != lastParent) {
        if (parent) Log("host: enganchado a la entidad %d", (int)(parent - (uint8_t*)G_ENTITIES) / GENTITY_SIZE);
        else Log("host: suelto");
        lastParent = parent;
    }
    return r;
}

asm(R"(
.intel_syntax noprefix
.globl _Cod2_LocalThinkStub
_Cod2_LocalThinkStub:
    pushad
    call _Cod2_LocalThink
    popad
    ret
.globl _Cod2_ReliableStub
_Cod2_ReliableStub:
    push ebx
    pushad
    push ebx
    call _Cod2_Reliable
    add esp, 4
    mov dword ptr [esp+28], eax
    popad
    test eax, eax
    jz 1f
    mov ebx, eax
    mov eax, 0x40f580
    call eax
1:  pop ebx
    ret
.globl _Cod2_SnapshotStub
_Cod2_SnapshotStub:
    pushad
    call _Cod2_Snapshot
    test eax, eax
    popad
    jz 2f
    mov eax, 0x44b180
    call eax
2:  ret
.globl _Cod2_GetEntMatch
_Cod2_GetEntMatch:
    movzx edx, ax
    cmp edx, ebp
    jne 4f
    cmp edi, 1
    jb 5f
    cmp edi, 4
    jb 4f
5:  push 0x4fa0fd
    ret
4:  push 0x4fa11b
    ret
.globl _Cod2_NoDObjBounds
_Cod2_NoDObjBounds:
    xor eax, eax
    mov dword ptr [edx], eax
    mov dword ptr [edx+4], eax
    mov dword ptr [edx+8], eax
    mov dword ptr [ecx], eax
    mov dword ptr [ecx+4], eax
    mov dword ptr [ecx+8], eax
    ret
.globl _Cod2_PrecacheEndStub
_Cod2_PrecacheEndStub:
    pushad
    call _Cod2_PrecacheEnd
    popad
    mov dword ptr [0x9ff014], esi
    mov dword ptr [0x1c60528], esi
    ret
.globl _Cod2_ShutdownGameStub
_Cod2_ShutdownGameStub:
    pushad
    call _Cod2_ShutdownGame
    popad
    push 0x4d9ce0
    ret
.globl _Cod2_CgameSnapStub
_Cod2_CgameSnapStub:
    push dword ptr [esp+8]
    push dword ptr [esp+8]
    mov eax, 0x4a4330
    call eax
    add esp, 8
    pushad
    call _Cod2_AfterSnapshot
    popad
    ret
.globl _Cod2_ComFrameStub
_Cod2_ComFrameStub:
    pushad
    call _Cod2_ComFrame
    popad
    push 0x44f0a0
    ret
.globl _Cod2_FreeDObjStub
_Cod2_FreeDObjStub:
    pushad
    push dword ptr [esp+32]
    push ecx
    call _Cod2_KeepDObj
    add esp, 8
    test eax, eax
    popad
    jnz 3f
    push edi
    movsx edi, word ptr [ecx*2 + 0x74a980]
    push 0x42f769
    ret
3:  ret
.att_syntax
)");
extern "C" void Cod2_FreeDObjStub();
extern "C" void Cod2_ComFrameStub();
extern "C" void Cod2_LocalThinkStub();
extern "C" void Cod2_ReliableStub();
extern "C" void Cod2_SnapshotStub();
extern "C" void Cod2_CgameSnapStub();
extern "C" void Cod2_PrecacheEndStub();
extern "C" void Cod2_ShutdownGameStub();
extern "C" void Cod2_NoDObjBounds();
extern "C" void Cod2_GetEntMatch();

// Antes de G_ShutdownGame (cambio de nivel, punto de control, error, salir): la memoria de los scripts sigue entre
// niveles, asi que los arboles armados aca (cuerpos en el host, DObj copiados del host en el que se une) se sueltan
// mientras sus XAnim siguen validos. Sin esto se perdian en cada nivel y en el segundo mapa el que se une se quedaba
// sin memoria ("failed memory allocation for script usage").
extern "C" void __cdecl Cod2_ShutdownGame()
{
    int freed = 0;
    for (int e = 0; e < MAX_ENTS; ++e)
        if (g_ownTree[e]) FreeClientDObj(e), ++freed;
    for (auto& t : g_bodyTree) t = nullptr;
    if (freed) Log("fin del nivel: %d arboles propios liberados", freed);
}

// Llamado desde G_InitGame: level.clients pasa a un arreglo de 4 y las entidades 0..3 quedan para jugadores.
extern "C" void __cdecl Cod2_SetupClients()
{
    memset(g_clients, 0, sizeof g_clients);
    *(uint8_t**)LEVEL_CLIENTS = g_clients;
    *(uint8_t**)SV_GAMECLIENTS = g_clients;
    *(int*)LEVEL_MAXCLIENTS = MAX_CLIENTS;
    for (int n = 0; n < MAX_CLIENTS; ++n) *(uint8_t**)(Entity(n) + GE_CLIENT) = GClient(n);
    *(int*)LEVEL_NUM_ENTITIES = MAX_CLIENTS;
    *(int*)SV_GENTITIES_NUM = MAX_CLIENTS;
    // mapa nuevo: los remotos vuelven a entrar. Se les avisa ya: hasta el "clic para empezar" el host no atiende la red
    // y, sin noticias en 20 s, el que se une se daria por desconectado.
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active) continue;
        if (g_role == HOST) SendRejoin(g_remote[n].addr, true);
        Log("host: jugador %d, que cargue %s", n + 1, MapName());
    }
    for (auto& r : g_remote) r.active = false;
    for (int n = 0; n < MAX_CLIENTS; ++n) g_bodyTree[n] = nullptr, g_bodyAnim[n] = -1;  // las tablas de DObj se vacian
    memset(g_ownTree, 0, sizeof g_ownTree);
    memset(g_downed, 0, sizeof g_downed);
    memset(g_rideParent, 0, sizeof g_rideParent);
    g_levelStart = GetTickCount();
    g_joined = false;
    g_mapLoadSince = 0;
    RegisterCommands();
    Log("G_InitGame: %d jugadores, level.clients en %p", MAX_CLIENTS, (void*)g_clients);
}

asm(R"(
.intel_syntax noprefix
.globl _Cod2_InitClientsStub
_Cod2_InitClientsStub:
    pushad
    call _Cod2_SetupClients
    popad
    ret
.att_syntax
)");
extern "C" void Cod2_InitClientsStub();

namespace {
bool Expect(uintptr_t addr, const char* bytes, size_t n)
{
    if (!memcmp((void*)addr, bytes, n)) return true;
    Log("ERROR: bytes inesperados en %08x", (unsigned)addr);
    return false;
}

void CallTo(uintptr_t at, void* target, int len)  // call rel32 + nops
{
    uint8_t patch[16] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)target - (at + 5));
    hook::Write(at, patch, len);
}

void Install()
{
    if (!Expect(0x448eca, "\x68\x08\xaa\x00\x00", 5) || !Expect(0x448edd, "\x68\x08\xaa\x00\x00", 5) ||
        !Expect(INIT_CLIENTS_AT, "\x89\x2d\x2c\x24\x82\x00", 6) || !Expect(LOCAL_THINK_CALL, "\xe8\x65\xdf\x0d\x00", 5) ||
        !Expect(RELIABLE_CALL, "\xe8\x41\x4c\xfc\xff", 5) || !Expect(SNAPSHOT_CALL, "\xe8\x2a\x08\x00\x00", 5) ||
        !Expect(G_RUN_FRAME, "\xa1\x5c\x10\xc6\x01", 5) ||
        !Expect(CGAME_SNAP_CALL, "\xe8\xbd\x99\x05\x00", 5) ||
        !Expect(CL_DOBJ_FREE, "\x57\x0f\xbf\x3c\x4d\x80\xa9\x74\x00", 9))
        return;
    // guardado: puntero a cliente como indice+1 (WriteField1 0x4e0d3d, ReadField 0x4e1125) y numero de cliente
    // (G_LoadGame 0x4e4f7b); los tres comparan con 1
    struct { uintptr_t at; uint8_t max; } limits[] = {{0x4e0d3d, MAX_CLIENTS}, {0x4e1125, MAX_CLIENTS}, {0x4e4f7b, MAX_CLIENTS - 1}};
    for (auto& l : limits)
        if (Expect(l.at, l.at == 0x4e1125 ? "\x83\xfe\x01" : "\x83\xff\x01", 3)) hook::Write(l.at + 2, &l.max, 1);
    // G_FreeEntity devuelve a la lista libre toda entidad que no sea la 0 y G_Spawn la reusa: una entidad comun (un
    // avion, una granada) en el lugar 1..3 pasa por ClientEndFrame (G_RunFrame lo llama para las entidades en uso
    // menores que level.maxclients) y el juego se cae (0x4e804a). Las de los jugadores no vuelven a la lista.
    const uint8_t maxClients = MAX_CLIENTS;
    if (Expect(0x4d7ed9, "\x83\xf9\x01", 3)) hook::Write(0x4d7ed9 + 2, &maxClients, 1);
    // G_LoadGame arma la lista libre de nuevo desde la entidad 1 (mov edi, 1 / mov eax, &g_entities[1]): desde la 4
    if (Expect(0x4e4e99, "\xbf\x01\x00\x00\x00", 5) && Expect(0x4e4eae, "\xb8\xfc\x60\xb1\x01", 5)) {
        uint32_t first = MAX_CLIENTS, ent = (uint32_t)(uintptr_t)Entity(MAX_CLIENTS);
        hook::Write(0x4e4e99 + 1, &first, 4);
        hook::Write(0x4e4eae + 1, &ent, 4);
    }
    // G_LoadGame corta ("client mis-match in savegame") si la partida tiene un jugador que no esta conectado: en vez
    // del error se lo marca conectado (mov dword [esi+0x85c0], 2) y se carga; el remoto lo retoma al volver a entrar
    if (Expect(0x4e4faa, "\x68\x54\x39\x58\x00\x6a\x01\xe8\x3a\x78\xf4\xff\x83\xc4\x08", 15))
        hook::Write(0x4e4faa, "\xc7\x86\xc0\x85\x00\x00\x02\x00\x00\x00\x90\x90\x90\x90\x90", 15);
    // SV_Startup: svs.clients con lugar para 4 (malloc y memset)
    uint32_t size = MAX_CLIENTS * CLIENT_SIZE;
    hook::Write(0x448eca + 1, &size, 4);
    hook::Write(0x448edd + 1, &size, 4);
    CallTo(INIT_CLIENTS_AT, (void*)Cod2_InitClientsStub, 6);  // Cod2_SetupClients pone sv.num_entities
    CallTo(LOCAL_THINK_CALL, (void*)Cod2_LocalThinkStub, 5);
    CallTo(RELIABLE_CALL, (void*)Cod2_ReliableStub, 5);
    CallTo(SNAPSHOT_CALL, (void*)Cod2_SnapshotStub, 5);
    CallTo(CGAME_SNAP_CALL, (void*)Cod2_CgameSnapStub, 5);
    if (Expect(0x42edea, "\xe8\xb1\x02\x02\x00", 5)) CallTo(0x42edea, (void*)Cod2_ComFrameStub, 5);  // Com_Frame
    if (Expect(PRECACHE_END, "\x89\x35\x14\xf0\x9f\x00\x89\x35\x28\x05\xc6\x01", 12))
        CallTo(PRECACHE_END, (void*)Cod2_PrecacheEndStub, 12);  // fin de la precarga del nivel
    if (Expect(SHUTDOWN_GAME_CALL, "\xe8\x2a\x13\x09\x00", 5))
        CallTo(SHUTDOWN_GAME_CALL, (void*)Cod2_ShutdownGameStub, 5);  // SV_ShutdownGameProgs -> G_ShutdownGame
    // En el que se une la tabla de DObj es la del host, pero los trazos del movimiento local (ClientThink) cruzan las
    // entidades de su propio nivel congelado: los limites del DObj de una entidad (0x4481c0, EAX = entidad, EDX/ECX =
    // min/max) leen sin mirar si hay DObj y el juego se caia (0x4481e1) cuando el host ya no tenia esa entidad. Sin DObj:
    // limites vacios (je al relleno de int3 en 0x448205, de ahi jmp a Cod2_NoDObjBounds).
    if (Expect(0x4481cf, "\x74\x0d", 2) && Expect(0x448205, "\xcc\xcc\xcc\xcc\xcc", 5)) {
        uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)Cod2_NoDObjBounds - (0x448205 + 5));
        hook::Write(0x448205, jmp, 5);
        hook::Write(0x4481cf, "\x74\x34", 2);
    }
    // getent (0x4fa090) corta el hilo del script si encuentra dos entidades: animscripts\init.gsc hace
    // level.player = getent("player", "classname") con el primer soldado del nivel, y si un remoto ya entro (se une en
    // "clic para empezar") level.player queda sin definir y el nivel se rompe (en duhoc_assault no se baja del bote).
    // Los remotos (entidades 1..3, MAX_CLIENTS = 4 en Cod2_GetEntMatch) no cuentan para getent: el script ve un jugador.
    if (Expect(0x4fa0f6, "\x0f\xb7\xd0\x3b\xd5\x75\x1e", 7)) {
        uint8_t jmp[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)Cod2_GetEntMatch - (0x4fa0f6 + 5));
        hook::Write(0x4fa0f6, jmp, 7);
    }
    g_runFrame = (int(__cdecl*)(int, int))hook::Inline(G_RUN_FRAME, (void*)Cod2_RunFrame, 5);
    if (Expect(G_DAMAGE, "\x83\xec\x20\x53\x55", 5)) g_gDamage = (GDamageFn)hook::Inline(G_DAMAGE, (void*)Cod2_GDamage, 5);
    uint8_t jmp[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};  // DObj_Free -> Cod2_FreeDObjStub
    *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)Cod2_FreeDObjStub - (CL_DOBJ_FREE + 5));
    hook::Write(CL_DOBJ_FREE, jmp, 9);
    Log("parches puestos (svs.clients x%d, G_InitGame, red)", MAX_CLIENTS);
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH];
        GetModuleFileNameA(self, p, MAX_PATH);
        strcpy(strrchr(p, '\\') + 1, strstr(GetCommandLineA(), "coop2join") ? "cod2coop_join.log" : "cod2coop.log");
        g_log = fopen(p, "w");
        strcpy(g_cmdFile, p);
        strcpy(strrchr(g_cmdFile, '.'), ".cmd");
        Install();
    }
    return TRUE;
}
