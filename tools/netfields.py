# Compara las tablas de campos de red (nombre, offset) de dos exes: dan los offsets de playerState/entityState.
# Uso: python netfields.py <viejo.exe> <nuevo.exe> <campo_de_la_tabla>
import struct, sys
def load(f):
    d = open(f, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]; n = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(n):
        o = pe + 24 + opt + i * 40; vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8); secs.append((0x400000 + va, rs, ro))
    v2o = lambda v: next((ro + v - va for va, rs, ro in secs if va <= v < va + rs), None)
    o2v = lambda o: next((va + o - ro for va, rs, ro in secs if ro <= o < ro + rs), None)
    return d, v2o, o2v
def name(d, v2o, p):
    o = v2o(p) if 0x400000 < p < 0x1000000 else None
    if o is None: return None
    s = d[o:d.find(b'\0', o)]
    return s.decode() if 0 < len(s) < 40 and all(32 <= c < 127 for c in s) else None
def table(f, field):
    d, v2o, o2v = load(f)
    sva = o2v(d.find(b'\0' + field.encode() + b'\0') + 1)
    j = d.find(struct.pack('<I', sva))
    s = j
    while name(d, v2o, struct.unpack_from('<I', d, s - 12)[0]): s -= 12
    fl = {}
    while (nm := name(d, v2o, struct.unpack_from('<I', d, s)[0])):
        fl[nm] = struct.unpack_from('<I', d, s + 4)[0]; s += 12
    return fl
a, b = table(sys.argv[1], sys.argv[3]), table(sys.argv[2], sys.argv[3])
for k in sorted(a, key=a.get):
    print(f'{k:28} {a[k]:#7x} {b.get(k, -1):#7x}' + ('' if a[k] == b.get(k) else '  *'))
print('solo en el nuevo:', ', '.join(f'{k}={b[k]:#x}' for k in b if k not in a))
