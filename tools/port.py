# Busca en otro binario (CoDUOSP.exe, uo_gamex86.dll...) las direcciones que el mod usa en el de CoD1.
# Uso: python -I port.py <viejo.pe> <nuevo.pe> <va|rva> [...]
# Codigo: firma de bytes desde la direccion, sin las direcciones absolutas ni los saltos (se agranda hasta que
# sea unica). Datos: cada instruccion del viejo que los nombra se busca con su firma y se lee el valor en el
# nuevo; gana el valor con mas votos. Imprime viejo -> nuevo (votos) o "?".
import re, struct, sys
from collections import Counter
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode()
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        secs.append((name, base + va, max(vs, rs), ro, rs))
    t = [s for s in secs if s[0] == '.text'][0]
    return dict(d=d, base=base, secs=secs, tva=t[1], text=d[t[3]:t[3] + t[4]])

cs = Cs(CS_ARCH_X86, CS_MODE_32)
cs.detail = True

def masked(pe, va, nbytes):
    """Patron regex de bytes desde va, con comodines en direcciones y desplazamientos de saltos."""
    off = va - pe['tva']
    code = pe['text'][off:off + nbytes + 16]
    pat, used = [], 0
    for ins in cs.disasm(code, va):
        b = list(ins.bytes)
        mask = [False] * len(b)
        enc = ins.encoding if hasattr(ins, 'encoding') else None
        for o, sz in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
            if o and sz == 4:
                v = struct.unpack_from('<I', ins.bytes, o)[0]
                if v >= 0x10000 and v < 0xFFFF0000 or ins.group(1) or ins.group(2) or ins.group(7):
                    mask[o:o + 4] = [True] * 4
            elif o and sz == 1 and (ins.group(1) or ins.group(7)):  # jcc/jmp corto: puede cambiar
                mask[o] = True
        pat.append(b''.join(b'.' if m else re.escape(bytes([x])) for x, m in zip(b, mask)))
        used += len(b)
        if used >= nbytes:
            break
    return b''.join(pat), used

def find_code(old, new, va, start=None):
    """Busca en new la firma de old en va. start = va del comienzo de la firma (para direcciones a mitad)."""
    s = start or va
    for n in (16, 24, 32, 48, 64, 96, 128):
        pat, used = masked(old, s, n)
        hits = [m.start() for m in re.finditer(pat, new['text'], re.S)]
        if len(hits) == 1:
            return new['tva'] + hits[0] + (va - s)
        if not hits:
            return None
    return None

def ins_containing(pe, p):
    """Instruccion del viejo que contiene el byte en va p (como inmediato o desplazamiento)."""
    for k in range(1, 12):
        st = p - k
        off = st - pe['tva']
        for ins in cs.disasm(pe['text'][off:off + 16], st):
            if (ins.disp_offset == k and ins.disp_size == 4) or (ins.imm_offset == k and ins.imm_size == 4):
                return st
            break
    return None

def find_data(old, new, va):
    """Vota el valor nuevo de una direccion de datos (o de codigo llamado/referido) por las referencias."""
    votes = Counter()
    pat = struct.pack('<I', va)
    t = old['text']
    i = t.find(pat)
    while i != -1 and sum(votes.values()) < 40:
        p = old['tva'] + i
        st = ins_containing(old, p)
        if st is not None:
            for back in (0, 1):  # firma desde la instruccion; si no es unica, desde unas antes
                r = find_code(old, new, p, st)
                if r is not None:
                    votes[struct.unpack_from('<I', new['text'], r - new['tva'])[0]] += 1
                    break
        i = t.find(pat, i + 1)
    # llamadas relativas (call rel32) al codigo
    for m in re.finditer(rb'\xE8', t):
        if sum(votes.values()) >= 40:
            break
        j = m.start()
        if j + 5 <= len(t) and old['tva'] + j + 5 + struct.unpack_from('<i', t, j + 1)[0] == va:
            r = find_code(old, new, old['tva'] + j)
            if r is not None:
                o = r - new['tva']
                votes[r + 5 + struct.unpack_from('<i', new['text'], o + 1)[0] & 0xFFFFFFFF] += 1
    return votes

def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    for a in sys.argv[3:]:
        va = int(a, 16)
        rva = va < old['base']
        if rva:
            va += old['base']
        intext = old['tva'] <= va < old['tva'] + len(old['text'])
        direct = find_code(old, new, va) if intext else None
        votes = find_data(old, new, va)
        best = votes.most_common(3)
        out = lambda v: hex(v - new['base']) if rva else hex(v)
        res = out(direct) if direct is not None else '?'
        vtxt = ' '.join(f'{out(v)}*{c}' for v, c in best)
        flag = '' if direct is None or not best or best[0][0] == direct else '  DISTINTO'
        print(f'{a:>12} code={res:<12} refs: {vtxt or "-"}{flag}')

main()
