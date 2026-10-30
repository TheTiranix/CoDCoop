#pragma once
// Direcciones y estructuras de CoDSP.exe 1.5 (ver notes/ARQUITECTURA.md).
#include <cstdint>

struct netadr_t {
    int type;
    uint8_t ip[4];
    uint8_t ipx[10];
    uint16_t port;  // orden de red
};
static_assert(sizeof(netadr_t) == 20, "netadr_t");

struct msg_t {
    int allowoverflow, overflowed;
    uint8_t* data;
    int maxsize, cursize, readcount, bit;
};

enum { NA_LOOPBACK = 2, NA_IP = 4 };
enum { NS_CLIENT = 0, NS_SERVER = 1 };

namespace g {
constexpr int MAX_CLIENTS = 4;
constexpr int CLIENT_SIZE = 0xb87c;    // client_t
constexpr int ENTSTATE_SIZE = 0x9c;    // entityState_t al principio de gentity_t
constexpr int MAX_GENTITIES = 2048;
constexpr int MAX_CONFIGSTRINGS = 2048;

// client_t (servidor)
constexpr int CL_STATE = 0x0, CL_RELSEQ = 0x18, CL_RELACK = 0x1c, CL_RELSENT = 0x20, CL_GAMESTATEMSG = 0x2c,
              CL_LASTCLIENTCMD = 0x60, CL_GENTITY = 0x464, CL_DELTA = 0x468, CL_NETCHAN = 0x784c;
// netchan_t
constexpr int NC_SOCK = 0x0, NC_ADR = 0x8, NC_QPORT = 0x1c, NC_IN = 0x20, NC_OUT = 0x24;

inline uint8_t*& svs_clients = *(uint8_t**)0x152e8e8;
inline uint8_t*& sv_gentities = *(uint8_t**)0x152bc58;
inline int& sv_gentitySize = *(int*)0x152bc5c;
inline int& sv_numEntities = *(int*)0x152bc60;
inline int& sv_state = *(int*)0x14f4840;        // 2 = jugando
inline uint16_t* sv_configstrings = (uint16_t*)0x14f4c54;
inline int& snap_numEntities = *(int*)0x152bcd4;  // lista global del ultimo snapshot construido
inline int* snap_entities = (int*)0x152bcd8;
inline uint8_t*& gvm = *(uint8_t**)0x13f9db8;
inline int& inGameCall = *(int*)0xa8f648;        // el motor lo pone en 1 antes de llamar al modulo de juego
inline uint8_t*& sl_pool = *(uint8_t**)0x8d8bf8;  // texto de un handle h = pool + h*8 + 4

inline int& cmd_argc = *(int*)0x885db8;
inline char** cmd_argv = (char**)0x8838b8;

inline uint8_t* clc_netchan = (uint8_t*)0x14b95d0;
inline int& cls_state = *(int*)0x1439260;
inline int& clc_reliableSequence = *(int*)0x14395bc;
inline int& clc_serverCommandSequence = *(int*)0x14795c8;

using Printf_t = void(__cdecl*)(const char*, ...);
inline auto Com_Printf = (Printf_t)0x42e300;
// 0x4213d0 = Cmd_AddCommand local. (0x420d40 es Cmd_AddServerCommand: esos solo corren dentro del frame
// del servidor, que en el que se une esta congelado.)
inline auto Cmd_AddCommand = (void(__cdecl*)(const char*, void (*)()))0x4213d0;
inline auto Z_Malloc = (void*(__cdecl*)(int))0x42a480;
inline auto Cvar_FindVar = (uint8_t*(__cdecl*)(const char*))0x431920;  // cvar_t: +4 string, +0x20 integer
inline auto Cbuf_AddText_ = (void*)0x4209d0;                            // texto en EAX
inline auto SV_SetConfigstring = (void(__cdecl*)(int, const char*))0x44b900;
inline auto CL_ApplyConfigstrings = (void(__cdecl*)(uint16_t*))0x40fae0;
inline auto CL_NetchanSetup = (void(__cdecl*)(netadr_t))0x40de40;

inline const char* Argv(int i) { return i < cmd_argc ? cmd_argv[i] : ""; }
inline const char* CvarString(const char* name)
{
    uint8_t* v = Cvar_FindVar(name);
    return v ? *(const char**)(v + 4) : "";
}
inline void Cbuf_AddText(const char* text)
{
    asm volatile("call *%1" : "+a"(text) : "r"(Cbuf_AddText_) : "ecx", "edx", "memory");
}
inline const char* ConfigString(int i)
{
    uint16_t h = sv_configstrings[i];
    return h ? (const char*)(sl_pool + h * 8 + 4) : "";
}
inline void SV_SendClientSnapshot(uint8_t* cl)  // 0x44d960, client en ECX
{
    asm volatile("call *%1" : "+c"(cl) : "r"((void*)0x44d960) : "eax", "edx", "memory");
}
// --- DObj (modelo + animacion) y XAnim; ver notes/ARQUITECTURA.md "Tramo 2" ---
constexpr int DOBJ_SIZE = 0x58;
inline int16_t* serverObjMap = (int16_t*)0x886f10;       // [MAX_GENTITIES] indice de DObj del servidor
inline uint8_t* dobjPool = (uint8_t*)0x888020;           // DObj: +0 arbol, +0x12 u16, +0x16 nModelos, +0x18 XModel*[8],
                                                         // +0x38 u16[8], +0x48 hueso padre[8]
inline uint8_t** xanimTable = (uint8_t**)0x8d84c8;       // [2][128] XAnim* (nombre en +0, cantidad en +4)
inline uint8_t* xanimInfo = (uint8_t*)0xa6d000;          // pool XAnimInfo 0x44 c/u; estado cliente +0x1c..+0x37
inline auto Com_ServerDObjCreate = (void(__cdecl*)(void* models, int num, void* tree, int ent, int u12))0x431030;
inline auto Com_SyncClientDObjs = (void(__cdecl*)())0x431150;  // serverObjMap -> clientObjMap
inline auto XAnimAllocInfo = (uint8_t*(__cdecl*)(void* tree, int anim))0x47d2d0;  // devuelve info+0x10
inline void Com_ServerDObjFree(int ent)  // 0x4310f0, ECX = ent
{
    asm volatile("call *%1" : "+c"(ent) : "r"((void*)0x4310f0) : "eax", "edx", "memory");
}
inline uint8_t* XAnimCreateTree(uint8_t* xanim)  // 0x431400, EBX = XAnim*
{
    uint8_t* r;
    asm volatile("call *%2" : "=a"(r), "+b"(xanim) : "r"((void*)0x431400) : "ecx", "edx", "memory");
    return r;
}
inline void XAnimFreeTree(uint8_t* tree)  // 0x431440, ECX = arbol
{
    asm volatile("call *%1" : "+c"(tree) : "r"((void*)0x431440) : "eax", "edx", "memory");
}
inline void XAnimFreeInfo(int index)  // 0x47a310, DI = indice del info (no limpia el arbol)
{
    asm volatile("call *%1" : "+D"(index) : "r"((void*)0x47a310) : "eax", "ecx", "edx", "memory");
}
inline uint8_t* XModelPrecache(const char* name)  // 0x4802f0: ECX = nombre sin "xmodel/", (0, alloc, 0)
{
    uint8_t* r;
    asm volatile("push $0\n\tpush $0x431380\n\tpush $0\n\tcall *%2\n\tadd $12, %%esp"
                 : "=a"(r), "+c"(name)
                 : "r"((void*)0x4802f0)
                 : "edx", "memory");
    return r;
}
inline void XAnimCloneServerToClient(uint8_t* tree)  // 0x47b010(nodo 0) con el arbol en el global 0xa6d008
{
    *(uint8_t**)0xa6d008 = tree;
    asm volatile("push $0\n\tcall *%0\n\tadd $4, %%esp" : : "r"((void*)0x47b010) : "eax", "ecx", "edx", "memory");
}
inline uint8_t* ServerDObj(int ent) { return serverObjMap[ent] ? dobjPool + serverObjMap[ent] * DOBJ_SIZE : nullptr; }

inline uint8_t* Client(int n) { return svs_clients + n * CLIENT_SIZE; }
inline uint8_t* Entity(int n) { return sv_gentities + n * sv_gentitySize; }
// VM_Call 0x44f270 (EAX = vm, 13 args): fija currentVM, que usan los traps del modulo de juego.
extern "C" int CallVM(void* vm, const int* args13);
inline int GameCall(int cmd, int a = 0, int b = 0, int c = 0)
{
    int args[13] = {cmd, a, b, c};
    inGameCall = 1;
    return CallVM(gvm, args);
}
}  // namespace g
