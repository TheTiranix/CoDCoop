// Prueba de CoD2 (CoD2SP_s.exe 1.3): el servidor de un jugador reserva un solo cliente. Esto lo amplia a 4 y
// agrega `coop2spawn` para meter al jugador 2 en el mapa (sin red todavia). Lo carga ddraw.dll (loader.cpp).
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
// G_InitGame: `mov [sv.num_entities], ebp` (ebp = 1) despues de armar level.clients y antes de cargar el mapa
constexpr uintptr_t INIT_CLIENTS_AT = 0x4d9be9;

auto Com_Printf = (void(__cdecl*)(const char*, ...))0x42c130;
auto Cmd_AddCommand = (void(__cdecl*)(const char*, void (*)()))0x41bb00;
auto ClientConnect = (int(__cdecl*)(int))0x4e8f40;
constexpr uintptr_t CLIENT_BEGIN = 0x4e8fe0;  // EAX = cliente

alignas(16) uint8_t g_clients[MAX_CLIENTS * GCLIENT_SIZE];
FILE* g_log;
constexpr uintptr_t SVS_CLIENTS = 0x825508, CL_LASTUSERCMD = 0x2410, USERCMD_SIZE = 0x30;
constexpr uintptr_t CLIENT_THINK = 0x4e7ee0, LOCAL_THINK_CALL = 0x409f76;  // ClientThink: EAX = cliente

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

uint8_t* Entity(int n) { return (uint8_t*)G_ENTITIES + n * GENTITY_SIZE; }

void ClientBegin(int n)
{
    asm volatile("call *%1" : "+a"(n) : "r"(CLIENT_BEGIN) : "ecx", "edx", "memory");
}

void Cmd_Spawn()
{
    int n = 1;
    if (*(int*)(g_clients + n * GCLIENT_SIZE + GC_CONNECTED)) {
        Com_Printf("coop2spawn: el jugador 2 ya esta\n");
        return;
    }
    // ClientConnect empieza con la init de caminos de la IA (call 0x516bc0 en 0x4e8f48), pensada para el inicio del
    // nivel: a mitad de partida deja una lista circular y el juego se cuelga (igual que en CoD1). Se saltea.
    const uintptr_t pathInit = 0x4e8f48;
    uint8_t saved[5];
    memcpy(saved, (void*)pathInit, 5);
    bool skip = !memcmp(saved, "\xe8\x73\xdc\x02\x00", 5);
    if (skip) hook::Write(pathInit, "\x0F\x1F\x44\x00\x00", 5);
    ClientConnect(n);
    if (skip) hook::Write(pathInit, saved, 5);
    ClientBegin(n);
    const float* o = (const float*)(g_clients + n * GCLIENT_SIZE + 0x14);  // ps.origin
    Com_Printf("coop2spawn: jugador 2 en %.0f %.0f %.0f\n", o[0], o[1], o[2]);
    Log("coop2spawn: jugador 2 en %.0f %.0f %.0f", o[0], o[1], o[2]);
}
}  // namespace

// Prueba: despues del jugador local, los demas jugadores conectados piensan con el mismo usercmd.
bool g_think;  // `coop2think` lo prende y apaga

extern "C" void __cdecl Cod2_AfterThink(const uint8_t* cmd)
{
    if (!g_think) return;
    static DWORD lastLog;
    for (int n = 1; n < MAX_CLIENTS; ++n) {
        uint8_t* gc = g_clients + n * GCLIENT_SIZE;
        if (*(int*)(gc + GC_CONNECTED) != 2) continue;
        memcpy(*(uint8_t**)SVS_CLIENTS + n * CLIENT_SIZE + CL_LASTUSERCMD, cmd, USERCMD_SIZE);
        int num = n;
        asm volatile("call *%1" : "+a"(num) : "r"(CLIENT_THINK) : "ecx", "edx", "memory");
        if (GetTickCount() - lastLog > 1000) {
            lastLog = GetTickCount();
            const float *o = (const float*)(gc + 0x14), *h = (const float*)(g_clients + 0x14);
            char hex[USERCMD_SIZE * 2 + 1];
            for (int i = 0; i < (int)USERCMD_SIZE; ++i) sprintf(hex + i * 2, "%02x", cmd[i]);
            Log("host %.0f %.0f %.0f | jugador %d %.0f %.0f %.0f | cmd %s", h[0], h[1], h[2], n + 1, o[0], o[1], o[2], hex);
        }
    }
}
// El cliente local llama a ClientThink(0) directo (0x409f76, sin pasar por SV_ClientThink): ese call va aca.
extern "C" void __cdecl Cod2_ThinkAll()
{
    int zero = 0;
    asm volatile("call *%1" : "+a"(zero) : "r"(CLIENT_THINK) : "ecx", "edx", "memory");
    Cod2_AfterThink(*(uint8_t**)SVS_CLIENTS + CL_LASTUSERCMD);
}
namespace {
}  // namespace

// Llamado desde G_InitGame: level.clients pasa a un arreglo de 4 y las entidades 0..3 quedan para jugadores.
extern "C" void __cdecl Cod2_SetupClients()
{
    memset(g_clients, 0, sizeof g_clients);
    *(uint8_t**)LEVEL_CLIENTS = g_clients;
    *(uint8_t**)SV_GAMECLIENTS = g_clients;
    *(int*)LEVEL_MAXCLIENTS = MAX_CLIENTS;
    for (int n = 0; n < MAX_CLIENTS; ++n) *(uint8_t**)(Entity(n) + GE_CLIENT) = g_clients + n * GCLIENT_SIZE;
    *(int*)LEVEL_NUM_ENTITIES = MAX_CLIENTS;
    *(int*)SV_GENTITIES_NUM = MAX_CLIENTS;
    static bool cmds;
    if (!cmds) {
        cmds = true;
        Cmd_AddCommand("coop2spawn", Cmd_Spawn);
        Cmd_AddCommand("coop2think", [] { g_think = !g_think; Com_Printf("coop2think %d\n", g_think); });
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

void Install()
{
    if (!Expect(0x448eca, "\x68\x08\xaa\x00\x00", 5) || !Expect(0x448edd, "\x68\x08\xaa\x00\x00", 5) ||
        !Expect(INIT_CLIENTS_AT, "\x89\x2d\x2c\x24\x82\x00", 6) ||
        !Expect(LOCAL_THINK_CALL, "\xe8\x65\xdf\x0d\x00", 5))
        return;
    hook::PatchRel32(LOCAL_THINK_CALL, (void*)Cod2_ThinkAll);
    // guardado: puntero a cliente como indice+1 (WriteField1 0x4e0d3d, ReadField 0x4e1125) y numero de cliente
    // (G_LoadGame 0x4e4f7b); los tres comparan con 1
    struct { uintptr_t at; uint8_t max; } limits[] = {{0x4e0d3d, MAX_CLIENTS}, {0x4e1125, MAX_CLIENTS}, {0x4e4f7b, MAX_CLIENTS - 1}};
    for (auto& l : limits)
        if (Expect(l.at, l.at == 0x4e1125 ? "\x83\xfe\x01" : "\x83\xff\x01", 3)) hook::Write(l.at + 2, &l.max, 1);
    // SV_Startup: svs.clients con lugar para 4 (malloc y memset)
    uint32_t size = MAX_CLIENTS * CLIENT_SIZE;
    hook::Write(0x448eca + 1, &size, 4);
    hook::Write(0x448edd + 1, &size, 4);
    // G_InitGame: `mov [sv.num_entities], ebp` -> call al stub (Cod2_SetupClients pone ese valor en 4)
    uint8_t patch[6] = {0xE8, 0, 0, 0, 0, 0x90};
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)Cod2_InitClientsStub - (INIT_CLIENTS_AT + 5));
    hook::Write(INIT_CLIENTS_AT, patch, 6);
    Log("parches puestos (svs.clients x%d, G_InitGame)", MAX_CLIENTS);
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH];
        GetModuleFileNameA(self, p, MAX_PATH);
        strcpy(strrchr(p, '\\') + 1, "cod2coop.log");
        g_log = fopen(p, "w");
        Install();
    }
    return TRUE;
}
