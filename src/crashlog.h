#pragma once
// Registro de cierres (CoD1/UO en loader.cpp, CoD2 en cod2.cpp): la direccion del cierre y las posibles direcciones
// de retorno de la pila como "modulo+desplazamiento", para que sirvan aunque el modulo cargue en otro lugar (el
// modulo de juego de UO a veces queda debajo de 0x10000000).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace crashlog {

// direccion dentro del codigo de un modulo que no es de Windows: "nombre+desplazamiento" en out
inline bool CodeAddress(uint32_t v, char* out, size_t size, bool anyCode = false)
{
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)v, &m))
        return false;
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    uintptr_t code = (uintptr_t)m + nt->OptionalHeader.BaseOfCode;
    if (!anyCode && (v < code || v >= code + nt->OptionalHeader.SizeOfCode)) return false;
    char path[MAX_PATH], win[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    GetWindowsDirectoryA(win, MAX_PATH);
    if (!anyCode && !_strnicmp(path, win, strlen(win))) return false;  // kernel32, ntdll...: ruido
    const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    snprintf(out, size, "%s+%x", name, (unsigned)(v - (uint32_t)(uintptr_t)m));
    return true;
}

// hasta 16 direcciones de retorno posibles desde sp, como " modulo+desp" cada una
inline void Stack(const uint32_t* sp, char* line, size_t size)
{
    char one[96];
    int len = 0, found = 0;
    line[0] = 0;
    for (int i = 0; i < 4096 && found < 16 && len < (int)size - 100 && !IsBadReadPtr(sp + i, 4); ++i)
        if (CodeAddress(sp[i], one, sizeof one)) {
            len += snprintf(line + len, size - len, " %s", one);
            ++found;
        }
}

// dos lineas al registro: el cierre con los registros y hasta 16 direcciones de retorno posibles
template <class LogFn> void Write(EXCEPTION_POINTERS* ep, LogFn log)
{
    CONTEXT* c = ep->ContextRecord;
    uint32_t at = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    char where[96] = "?";
    CodeAddress(at, where, sizeof where, true);
    log("CRASH %08lx en %08x %s (eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx)",
        ep->ExceptionRecord->ExceptionCode, at, where, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    char line[1024];
    Stack((const uint32_t*)c->Esp, line, sizeof line);
    log("  pila:%s", line);
}

}  // namespace crashlog
