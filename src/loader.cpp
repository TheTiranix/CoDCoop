// ddraw.dll para CoDSP.exe: reenvia las 2 funciones que importa el juego al ddraw del sistema y
// carga el mod. Los ganchos al motor estan aca (fijos); la logica vive en codcoop_core.dll, que se
// recarga sola cuando cambia el archivo (o con `coopreload`). La memoria de estado del nucleo la
// guarda este cargador para que la conexion sobreviva a la recarga.
#include <windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "api.h"
#include "game.h"
#include "hook.h"

static FILE* g_log;
static char g_dir[MAX_PATH];  // carpeta del exe, con barra final

static void LogLine(const char* s)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s);
    fflush(g_log);
}

static void Log(const char* fmt, ...)
{
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    LogLine(b);
}

static FARPROC Real(const char* name)
{
    static HMODULE m;
    if (!m) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\ddraw.dll");
        m = LoadLibraryA(p);
    }
    return m ? GetProcAddress(m, name) : nullptr;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreateEx(GUID* g, void** dd, REFIID iid, IUnknown* outer)
{
    auto f = (HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*))Real("DirectDrawCreateEx");
    return f ? f(g, dd, iid, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawEnumerateExA(void* cb, void* ctx, DWORD flags)
{
    auto f = (HRESULT(WINAPI*)(void*, void*, DWORD))Real("DirectDrawEnumerateExA");
    return f ? f(cb, ctx, flags) : E_FAIL;
}

// ---- nucleo recargable ----
struct Core {
    HMODULE module;
    char path[MAX_PATH];  // copia temporal cargada (el original queda libre para el build)
    void(__cdecl* hostFrame)();
    int(__cdecl* frozenFrame)(int);
    void(__cdecl* command)(const char*);
    void(__cdecl* shutdown)();
};
static Core g_core;
static FILETIME g_coreFileTime;  // del codcoop_core.dll que se intento cargar por ultima vez
static uint8_t* g_state;
static uint32_t g_stateVersion;
static const CoreApi g_api = {LogLine};

extern "C" {
void* g_trampSend;
void* g_trampGetLoop;
void* g_coreSendPacket;  // los usan los stubs en asm; null mientras no hay nucleo
void* g_coreGetPacket;
void __cdecl Loader_Tick();
void HookSendPacket();
void HookGetLoopPacket();
}

static char g_cmdNames[32][32];
static int g_numCmds;

static void ReloadCore();

// Todos los comandos del mod entran por aca: el nombre esta en argv[0].
static void __cdecl LoaderCmd()
{
    const char* name = g::Argv(0);
    if (!_stricmp(name, "coopreload")) return ReloadCore();
    if (g_core.command) g_core.command(name);
}

static void RegisterCmd(const char* name)
{
    for (int i = 0; i < g_numCmds; ++i)
        if (!_stricmp(g_cmdNames[i], name)) return;
    if (g_numCmds == 32 || strlen(name) >= sizeof g_cmdNames[0]) return;
    strcpy(g_cmdNames[g_numCmds], name);
    g::Cmd_AddCommand(g_cmdNames[g_numCmds++], LoaderCmd);
}

static void UnloadCore()
{
    if (!g_core.module) return;
    g_coreSendPacket = g_coreGetPacket = nullptr;
    Core old = g_core;
    g_core = Core{};
    old.shutdown();
    FreeLibrary(old.module);
    DeleteFileA(old.path);
}

static bool LoadCore()
{
    char src[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof src, "%scodcoop_core.dll", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &fa)) {
        Log("falta %s", src);
        return false;
    }
    g_coreFileTime = fa.ftLastWriteTime;
    static int count;
    snprintf(dst, sizeof dst, "%scodcoop_tmp\\core_%lu_%d.dll", g_dir, GetCurrentProcessId(), ++count);
    if (!CopyFileA(src, dst, FALSE)) {
        Log("no se pudo copiar el nucleo a %s (%lu)", dst, GetLastError());
        return false;
    }
    HMODULE m = LoadLibraryA(dst);
    if (!m) {
        Log("no se pudo cargar %s (%lu)", dst, GetLastError());
        return false;
    }
    auto version = (uint32_t(__cdecl*)())GetProcAddress(m, "Coop_StateVersion");
    auto size = (int(__cdecl*)())GetProcAddress(m, "Coop_StateSize");
    auto init = (void(__cdecl*)(const CoreApi*, void*, int))GetProcAddress(m, "Coop_Init");
    auto commands = (const char*(__cdecl*)())GetProcAddress(m, "Coop_Commands");
    Core c{m};
    strcpy(c.path, dst);
    c.hostFrame = (void(__cdecl*)())GetProcAddress(m, "Coop_HostFrame");
    c.frozenFrame = (int(__cdecl*)(int))GetProcAddress(m, "Coop_FrozenFrame");
    c.command = (void(__cdecl*)(const char*))GetProcAddress(m, "Coop_Command");
    c.shutdown = (void(__cdecl*)())GetProcAddress(m, "Coop_Shutdown");
    void* send = (void*)GetProcAddress(m, "Coop_SendPacket");
    void* get = (void*)GetProcAddress(m, "Coop_GetPacket");
    if (!version || !size || !init || !commands || !c.hostFrame || !c.frozenFrame || !c.command || !c.shutdown || !send || !get) {
        Log("al nucleo %s le faltan funciones exportadas", dst);
        FreeLibrary(m);
        DeleteFileA(dst);
        return false;
    }
    int fresh = 0;
    if (!g_state || version() != g_stateVersion) {
        if (g_state) VirtualFree(g_state, 0, MEM_RELEASE);
        g_state = (uint8_t*)VirtualAlloc(nullptr, size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_stateVersion = version();
        fresh = 1;
    }
    init(&g_api, g_state, fresh);
    char names[1024];  // Coop_Commands: nombres separados por espacios (el nucleo tiene lugar para 512)
    strncpy(names, commands(), sizeof names - 1);
    names[sizeof names - 1] = 0;
    for (char* t = strtok(names, " "); t; t = strtok(nullptr, " ")) RegisterCmd(t);
    g_core = c;
    g_coreSendPacket = send;
    g_coreGetPacket = get;
    Log("nucleo cargado: %s (%s)", dst, fresh ? "estado nuevo" : "estado conservado");
    return true;
}

static void ReloadCore()
{
    UnloadCore();
    LoadCore();
}

// Se llama varias veces por frame desde NET_GetLoopPacket (fuera del nucleo): primera carga y
// deteccion de un codcoop_core.dll nuevo (quieto hace 1 s, para no leerlo a medio copiar).
extern "C" void __cdecl Loader_Tick()
{
    static bool started;
    if (!started) {
        started = true;
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof tmp, "%scodcoop_tmp", g_dir);
        CreateDirectoryA(tmp, nullptr);
        WIN32_FIND_DATAA fd;
        snprintf(tmp, sizeof tmp, "%scodcoop_tmp\\core_*.dll", g_dir);
        HANDLE h = FindFirstFileA(tmp, &fd);
        for (BOOL ok = h != INVALID_HANDLE_VALUE; ok; ok = FindNextFileA(h, &fd)) {
            snprintf(tmp, sizeof tmp, "%scodcoop_tmp\\%s", g_dir, fd.cFileName);
            DeleteFileA(tmp);  // las de otra instancia abierta no se borran (estan en uso)
        }
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        RegisterCmd("coopreload");
        LoadCore();
    }
    static DWORD last;
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    char src[MAX_PATH];
    snprintf(src, sizeof src, "%scodcoop_core.dll", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &fa) || !CompareFileTime(&fa.ftLastWriteTime, &g_coreFileTime)) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{{now.dwLowDateTime, now.dwHighDateTime}}, b{{fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime}};
    if (a.QuadPart - b.QuadPart < 10000000) return;  // 1 s en unidades de 100 ns
    g_coreFileTime = fa.ftLastWriteTime;
    Log("hay un nucleo nuevo en disco: recargando");
    g::Cbuf_AddText("coopreload\n");  // se ejecuta desde la consola, sin codigo del nucleo en la pila
}

// ---- ganchos del motor ----
static bool Expect(uintptr_t addr, const char* bytes, size_t n)
{
    if (!memcmp((void*)addr, bytes, n)) return true;
    Log("ERROR: bytes inesperados en %08x (otra version del exe?)", (unsigned)addr);
    return false;
}

static void (*g_origSendClientMessages)();
static void (*g_origSvFrame)(int);

// NET_SendPacket 0x43f290: ECX sock, EAX data, EBX len, netadr_t por valor en la pila (limpia el llamador).
// NET_GetLoopPacket 0x43f1c0: EAX sock, EBX msg_t*, [esp+4] from; devuelve 1 si hay paquete.
asm(R"(
.intel_syntax noprefix
.globl _HookSendPacket
_HookSendPacket:
    cmp dword ptr [esp+4], 4
    je 1f
    jmp dword ptr [_g_trampSend]
1:
    push esi
    mov esi, dword ptr [_g_coreSendPacket]
    test esi, esi
    jz 3f
    lea edx, [esp+8]
    push edx
    push ebx
    push eax
    push ecx
    call esi
    add esp, 16
3:
    pop esi
    ret
.globl _HookGetLoopPacket
_HookGetLoopPacket:
    push eax
    push dword ptr [esp+8]
    call dword ptr [_g_trampGetLoop]
    add esp, 4
    pop ecx
    test eax, eax
    jnz 2f
    push ecx
    call _Loader_Tick
    pop ecx
    xor eax, eax
    mov edx, dword ptr [_g_coreGetPacket]
    test edx, edx
    jz 2f
    push dword ptr [esp+4]
    push ebx
    push ecx
    call edx
    add esp, 12
2:
    ret
.att_syntax
)");

static void __cdecl Hook_SendClientMessages()
{
    g_origSendClientMessages();
    if (g_core.hostFrame) g_core.hostFrame();
}

static void __cdecl Hook_SvFrame(int msec)
{
    if (!g_core.frozenFrame || !g_core.frozenFrame(msec)) g_origSvFrame(msec);
}

static void InstallHooks()
{
    // los mismos 4 ganchos en CoD1 y en UO (solo cambian las direcciones que llevan adentro)
    uintptr_t send = g::A(0x43f290, 0x441120), getLoop = g::A(0x43f1c0, 0x441050), sendMsgs = g::A(0x44da50, 0x44fd00),
              svFrame = g::A(0x44cf10, 0x44f220);
    bool ok = g::uo ? Expect(send, "\x56\x8B\xF0\xA1\x38\x1D\x80\x04", 8) && Expect(getLoop, "\x69\xC0\x0C\x20\x00\x00", 6) &&
                          Expect(sendMsgs, "\xA1\xA4\x07\x7C\x03", 5) && Expect(svFrame, "\x51\xA1\x80\x1C\x80\x04", 6)
                    : Expect(send, "\x56\x8B\xF0\xA1\x80\x07\x4F\x01", 8) && Expect(getLoop, "\x69\xC0\x0C\x20\x00\x00", 6) &&
                          Expect(sendMsgs, "\xA1\xB8\x9D\x3F\x01", 5) && Expect(svFrame, "\x51\xA1\x20\xE9\x52\x01", 6);
    if (!ok) return;
    g_trampSend = hook::Inline(send, (void*)HookSendPacket, 8);
    g_trampGetLoop = hook::Inline(getLoop, (void*)HookGetLoopPacket, 6);
    g_origSendClientMessages = (void (*)())hook::Inline(sendMsgs, (void*)Hook_SendClientMessages, 5);
    g_origSvFrame = (void (*)(int))hook::Inline(svFrame, (void*)Hook_SvFrame, 6);
    Log("ganchos (%s): send %p getloop %p sendmsgs %p svframe %p", g::uo ? "UO" : "CoD1", g_trampSend, g_trampGetLoop,
        (void*)g_origSendClientMessages, (void*)g_origSvFrame);
}

// Crash sin manejar: direccion y posibles direcciones de retorno de la pila (codigo del exe o de las DLL).
static LONG WINAPI CrashLog(EXCEPTION_POINTERS* ep)
{
    CONTEXT* c = ep->ContextRecord;
    Log("CRASH %08lx en %08lx (eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx)",
        ep->ExceptionRecord->ExceptionCode, (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, c->Eax,
        c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    char line[512] = "";
    int len = 0, found = 0;
    const uint32_t* sp = (const uint32_t*)c->Esp;
    for (int i = 0; i < 2048 && found < 16 && !IsBadReadPtr(sp + i, 4); ++i) {
        uint32_t v = sp[i];
        HMODULE m;
        if ((v >= 0x401000 && v < g::A(0x533000, 0x53f000)) ||
            (v > 0x10000000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  (LPCSTR)(uintptr_t)v, &m) && m != GetModuleHandleA("kernel32.dll"))) {
            len += snprintf(line + len, sizeof line - len, " %08x", v);
            ++found;
        }
    }
    Log("  pila:%s (nucleo en %p)", line, (void*)g_core.module);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        strrchr(g_dir, '\\')[1] = 0;
        char p[MAX_PATH];
        // una instancia que se une escribe aparte (pruebas con 2 juegos en la misma carpeta)
        snprintf(p, sizeof p, "%s%s", g_dir, strstr(GetCommandLineA(), "+coopjoin") ? "codcoop_join.log" : "codcoop.log");
        g_log = _fsopen(p, "w", _SH_DENYWR);  // otro juego abierto desde la misma carpeta ya lo tiene: *_2.log
        if (!g_log) {
            strcpy(strrchr(p, '.'), "_2.log");
            g_log = _fsopen(p, "w", _SH_DENYWR);
        }
        Log("codcoop cargado en %s, exe base %p", GetCommandLineA(), GetModuleHandleA(nullptr));
        LoadLibraryA("ws2_32.dll");  // que winsock quede cargado aunque se descargue el nucleo
        InstallHooks();
        SetUnhandledExceptionFilter(CrashLog);
    }
    return TRUE;
}
