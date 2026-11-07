# Busca a mano lo que port.py no encontro: compara la secuencia de instrucciones (mnemonico + registros, sin
# numeros) de <viejo> en va con cada byte de una ventana del nuevo alrededor de una va estimada.
# Uso: python near.py <viejo.pe> <nuevo.pe> <va> <va_estimada_en_nuevo> [ventana=0x800]
import re, sys
exec(open(__file__.replace('near.py', 'port.py')).read().split('def main')[0])
old, new = load(sys.argv[1]), load(sys.argv[2])
va, guess = int(sys.argv[3], 16), int(sys.argv[4], 16)
win = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x800
if va < old['base']: va += old['base']; guess += new['base']
num = re.compile(r'0x[0-9a-f]+|\b\d+\b')
def seq(pe, a, k=16):
    o = a - pe['tva']
    return [ins.mnemonic + ' ' + num.sub('N', ins.op_str) for ins in cs.disasm(pe['text'][o:o + k * 15], a)][:k]
ref = seq(old, va)
best = []
for c in range(guess - win, guess + win):
    s = seq(new, c)
    score = sum(1 for x, y in zip(ref, s) if x == y)
    best.append((score, c))
best.sort(reverse=True)
print(hex(va), 'mejores:', ' '.join(f'{c:#x}={s}/{len(ref)}' for s, c in best[:4]))
