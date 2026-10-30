# Lee memoria de un juego abierto.  python -I mem.py <pid> <expr>...
#   expr: dir hex (dword), "s:<dir>" (texto), "cv:<dir>" (puntero a cvar_t -> nombre, string, integer)
import ctypes, struct, sys

k32 = ctypes.windll.kernel32
h = k32.OpenProcess(0x10, False, int(sys.argv[1]))

def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw

def u32(addr):
    b = read(addr, 4)
    return struct.unpack('<I', b)[0] if b else None

def text(addr, n=64):
    b = read(addr, n) or b''
    return b.split(b'\0')[0].decode('latin-1')

for e in sys.argv[2:]:
    if e.startswith('s:'):
        print(e, repr(text(int(e[2:], 16))))
    elif e.startswith('cv:'):
        cv = u32(int(e[3:], 16))
        print(e, f'cvar {cv:#x}' if cv else 'cvar 0', repr(text(u32(cv))) if cv else '', repr(text(u32(cv + 4))) if cv else '',
              u32(cv + 0x20) if cv else '')
    else:
        v = u32(int(e, 16))
        print(e, f'{v:#010x}' if v is not None else 'ilegible', v)
