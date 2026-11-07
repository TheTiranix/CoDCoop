# Arma game2/main/zz_coop2.iwd (Call of Duty 2): ui/main.menu del juego + el boton "Co-op" y su ventana
# (ui/cod2_coop_*.menu). El original sale de iw_06.iwd y no se toca; en el repo quedan solo los agregados.
# Uso: python -I tools/make_ui2.py [carpeta main de CoD2]
import os, re, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
main_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'game2', 'main')
read = lambda name: open(os.path.join(root, 'ui', name), encoding='utf-8').read()

with zipfile.ZipFile(os.path.join(main_dir, 'iw_06.iwd')) as z:
    lines = z.read('ui/main.menu').decode('latin-1').split('\n')

# en el menu "main_text" (hasta el siguiente menuDef) lo que esta de Multijugador para abajo baja 30 px
start = next(i for i, l in enumerate(lines) if re.match(r'\s*name\s+main_text\s*$', l))
end = next(i for i in range(start + 1, len(lines)) if re.match(r'\s*menuDef\b', lines[i]))
move = {300: 331, 331: 360, 360: 389}
for i in range(start, end):
    lines[i] = re.sub(r'(rect\s+MAIN_RECT_X\s+)(\d+)', lambda m: m.group(1) + str(move.get(int(m.group(2)), int(m.group(2)))),
                      lines[i])
# el boton va antes del itemDef "playselection" (Multijugador)
sel = next(i for i in range(start, end) if re.match(r'\s*name\s+playselection\s*$', lines[i]))
item = max(i for i in range(start, sel) if re.match(r'\s*itemDef\b', lines[i]))
lines[item:item] = read('cod2_coop_button.menu').rstrip('\n').split('\n')
# la ventana, antes de la llave que cierra el archivo
close = max(i for i, l in enumerate(lines) if l.strip() == '}')
lines[close:close] = read('cod2_coop_popmenu.menu').rstrip('\n').split('\n')

out = os.path.join(main_dir, 'zz_coop2.iwd')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('ui/main.menu', '\n'.join(lines).encode('latin-1'))
print('ok:', out)
