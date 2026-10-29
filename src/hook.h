#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// Utilidades minimas de parcheo para un exe de 32 bits sin ASLR (base 0x400000).
namespace hook {

inline bool Write(uintptr_t addr, const void* data, size_t len)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, data, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    return true;
}

// Reemplaza el destino de un `call rel32` o `jmp rel32` en `addr`.
inline bool PatchRel32(uintptr_t addr, void* target)
{
    uint8_t op = *(uint8_t*)addr;
    if (op != 0xE8 && op != 0xE9) return false;
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));
    return Write(addr + 1, &rel, 4);
}

// Hook inline: copia `stolen` bytes (instrucciones completas, >= 5) a un trampolin y pone un
// jmp a `detour`. Devuelve el trampolin para llamar a la original. Las instrucciones robadas
// no pueden llevar operandos relativos (call/jmp/jcc).
inline void* Inline(uintptr_t addr, void* detour, size_t stolen)
{
    if (stolen < 5) return nullptr;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, stolen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, (void*)addr, stolen);
    tramp[stolen] = 0xE9;
    *(int32_t*)(tramp + stolen + 1) = (int32_t)((addr + stolen) - ((uintptr_t)tramp + stolen + 5));
    uint8_t patch[16];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)detour - (addr + 5));
    if (!Write(addr, patch, stolen)) return nullptr;
    return tramp;
}

// Engancha una funcion exportada por una DLL (kernel32, user32...). Sigue stubs `jmp`
// hasta el cuerpo real y solo soporta los prologos `mov edi,edi; push ebp; mov ebp,esp` (5 bytes)
// y `jmp [abs]` (6 bytes). Devuelve el trampolin, o nullptr (y deja el motivo en `why`).
inline void* Api(const char* mod, const char* fn, void* detour, char* why, size_t whyLen)
{
    HMODULE m = GetModuleHandleA(mod);
    if (!m) m = LoadLibraryA(mod);
    uint8_t* p = m ? (uint8_t*)GetProcAddress(m, fn) : nullptr;
    if (!p) { snprintf(why, whyLen, "no se encontro %s!%s", mod, fn); return nullptr; }
    for (int hop = 0; hop < 4; ++hop) {
        if (p[0] == 0xFF && p[1] == 0x25) {                       // jmp dword ptr [abs]
            uint8_t* next = *(uint8_t**)(*(uintptr_t*)(p + 2));
            if (!next) break;
            p = next; continue;
        }
        if (p[0] == 0xE9) { p = p + 5 + *(int32_t*)(p + 1); continue; }   // jmp rel32
        break;
    }
    size_t stolen = 0;
    if (!memcmp(p, "\x8B\xFF\x55\x8B\xEC", 5)) stolen = 5;                // mov edi,edi; push ebp; mov ebp,esp
    else if (!memcmp(p, "\x55\x8B\xEC\x6A\xFF", 5)) stolen = 5;           // push ebp; mov ebp,esp; push -1
    if (!stolen) {
        snprintf(why, whyLen, "%s!%s prologo no soportado: %02X %02X %02X %02X %02X %02X %02X %02X", mod, fn,
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        return nullptr;
    }
    void* t = Inline((uintptr_t)p, detour, stolen);
    if (!t) snprintf(why, whyLen, "%s!%s: no se pudo parchear", mod, fn);
    return t;
}

// Reemplaza la entrada de la IAT del modulo `exe` para `dll!fn`. Devuelve la funcion original.
inline void* Iat(HMODULE exe, const char* dll, const char* fn, void* detour)
{
    uint8_t* b = (uint8_t*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(b + d->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(b + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            void* orig = (void*)iat->u1.Function;
            Write((uintptr_t)&iat->u1.Function, &detour, sizeof detour);
            return orig;
        }
    }
    return nullptr;
}

}  // namespace hook
