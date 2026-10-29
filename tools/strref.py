# Busca strings en un PE y las instrucciones que las referencian (imm32 = VA del string).
# Uso: python -I strref.py <pe> <texto> [<texto>...]
import struct, sys

def sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode()
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        out.append((name, base + va, vs, ro, rs))
    return base, out

def off2va(secs, off):
    for _, va, vs, ro, rs in secs:
        if ro <= off < ro + rs:
            return va + off - ro

def va2off(secs, v):
    for _, va, vs, ro, rs in secs:
        if va <= v < va + rs:
            return ro + v - va

d = open(sys.argv[1], 'rb').read()
base, secs = sections(d)
text = [s for s in secs if s[0] == '.text'][0]
tstart, tend = text[3], text[3] + text[4]
for s in sys.argv[2:]:
    b = s.encode()
    i = d.find(b)
    while i != -1:
        st = i - 1 if d[i - 1] == 0x15 else i  # los errores de Com_Error llevan un \x15 delante
        if d[st - 1] == 0 and d[i + len(b)] in (0, 10):  # string entera (puede terminar en \n)
            va = off2va(secs, st)
            pat = struct.pack('<I', va)
            refs = []
            j = d.find(pat, tstart, tend)
            while j != -1:
                refs.append(hex(off2va(secs, j)))
                j = d.find(pat, j + 1, tend)
            print(f'{s!r} @ {va:#x} refs: {" ".join(refs) or "-"}')
        i = d.find(b, i + 1)
