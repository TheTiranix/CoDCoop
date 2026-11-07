// CoD2 (CoD2SP_s.exe 1.3), en prueba. El servidor de un jugador reserva un solo cliente: esto lo amplia a 4 y
// agrega una red UDP propia. Lo carga ddraw.dll (loader.cpp) desde la carpeta del juego.
//   coop2host            el que tiene la partida (puerto UDP 28962)
//   coop2join            se une a la IP que sigue a "+coop2join" en la linea de comandos (mismo mapa cargado)
// El cliente de CoD2 no recibe paquetes: el servidor le arma el snapshot como mensaje (SV_SendClientSnapshot) y se lo
// pasa directo al parser (CL_ParseServerMessage). El host arma ese mismo mensaje para cada remoto y lo manda por UDP;
// el que se une congela su juego local y le pasa a su parser lo que llega.
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr uintptr_t SV_WRITE_SNAPSHOT = 0x44af00;  // ESI = msg, EDX = client_t
constexpr uintptr_t CL_PARSE_MESSAGE = 0x40cf00;   // ESI = msg, EAX = numero de mensaje
constexpr uintptr_t G_RUN_FRAME = 0x4da970;        // int (int parcial, int limite); 1 = frame terminado

auto Com_Printf = (void(__cdecl*)(const char*, ...))0x42c130;
auto Cmd_AddCommand = (void(__cdecl*)(const char*, void (*)()))0x41bb00;
auto ClientConnect = (int(__cdecl*)(int))0x4e8f40;

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
enum : uint8_t { M_JOIN = 'J', M_ACCEPT = 'A', M_FULL = 'F', M_CMD = 'C', M_SNAP = 'S' };
enum Role { NONE, HOST, JOINER } g_role;
SOCKET g_sock = INVALID_SOCKET;

struct Remote {
    bool active;
    sockaddr_in addr;
} g_remote[MAX_CLIENTS];
DWORD g_joinedAt[MAX_CLIENTS];  // host: cuando entro cada remoto

sockaddr_in g_host;
bool g_joined;
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

// ---------- host ----------
void HostJoin(const sockaddr_in& from)
{
    int n = 0;
    for (int i = 1; i < MAX_CLIENTS && !n; ++i)
        if (g_remote[i].active && SameAddr(g_remote[i].addr, from)) n = i;
    if (!n) {
        for (int i = 1; i < MAX_CLIENTS && !n; ++i)
            if (!g_remote[i].active && !*(int*)(GClient(i) + GC_CONNECTED)) n = i;
        if (!n) {
            Packet(M_FULL).Send(from);
            return;
        }
        g_remote[n] = {true, from};
        g_joinedAt[n] = GetTickCount();
        *(uint8_t**)(Client(n) + CL_GENTITY) = Entity(n);
        // ClientConnect empieza con la init de caminos de la IA (call 0x516bc0), pensada para el inicio del nivel: a
        // mitad de partida deja una lista circular y el juego se cuelga (igual que en CoD1). Se saltea.
        uint8_t saved[5];
        memcpy(saved, (void*)PATH_INIT_CALL, 5);
        bool skip = !memcmp(saved, "\xe8\x73\xdc\x02\x00", 5);
        if (skip) hook::Write(PATH_INIT_CALL, "\x0F\x1F\x44\x00\x00", 5);
        ClientConnect(n);
        if (skip) hook::Write(PATH_INIT_CALL, saved, 5);
        ClientBegin(n);
        // aparece donde esta el host (el punto de inicio del mapa puede quedar lejos o fuera de la zona de juego)
        float* o = (float*)(GClient(n) + 0x14);
        memcpy(o, GClient(0) + 0x14, 12);
        memset(GClient(n) + 0x20, 0, 12);  // velocidad
        Log("host: %s:%d entra como jugador %d en %.0f %.0f %.0f", inet_ntoa(from.sin_addr), ntohs(from.sin_port), n + 1,
            o[0], o[1], o[2]);
        Com_Printf("^2COOP: a player joined (player %d)\n", n + 1);
    }
    Packet p(M_ACCEPT);
    p.Put<int>(n);
    p.Send(from);
}

void HostUsercmd(const sockaddr_in& from, const uint8_t* cmd, int len)
{
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active || !SameAddr(g_remote[n].addr, from)) continue;
        if (len < USERCMD_SIZE || *(int*)(GClient(n) + GC_CONNECTED) != 2) return;
        memcpy(Client(n) + CL_LASTUSERCMD, cmd, USERCMD_SIZE);
        ClientThink(n);
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
constexpr int TRAIL = 40, PS_ORIGIN = 0x14, PS_VELOCITY = 0x20, PS_GROUNDENT = 0x60, GE_CURRENT_ORIGIN = 0xdc;
float g_trail[TRAIL][3];
int g_trailCount, g_trailNext;
DWORD g_farSince[MAX_CLIENTS];

float Dist2(const float* a, const float* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
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
        if (!g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2 || Dist2(o, host) < dist * dist) {
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

void HostSendSnapshots()
{
    HostCatchUp();
    static DWORD lastLog;
    bool log = GetTickCount() - lastLog > 2000;
    if (log) lastLog = GetTickCount();
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        if (!g_remote[n].active || *(int*)(GClient(n) + GC_CONNECTED) != 2) continue;
        uint8_t* cl = Client(n);
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
    Packet(M_JOIN).Send(g_host);
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
            if (type == M_JOIN) HostJoin(from);
            else if (type == M_CMD) HostUsercmd(from, body, bodyLen);
        } else if (g_role == JOINER && SameAddr(from, g_host)) {
            if (type == M_ACCEPT && bodyLen >= 4 && !g_joined) {
                g_joined = true;
                g_slot = *(const int*)body;
                Log("joiner: el host me acepto como jugador %d", g_slot + 1);
                Com_Printf("^2COOP: connected to the host (player %d)\n", g_slot + 1);
            } else if (type == M_FULL) {
                Com_Printf("^1COOP: the game is full\n");
                g_role = NONE;
            } else if (type == M_SNAP && g_joined) {
                JoinerSnapshot(body, bodyLen);
            }
        }
    }
}

void DeliverCommands(uint8_t* block)
{
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
    Com_Printf("^2COOP: hosting on UDP port %d\n", PORT);
}

void Cmd_Join()
{
    // la IP se toma de la linea de comandos ("+coop2join 1.2.3.4"): todavia no hay Cmd_Argv de CoD2
    const char* c = strstr(GetCommandLineA(), "coop2join");
    char ip[64] = "127.0.0.1";
    if (c) sscanf(c + 9, " %63[0-9.]", ip);
    if (!OpenSocket(0)) return;
    g_host = {};
    g_host.sin_family = AF_INET;
    g_host.sin_port = htons(PORT);
    g_host.sin_addr.s_addr = inet_addr(ip);
    g_role = JOINER;
    g_joined = false;
    Log("joiner: uniendome a %s:%d", ip, PORT);
    Com_Printf("^2COOP: joining %s...\n", ip);
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
    ClientThink(0);
}

// SV_FinishFrame, antes de pasarle al cliente local los comandos del servidor (EBX = bloque). Devuelve el bloque a
// pasar o 0 para no pasar nada.
extern "C" uint8_t* __cdecl Cod2_Reliable(uint8_t* block)
{
    if (g_role == HOST && *(int*)(block + 4) != *(int*)(block + 8)) {  // comandos nuevos este frame
        memcpy(g_block, block, BLOCK_SIZE);
        g_blockSends = 3;
    }
    if (g_role == JOINER && g_joined) return nullptr;  // los del juego local no; los del host en Cod2_Snapshot
    return block;
}

// SV_FinishFrame, en lugar del snapshot del cliente local. Devuelve 1 para mandar el snapshot local.
extern "C" int __cdecl Cod2_Snapshot()
{
    Poll();
    if (g_role == HOST) {
        HostSendSnapshots();
        return 1;
    }
    if (g_role == JOINER) {
        if (!g_joined) {
            if (GetTickCount() - g_lastJoinTry > 1000) JoinerSendJoin();
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
    return g_runFrame(partial, limit);
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
.att_syntax
)");
extern "C" void Cod2_LocalThinkStub();
extern "C" void Cod2_ReliableStub();
extern "C" void Cod2_SnapshotStub();

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
    for (auto& r : g_remote) r.active = false;  // mapa nuevo: los remotos vuelven a entrar
    g_joined = false;
    static bool cmds;
    if (!cmds) {
        cmds = true;
        Cmd_AddCommand("coop2host", Cmd_Host);
        Cmd_AddCommand("coop2join", Cmd_Join);
    }
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
    uint8_t patch[8] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)target - (at + 5));
    hook::Write(at, patch, len);
}

void Install()
{
    if (!Expect(0x448eca, "\x68\x08\xaa\x00\x00", 5) || !Expect(0x448edd, "\x68\x08\xaa\x00\x00", 5) ||
        !Expect(INIT_CLIENTS_AT, "\x89\x2d\x2c\x24\x82\x00", 6) || !Expect(LOCAL_THINK_CALL, "\xe8\x65\xdf\x0d\x00", 5) ||
        !Expect(RELIABLE_CALL, "\xe8\x41\x4c\xfc\xff", 5) || !Expect(SNAPSHOT_CALL, "\xe8\x2a\x08\x00\x00", 5) ||
        !Expect(G_RUN_FRAME, "\xa1\x5c\x10\xc6\x01", 5))
        return;
    // guardado: puntero a cliente como indice+1 (WriteField1 0x4e0d3d, ReadField 0x4e1125) y numero de cliente
    // (G_LoadGame 0x4e4f7b); los tres comparan con 1
    struct { uintptr_t at; uint8_t max; } limits[] = {{0x4e0d3d, MAX_CLIENTS}, {0x4e1125, MAX_CLIENTS}, {0x4e4f7b, MAX_CLIENTS - 1}};
    for (auto& l : limits)
        if (Expect(l.at, l.at == 0x4e1125 ? "\x83\xfe\x01" : "\x83\xff\x01", 3)) hook::Write(l.at + 2, &l.max, 1);
    // SV_Startup: svs.clients con lugar para 4 (malloc y memset)
    uint32_t size = MAX_CLIENTS * CLIENT_SIZE;
    hook::Write(0x448eca + 1, &size, 4);
    hook::Write(0x448edd + 1, &size, 4);
    CallTo(INIT_CLIENTS_AT, (void*)Cod2_InitClientsStub, 6);  // Cod2_SetupClients pone sv.num_entities
    CallTo(LOCAL_THINK_CALL, (void*)Cod2_LocalThinkStub, 5);
    CallTo(RELIABLE_CALL, (void*)Cod2_ReliableStub, 5);
    CallTo(SNAPSHOT_CALL, (void*)Cod2_SnapshotStub, 5);
    g_runFrame = (int(__cdecl*)(int, int))hook::Inline(G_RUN_FRAME, (void*)Cod2_RunFrame, 5);
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
        Install();
    }
    return TRUE;
}
