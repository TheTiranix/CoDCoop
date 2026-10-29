// ddraw.dll proxy para CoDSP.exe: reenvia las 2 funciones que importa el juego al ddraw del
// sistema y arranca el mod coop dentro del proceso.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include "hook.h"

static FILE* g_log;

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

// ---- ganchos del motor ----
static bool Expect(uintptr_t addr, const char* bytes, size_t n)
{
    if (!memcmp((void*)addr, bytes, n)) return true;
    Log("ERROR: bytes inesperados en %08x (otra version del exe?)", (unsigned)addr);
    return false;
}

struct netadr_t;
struct msg_t;
extern "C" {
void* g_trampSend;
void* g_trampGetLoop;
void (*g_origSendClientMessages)();
void (*g_origSvFrame)(int);
void __cdecl Coop_SendPacket(int sock, const void* data, int len, const netadr_t* to);
int __cdecl Coop_GetPacket(int sock, msg_t* msg, netadr_t* from);
void __cdecl Coop_HostFrame();
bool Coop_FrozenFrame(int msec);
void HookSendPacket();
void HookGetLoopPacket();
}

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
    lea edx, [esp+4]
    push edx
    push ebx
    push eax
    push ecx
    call _Coop_SendPacket
    add esp, 16
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
    push dword ptr [esp+4]
    push ebx
    push ecx
    call _Coop_GetPacket
    add esp, 12
2:
    ret
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

static void __cdecl Hook_SendClientMessages()
{
    g_origSendClientMessages();
    Coop_HostFrame();
}

static void __cdecl Hook_SvFrame(int msec)
{
    if (!Coop_FrozenFrame(msec)) g_origSvFrame(msec);
}

static void InstallHooks()
{
    if (!Expect(0x43f290, "\x56\x8B\xF0\xA1\x80\x07\x4F\x01", 8) || !Expect(0x43f1c0, "\x69\xC0\x0C\x20\x00\x00", 6) ||
        !Expect(0x44da50, "\xA1\xB8\x9D\x3F\x01", 5) || !Expect(0x44cf10, "\x51\xA1\x20\xE9\x52\x01", 6))
        return;
    g_trampSend = hook::Inline(0x43f290, (void*)HookSendPacket, 8);
    g_trampGetLoop = hook::Inline(0x43f1c0, (void*)HookGetLoopPacket, 6);
    g_origSendClientMessages = (void (*)())hook::Inline(0x44da50, (void*)Hook_SendClientMessages, 5);
    g_origSvFrame = (void (*)(int))hook::Inline(0x44cf10, (void*)Hook_SvFrame, 6);
    Log("ganchos: send %p getloop %p sendmsgs %p svframe %p", g_trampSend, g_trampGetLoop,
        (void*)g_origSendClientMessages, (void*)g_origSvFrame);
}

// Crash sin manejar: direccion y posibles direcciones de retorno de la pila (codigo del exe o de las DLL).
static LONG WINAPI CrashLog(EXCEPTION_POINTERS* ep)
{
    CONTEXT* c = ep->ContextRecord;
    Log("CRASH %08lx en %08lx (eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx)",
        ep->ExceptionRecord->ExceptionCode, (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, c->Eax,
        c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    char line[512];
    int len = 0, found = 0;
    const uint32_t* sp = (const uint32_t*)c->Esp;
    for (int i = 0; i < 2048 && found < 16 && !IsBadReadPtr(sp + i, 4); ++i) {
        uint32_t v = sp[i];
        HMODULE m;
        if ((v >= 0x401000 && v < 0x533000) ||
            (v > 0x10000000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  (LPCSTR)(uintptr_t)v, &m) && m != GetModuleHandleA("kernel32.dll"))) {
            len += snprintf(line + len, sizeof line - len, " %08x", v);
            ++found;
        }
    }
    Log("  pila:%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH];
        GetModuleFileNameA(nullptr, p, MAX_PATH);
        // una instancia que se une escribe aparte (pruebas con 2 juegos en la misma carpeta)
        strcpy(strrchr(p, '\\') + 1, strstr(GetCommandLineA(), "+coopjoin") ? "codcoop_join.log" : "codcoop.log");
        g_log = fopen(p, "w");
        Log("codcoop cargado en %s, exe base %p", GetCommandLineA(), GetModuleHandleA(nullptr));
        InstallHooks();
        SetUnhandledExceptionFilter(CrashLog);
    }
    return TRUE;
}
