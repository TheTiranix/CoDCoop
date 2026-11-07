# Alinea dos funciones equivalentes (viejo/nuevo) por la forma de las instrucciones (difflib) y muestra las que
# cambian solo en los numeros (offsets de estructuras, direcciones).
# Uso: python pair.py <viejo.pe> <nuevo.pe> <va_viejo> <va_nuevo> [n=200] [-a = todas las alineadas]
import difflib, re, sys
exec(open(__file__.replace('pair.py', 'port.py')).read().split('def main')[0])
args = [a for a in sys.argv[1:] if a != '-a']
everything = '-a' in sys.argv
old, new = load(args[0]), load(args[1])
a, b = int(args[2], 16), int(args[3], 16)
if a < old['base']: a += old['base']; b += new['base']
n = int(args[4]) if len(args) > 4 else 200
def ins(pe, va):
    o = va - pe['tva']
    out = []
    for i in cs.disasm(pe['text'][o:o + n * 15], va):
        out.append(i)
        if len(out) >= n or (i.mnemonic == 'int3' and len(out) > 3): break
    return out
num = re.compile(r'0x[0-9a-f]+|\b\d+\b')
x, y = ins(old, a), ins(new, b)
sx = [f'{i.mnemonic} {i.op_str}' for i in x]
sy = [f'{i.mnemonic} {i.op_str}' for i in y]
nx = [num.sub('N', s) for s in sx]
ny = [num.sub('N', s) for s in sy]
for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, nx, ny, autojunk=False).get_opcodes():
    if tag != 'equal': continue
    for i, j in zip(range(i1, i2), range(j1, j2)):
        if everything or sx[i] != sy[j]:
            print(f'{x[i].address:#x} {sx[i]:<46} {y[j].address:#x} {sy[j]}')
