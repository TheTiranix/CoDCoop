# Arma game/Main/pakz_coop.pk3: ui/main.menu del juego + el boton "Cooperativo" y su ventana (ui/coop_*.menu).
# El original sale de localized_spanish_pak0.pk3 y no se toca; en el repo quedan solo los agregados.
# Uso: python -I tools/make_ui.py
import os, re, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
main_dir = os.path.join(root, 'game', 'Main')
with zipfile.ZipFile(os.path.join(main_dir, 'localized_spanish_pak0.pk3')) as z:
    menu = z.read('ui/main.menu').decode('latin-1')
read = lambda name: open(os.path.join(root, 'ui', name), encoding='utf-8').read()

# los botones de partida en curso miran ui_coop_paused y no cl_paused: con remotos el host no pausa (ver
# UiPausedFrame en src/core.cpp) y sin esto su menu mostraba "Continuar partida" en vez de "Volver a la partida"
menu, changed = re.subn(r'(cvarTest\s+)"cl_paused"', r'\1"ui_coop_paused"', menu, flags=re.I)
assert changed == 17, changed
lines = menu.split('\n')
# lugar para el boton: en el menu "main" (hasta el siguiente menuDef) la columna de abajo pasa de 35 a 30 px
end = next(i for i, l in enumerate(lines) if i > 30 and re.match(r'\s*menuDef\b', l))
move = {285: 280, 320: 340, 355: 370, 390: 400}
for i in range(end):
    lines[i] = re.sub(r'(rect\s+385\s+)(\d+)', lambda m: m.group(1) + str(move.get(int(m.group(2)), int(m.group(2)))),
                      lines[i])
# el boton va antes del itemDef "options" del menu "main"
opt = next(i for i, l in enumerate(lines) if re.match(r'\s*name\s+options\s*$', l))
item = max(i for i in range(opt) if re.match(r'\s*itemDef\b', lines[i]))
lines[item:item] = read('coop_button.menu').rstrip('\n').split('\n')
# la ventana, antes de la llave que cierra el archivo
close = max(i for i, l in enumerate(lines) if l.strip() == '}')
lines[close:close] = read('coop_popmenu.menu').rstrip('\n').split('\n')

out = os.path.join(main_dir, 'pakz_coop.pk3')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('ui/main.menu', '\n'.join(lines).encode('latin-1'))
print('ok:', out)
