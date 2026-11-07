#pragma once
// Direcciones y estructuras de CoDSP.exe 1.5 y de CoDUOSP.exe 1.51 (United Offensive): ver notes/ARQUITECTURA.md
// y notes/uo_port.txt. A(cod1, uo) elige segun el exe que corre.
#include <windows.h>
#include <cstdint>
#include <cstring>

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
inline bool IsUO()
{
    static int v = -1;
    if (v < 0) {
        char p[MAX_PATH];
        GetModuleFileNameA(nullptr, p, MAX_PATH);
        const char* s = strrchr(p, '\\');
        v = _stricmp(s ? s + 1 : p, "CoDUOSP.exe") == 0;
    }
    return v;
}
inline const bool uo = IsUO();
inline uintptr_t A(uintptr_t cod1, uintptr_t uo_) { return IsUO() ? uo_ : cod1; }
inline const char* GameDll() { return IsUO() ? "uo_gamex86.dll" : "gamex86.dll"; }
inline const char* CgameDll() { return IsUO() ? "uo_cgamex86.dll" : "cgamex86.dll"; }
inline const char* UiDll() { return IsUO() ? "uo_uix86.dll" : "uix86.dll"; }

constexpr int MAX_CLIENTS = 4;
inline const int CLIENT_SIZE = (int)A(0xb87c, 0x106c4);  // client_t
constexpr int ENTSTATE_MAX = 0xa0;                        // para arreglos
inline const int ENTSTATE_SIZE = (int)A(0x9c, 0xa0);      // entityState_t al principio de gentity_t
constexpr int MAX_GENTITIES = 2048;
constexpr int MAX_CONFIGSTRINGS = 2048;
inline const int MAX_WEAPONS = (int)A(64, 128);

// client_t (servidor)
constexpr int CL_STATE = 0x0, CL_RELSEQ = 0x18, CL_RELACK = 0x1c, CL_RELSENT = 0x20, CL_GAMESTATEMSG = 0x2c,
              CL_LASTCLIENTCMD = 0x60, CL_GENTITY = 0x464, CL_DELTA = 0x468;
inline const int CL_NETCHAN = (int)A(0x784c, 0x8694);
// netchan_t
constexpr int NC_SOCK = 0x0, NC_ADR = 0x8, NC_QPORT = 0x1c, NC_IN = 0x20, NC_OUT = 0x24;

// playerState_t (= principio del gclient); ver notes/uo_playerstate.txt
inline const int PS_CLIENTNUM = (int)A(0x9c, 0xc4), PS_WEAPON = (int)A(0xa0, 0xc8), PS_VIEWMODEL = (int)A(0xac, 0xd4),
                 PS_AMMO = (int)A(0xf4, 0x11c), PS_CLIP = (int)A(0x1f4, 0x31c), PS_WEAPONS = (int)A(0x2f4, 0x51c),
                 PS_LOADOUT_END = (int)A(0x304, 0x534);  // fin de ammo, clip, armas y ranuras
inline const int PS_DELTA_ANGLES = (int)A(0x48, 0x4c), PS_GROUNDENT = (int)A(0x54, 0x58), PS_HEALTH = (int)A(0xe4, 0x10c),
                 PS_MAXHEALTH = (int)A(0xec, 0x114);
// gclient_t (despues del playerState)
inline const int GC_CONNECTED = (int)A(0x73e0, 0x8228), GC_CMD = (int)A(0x73e4, 0x822c), GC_LINKANGLES = (int)A(0x74c8, 0x8310);
// gentity_t: entityState igual hasta +0x9c (UO agrega 4 bytes al final), despues todo corrido
inline const int GE_CLIENT = (int)A(0x104, 0x108), GE_CONTENTS = (int)A(0xc4, 0xc8), GE_ORIGIN = (int)A(0xe0, 0xe4),
                 GE_FLAGS = (int)A(0x134, 0x13c), GE_HEALTH = (int)A(0x1d8, 0x1e0), GE_LINK = (int)A(0x294, 0x29c);
// WeaponDef (tabla en gamex86 + G_WEAPONDEFS): +4 nombre
inline const int WD_CLASS = (int)A(0x74, 0x80), WD_WORLDMODEL = (int)A(0x188, 0x1c8), WD_MAXAMMO = (int)A(0x198, 0x1e0),
                 WD_AMMOINDEX = (int)A(0x1a0, 0x1e8);
// clase de arma de vehiculo (ametralladora / cañon del tanque): CoD1 6 y 7, UO 7 y 9
inline bool VehicleClass(int cls) { return IsUO() ? cls == 7 || cls == 9 : cls == 6 || cls == 7; }
// gamex86 (RVA)
inline const uintptr_t G_WEAPONDEFS = A(0x125650, 0x151f34), G_NUMWEAPONS = A(0x12564c, 0x151f30),
                       G_ITEMREGISTERED = A(0x12eea0, 0x30c720), G_WEAPON_SLOT = A(0xb3e0, 0xc1d0),
                       G_WEAPON_GIVE = A(0xb000, 0xbde0), G_ADD_AMMO = A(0x1a940, 0x1caf0), G_DAMAGE = A(0x18800, 0x1a940),
                       G_ENTLINKTO = A(0x44b90, 0x48c40), G_ENTUNLINK = A(0x44d70, 0x48e20),
                       G_SETLINKOFFSET = A(0x45280, 0x49330), G_EXIT_REQUEST = A(0x2dbd00, 0x3089e0),
                       G_EXIT_VICTORY = A(0x2dbd08, 0x3089e8), G_EXIT_MAP = A(0x2dbd18, 0x3089f8),
                       G_EXIT_TYPE = A(0x298cac, 0x2c1fec), G_CONNECT_PATHINIT = A(0x14808, 0x163d8),
                       G_PATHINIT = A(0x6fde0, 0x75270), G_PMOVE_TRACE_IMM = A(0x12601, 0x140d1),
                       G_TRACE = A(0x42b00, 0x46740);

inline uint8_t*& svs_clients = *(uint8_t**)A(0x152e8e8, 0x4801c48);
inline uint8_t*& sv_gentities = *(uint8_t**)A(0x152bc58, 0x47fefb8);
inline int& sv_gentitySize = *(int*)A(0x152bc5c, 0x47fefbc);
inline int& sv_numEntities = *(int*)A(0x152bc60, 0x47fefc0);
inline uint8_t*& sv_gameClients = *(uint8_t**)A(0x152bc64, 0x47fefc4);
inline int& sv_gameClientSize = *(int*)A(0x152bc68, 0x47fefc8);
inline int& sv_state = *(int*)A(0x14f4840, 0x47c7ba0);        // 2 = jugando
inline uint16_t* sv_configstrings = (uint16_t*)A(0x14f4c54, 0x47c7fb4);
inline int& sv_time = *(int*)A(0x152e930, 0x4801c90);
inline int& snap_numEntities = *(int*)A(0x152bcd4, 0x47ff034);  // lista global del ultimo snapshot construido
inline int* snap_entities = (int*)A(0x152bcd8, 0x47ff038);
inline uint8_t*& gvm = *(uint8_t**)A(0x13f9db8, 0x37c07a4);
inline uint8_t*& currentVM = *(uint8_t**)A(0x13f9db4, 0x37c07ac);
inline int& inGameCall = *(int*)A(0xa8f648, 0xaadd40);        // el motor lo pone en 1 antes de llamar al modulo de juego
inline uint8_t*& sl_pool = *(uint8_t**)A(0x8d8bf8, 0x8f72f0);  // texto de un handle h = pool + h*8 + 4

inline int& cmd_argc = *(int*)A(0x885db8, 0x8933f8);
inline char** cmd_argv = (char**)A(0x8838b8, 0x8909f8);

inline uint8_t* clc_netchan = (uint8_t*)A(0x14b95d0, 0x48c2470);
inline int& cls_state = *(int*)A(0x1439260, 0x4842100);
inline int& clc_reliableSequence = *(int*)A(0x14395bc, 0x484245c);
inline int& clc_serverMessageSequence = *(int*)A(0x14795c4, 0x4882464);
inline int& clc_serverCommandSequence = *(int*)A(0x14795c8, 0x4882468);
inline int& cl_snapServerTime = *(int*)A(0x1426e68, 0x482e088);
inline int& cl_snapNum = *(int*)A(0x1426e6c, 0x482e08c);
inline uint8_t*& cgvm = *(uint8_t**)A(0x14bd620, 0x48ca4c8);
inline uint8_t*& uivm = *(uint8_t**)A(0x14bd648, 0x482df44);
inline uint8_t*& cl_paused = *(uint8_t**)A(0x14e440c, 0x4805ee8);  // cvar_t*

using Printf_t = void(__cdecl*)(const char*, ...);
inline auto Com_Printf = (Printf_t)A(0x42e300, 0x42ffb0);
// Cmd_AddCommand local. (0x420d40 es Cmd_AddServerCommand: esos solo corren dentro del frame
// del servidor, que en el que se une esta congelado.)
inline auto Cmd_AddCommand = (void(__cdecl*)(const char*, void (*)()))A(0x4213d0, 0x422860);
inline auto Z_Malloc = (void*(__cdecl*)(int))A(0x42a480, 0x42b8e0);
inline auto Cvar_FindVar = (uint8_t*(__cdecl*)(const char*))A(0x431920, 0x433700);  // cvar_t: +4 string, +0x20 integer
inline auto Cbuf_AddText_ = (void*)A(0x4209d0, 0x421ed0);                            // texto en EAX
inline auto SV_SetConfigstring = (void(__cdecl*)(int, const char*))A(0x44b900, 0x44db10);
inline auto CL_ApplyConfigstrings = (void(__cdecl*)(uint16_t*))A(0x40fae0, 0x410cb0);
inline auto CL_NetchanSetup = (void(__cdecl*)(netadr_t))A(0x40de40, 0x40ef70);

inline void Cvar_Set(const char* name, const char* value)  // EAX = 1 (forzar), cdecl (nombre, valor)
{
    int force = 1;
    asm volatile("push %2\n\tpush %1\n\tcall *%3\n\tadd $8, %%esp"
                 : "+a"(force)
                 : "r"(name), "r"(value), "r"((void*)A(0x431cf0, 0x433a60))
                 : "ecx", "edx", "memory");
}

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
// SV_SendServerCommand: ECX = client_t (0 = todos), cdecl (fmt, ...); va por el canal confiable.
// En el cgame (CG_ServerCommand 0x300206d0): "cp \"texto\"" = cartel al centro (\\n corta linea), "gmb" = negrita.
inline void SV_SendServerCommand(uint8_t* cl, const char* text)
{
    asm volatile("push %2\n\tpush %1\n\tcall *%3\n\tadd $8, %%esp"
                 : "+c"(cl)
                 : "r"("%s"), "r"(text), "r"((void*)A(0x44caf0, 0x44ed80))
                 : "eax", "edx", "memory");
}
inline void SV_SendClientSnapshot(uint8_t* cl)  // client en ECX
{
    asm volatile("call *%1" : "+c"(cl) : "r"((void*)A(0x44d960, 0x44fc10)) : "eax", "edx", "memory");
}
// UO: SVC_DirectConnect reserva los buffers del cliente con 0x44dd90 (EDI = client_t, cdecl cantidad = 0x100)
inline void SV_AllocClientBuffers(uint8_t* cl)
{
    asm volatile("push $0x100\n\tcall *%1\n\tadd $4, %%esp" : "+D"(cl) : "r"((void*)0x44dd90) : "eax", "ecx", "edx", "memory");
}
// --- DObj (modelo + animacion) y XAnim; ver notes/ARQUITECTURA.md "Tramo 2". UO: DObj de 0x5c (+0x18 mascara
// nueva, el resto corrido 4), arbol con un contador en +4 (infos desde +0xc) y entradas de modelo de 16 bytes ---
inline const int DOBJ_SIZE = (int)A(0x58, 0x5c);
inline const int DO_MODELS = (int)A(0x18, 0x1c), DO_FLAGS = (int)A(0x38, 0x3c), DO_PARENTS = (int)A(0x48, 0x4c),
                 DO_ORDER = (int)A(0x50, 0x54);  // +0x12 u16, +0x16 cantidad de modelos: iguales
inline const int TREE_INFO = (int)A(8, 0xc);     // arbol: +0 XAnim*, u16[cantidad] indices de XAnimInfo
inline int16_t* serverObjMap = (int16_t*)A(0x886f10, 0x894550);  // [MAX_GENTITIES] indice de DObj del servidor
inline int16_t* clientObjMap = (int16_t*)A(0x89fc88, 0x8ae2c8);
inline uint8_t* dobjPool = (uint8_t*)A(0x888020, 0x895660);      // DObj: +0 arbol, XModel*[8], u16[8], hueso padre[8]
inline uint8_t** xanimTable = (uint8_t**)A(0x8d84c8, 0x8f6bc0);  // [2][128] XAnim* (nombre en +0, cantidad en +4)
inline uint8_t* xanimInfo = (uint8_t*)A(0xa6d000, 0xa8b6f8);     // pool XAnimInfo 0x44 c/u; estado cliente +0x1c..+0x37
inline auto Com_ServerDObjCreate = (void(__cdecl*)(void* models, int num, void* tree, int ent, int u12))A(0x431030, 0x432e60);
inline auto Com_SyncClientDObjs = (void(__cdecl*)())A(0x431150, 0x432f80);  // serverObjMap -> clientObjMap
inline uint8_t* XAnimAllocInfo(uint8_t* tree, int anim)  // devuelve info+0x10. CoD1 cdecl (arbol, anim); UO EDX = arbol
{
    if (!IsUO()) return ((uint8_t*(__cdecl*)(void*, int))0x47d2d0)(tree, anim);
    uint8_t* r;
    asm volatile("push %2\n\tcall *%3\n\tadd $4, %%esp" : "=a"(r), "+d"(tree) : "r"(anim), "r"((void*)0x481df0) : "ecx", "memory");
    return r;
}
inline void Com_ServerDObjFree(int ent)  // ECX = ent
{
    asm volatile("call *%1" : "+c"(ent) : "r"((void*)A(0x4310f0, 0x432f20)) : "eax", "edx", "memory");
}
inline uint8_t* XAnimCreateTree(uint8_t* xanim)  // EBX = XAnim*
{
    uint8_t* r;
    asm volatile("call *%2" : "=a"(r), "+b"(xanim) : "r"((void*)A(0x431400, 0x4331f0)) : "ecx", "edx", "memory");
    return r;
}
// Bloque de memoria de un arbol (lo que libera XAnimFreeTree): indice y bytes. UO lo pide sin tipo (CoD1 con tipo 5,
// que el motor suelta al cambiar de nivel), asi que en UO hay que liberarlo a mano.
constexpr uintptr_t UO_TREE_BASE = 0x8f7400;
inline int TreeBytes(const uint8_t* xanim) { int n = *(const int*)(xanim + 4); return n * 2 + 0xc + (n * 3 + 1) * 2; }
inline void UO_FreeTreeBlock(int index, int bytes)  // MT_FreeIndex: ECX = indice, EAX = bytes
{
    asm volatile("call *%2" : "+c"(index), "+a"(bytes) : "r"((void*)0x45fd30) : "edx", "memory");
}
inline void XAnimFreeTree(uint8_t* tree)  // ECX = arbol
{
    asm volatile("call *%1" : "+c"(tree) : "r"((void*)A(0x431440, 0x433230)) : "eax", "edx", "memory");
}
// DI = indice del info (no limpia el lugar en el arbol). UO ademas EBX = arbol, y descuenta su contador (+4).
inline void XAnimFreeInfo(uint8_t* tree, int index)
{
    asm volatile("call *%2" : "+D"(index), "+b"(tree) : "r"((void*)A(0x47a310, 0x47ecb0)) : "eax", "ecx", "edx", "memory");
}
inline uint8_t* XModelPrecache(const char* name)  // ECX = nombre sin "xmodel/", (0, alloc, 0)
{
    uint8_t* r;
    asm volatile("push $0\n\tpush %3\n\tpush $0\n\tcall *%2\n\tadd $12, %%esp"
                 : "=a"(r), "+c"(name)
                 : "r"((void*)A(0x4802f0, 0x484cc0)), "r"((void*)A(0x431380, 0x433170))
                 : "edx", "memory");
    return r;
}
inline void XAnimCloneServerToClient(uint8_t* tree)  // (nodo 0) con el arbol en un global
{
    *(uint8_t**)A(0xa6d008, 0xa8b700) = tree;
    asm volatile("push $0\n\tcall *%0\n\tadd $4, %%esp" : : "r"((void*)A(0x47b010, 0x47fa80)) : "eax", "ecx", "edx", "memory");
}
inline uint8_t* ServerDObj(int ent) { return serverObjMap[ent] ? dobjPool + serverObjMap[ent] * DOBJ_SIZE : nullptr; }

inline uint8_t* Client(int n) { return svs_clients + n * CLIENT_SIZE; }
inline uint8_t* Entity(int n) { return sv_gentities + n * sv_gentitySize; }
inline uint8_t* PlayerState(int n) { return sv_gameClients ? sv_gameClients + n * sv_gameClientSize : nullptr; }
// VM_Call (EAX = vm, 13 args): fija currentVM, que usan los traps del modulo de juego.
extern "C" int CallVM(void* vm, const int* args13);
inline int GameCall(int cmd, int a = 0, int b = 0, int c = 0)
{
    int args[13] = {cmd, a, b, c};
    inGameCall = 1;
    return CallVM(gvm, args);
}
}  // namespace g
