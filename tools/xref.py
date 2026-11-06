# Sitios del .text que llaman/saltan a una VA o la nombran como inmediato. Uso: python xref.py <pe> <va>...
import re, struct, sys
exec(open(__file__.replace('xref.py', 'port.py')).read().split('def main')[0])
pe = load(sys.argv[1])
for a in sys.argv[2:]:
    va = int(a, 16); t = pe['text']; out = []
    for m in re.finditer(rb'[\xE8\xE9]', t):
        j = m.start()
        if j + 5 <= len(t) and (pe['tva'] + j + 5 + struct.unpack_from('<i', t, j + 1)[0]) & 0xFFFFFFFF == va:
            out.append(hex(pe['tva'] + j))
    i = t.find(struct.pack('<I', va))
    while i != -1:
        out.append('imm@' + hex(pe['tva'] + i)); i = t.find(struct.pack('<I', va), i + 1)
    print(a, ' '.join(out[:20]) or '-')
