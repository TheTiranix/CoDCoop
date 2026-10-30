# CoD1 coop online (campaña, hasta 4 jugadores)

Juego: `D:\Call of Duty Deluxe Edition` (CoDSP.exe 1.5, mar-2004, sin packer, base 0x400000).
Copia de desarrollo: `D:\CoDCoop\game` (pk3 con hardlink, el resto copiado; el juego original no se toca).
Mod: `ddraw.dll` proxy (el exe importa solo DirectDrawEnumerateExA y DirectDrawCreateEx). Log: `game\codcoop.log`.
Ghidra: proyecto `D:\CoDCoop\ghidra\CoD` con CoDSP.exe, gamex86.dll (base 0x20000000), cgamex86.dll (base 0x30000000).
`tools\gh.ps1 <programa> Decompile|XrefsTo <salida> <hex>...`, `tools\strref.py <pe> <texto>...` (xrefs de strings).

## Arquitectura elegida: servidor autoritativo del host + clientes remotos del motor

El SP de CoD1 es Quake 3 por dentro: servidor y cliente en el mismo proceso hablando por loopback.
El host juega la campaña normal (IA, scripts, guardados corren solo ahí); los demás se conectan como
clientes extra del servidor del host. Descartado: espejar NPCs como en Scarface (la IA se desincroniza)
y portar la campaña al MP (el MP no tiene IA).

## Hechos verificados (decompilado)

Servidor (MAX_CLIENTS = 4 ya en el motor):
- `svs.clients` = `*(0x152e8e8)`, 4 x client_t de 0xb87c bytes (SV_Startup 0x44be90 reserva 0x2e1f0).
- client_t: +0 state (1 = activo), +0x18/+0x1c reliableSequence/Acknowledge, +0x2c gamestateMessageNum,
  +0x30 lastUsercmd (11 dwords), +0x464 gentity, +0x46c frames/ps, +0x784c netchan
  (+0 sock, +4 remoteAddress netadr_t 20 bytes, port en +0x7866, +0x7868 qport, +0x7870 outgoingSequence).
- netadr_t: {type, ip[4], ipx[10], port(short, big endian)}; tipos 2=loopback, 4=IP, 5=IPX.
- SV_ReadPackets/PacketEvent 0x44ccb0 ya recorre los 4 clientes por direccion+qport.
- SV_GetUsercmd 0x449a10, SV_SendServerCommand 0x44caf0 y SV_SetConfigstring 0x44b900 ya recorren 4.
- SVC_DirectConnect 0x448d70: SIEMPRE usa el slot 0 (memset + Netchan_Setup + buffers). Lo llaman
  SV_SpawnServer 0x44c1b0 y SV_MapRestart/LoadGame 0x4482a0 con direccion loopback. No hay handshake:
  el handler de paquetes sin conexion 0x44cc00 solo entiende "disconnect".
- SV_ClientEnterWorld 0x448e40: VM_Call(2 ClientConnect) / gentity = sv.gentities + n*size / VM_Call(3 ClientBegin).
- SV_SendClientSnapshot 0x44d960 (ECX = client): construye y manda por Netchan_Transmit 0x43ed50.
- VM_Call = 0x44f270 (EAX = vm). gvm = `*(0x13f9db8)`, cgvm = `*(0x14bd620)`.
- SV_GameSystemCalls 0x44a2f0, CL_CgameSystemCalls 0x401a50.

Red:
- NET_SendPacket 0x43f290 (ECX sock, EAX data, EBX len, netadr por valor en la pila): SOLO loopback,
  no mira el tipo -> enganchar ahi para mandar UDP cuando to.type == 4.
- NET_GetLoopPacket 0x43f1c0 (EAX sock, EBX msg_t*, arg from*): lo usa Com_EventLoop 0x42f180 para
  cliente (sock 0 -> CL_PacketEvent 0x40dec0) y servidor (sock 1 -> SV si sv_running). Enganchar ahi
  para devolver paquetes UDP cuando la cola loopback esta vacia.
- El exe NO importa winsock: lo ponemos nosotros (ws2_32 en la DLL).
- CL: clc.netchan en 0x14b95d0 (remoteAddress 0x14b95d8, qport 0x14b95ec), cls.state 0x1439260,
  servername 0x1439270 ("localhost"), serverAddress 0x14394a4. Netchan_Setup del cliente 0x40de40.

Atajos del SP (lo que un cliente remoto NO tiene y hay que mandar aparte):
1. Gamestate: SV_SpawnServer llama directo a 0x40fae0(sv.configstrings) (configstrings como handles
   del pool de strings compartido 0x8d8bf8). Las actualizaciones sí viajan como comando "cs %i %s".
2. Estado de entidades: el snapshot solo trae ps + lista de numeros de entidad (+0x73e8 cuenta,
   +0x73ec lista). cgame copia el entityState (0x27 dwords) desde la MEMORIA DEL SERVIDOR con el
   trap 0xf8 = sv.gentities(0x152bc58) + n * gentitySize(0x152bc5c).
3. DObj (modelo + animacion): pool compartido 0x888020 (0x58 por DObj, 2 bits de uso por DObj en
   0x887f10: cliente/servidor). serverObjMap 0x886f10[2048], clientObjMap 0x89fc88[2048].
   0x431150 copia serverObjMap -> clientObjMap: el cliente dibuja con el MISMO DObj que anima el
   servidor. Com_ServerDObjCreate 0x431030, Com_ClientDObjCreate 0x430fd0, crear 0x476e00.
4. Traps de cgame 0xdf-0xe5 llaman al modulo de juego del servidor (protegidos con gvm != 0; en un
   cliente sin servidor devuelven 0). Son brujula y nodos de depuracion; 0xdf = info de actor.

Modulo de juego (gamex86.dll): vmMain 0x2001cd70. 0 Init, 1 Shutdown, 2 ClientConnect(n) 0x20014800,
3 ClientBegin 0x200148d0, 4 ClientDisconnect 0x20014ca0, 5 ClientCommand 0x20017ff0, 6 ClientThink
0x200129c0, 7 RunFrame(?). level.clients = `*(0x202db220)` (gclient 0x74e8), g_entities 0x2012fa80
(gentity 0x2c8). ClientConnect sirve para cualquier n y crea un "sentient" para que la IA lo vea.

## Hechos aprendidos en las pruebas (tramo 1)
- VM_Call 0x44f270 (EAX = vm, 13 args) fija currentVM (0x13f9db4); los traps del modulo pasan por
  0x44f040 que lo usa. Llamar a vmMain directo crashea (0x44f04b). Usar CallVM (main.cpp).
- gamex86.dll carga REUBICADA (vista en 0x21bb0000): nunca usar direcciones absolutas 0x20xxxxxx;
  usar base del modulo + RVA o los punteros que el motor guarda (sv.gentities, sv.gameClients 0x152bc64,
  sv.gameClientSize 0x152bc68).
- El cliente del SP toma su reloj de sv.time (CL_Frame 0x40df80: cl.serverTime = *(0x152e930)). En el
  que se une el servidor esta congelado: Coop_FrozenFrame mueve sv.time detras del ultimo snapshot.
- ClientSpawn (gamex86 RVA 0x14900) borra el gclient y no repone ps.clientNum (ps+0x9c): queda 0 y
  Pmove choca con la propia entidad (allsolid: ni gravedad ni movimiento). FixClientNum lo repone.
- Tras cargar un mapa el juego queda en pausa con "Haz clic para iniciar" (0x4103e0: menu de UI abierto
  -> cl_paused 1). En pruebas: `tools\run.ps1 click`.
- Si el juego se cierra mal deja `__CoDSP` junto al exe y pregunta "Modo Seguro" al abrir: borrarlo.
- playerState: +0 commandTime, +4 pm_type, +0xc pm_flags, +0x14 origin, +0x20 velocity,
  +0x3c gravity, +0x44 speed, +0x54 groundEntityNum, +0x9c clientNum, +0xa0 weapon, +0xe4 health.
- usercmd (client_t+0x30, 11 dwords): +0 serverTime, +4 buttons, +0x14 forwardmove (byte).

## Recarga en caliente (fase 0)
- ddraw.dll (src/loader.cpp) = cargador FIJO: proxy, ganchos, registro de crashes, comandos.
  codcoop_core.dll (src/core.cpp) = logica. build.bat copia el nucleo a game\; los juegos abiertos lo
  detectan (archivo nuevo y quieto 1 s) y lo recargan solos (`coopreload` en su consola). Cargan una
  COPIA en game\codcoop_tmp para que el build pueda pisar el original.
- Contrato en src/api.h. Todo lo que debe sobrevivir va en `State` (core.cpp); la memoria la guarda el
  cargador. Si cambia sizeof(State) o STATE_VERSION, se pone en cero (hay que volver a conectar).
- Comandos: el nucleo da la lista (Coop_Commands) y el cargador los registra con un despachador fijo.
  Un comando NUEVO aparece en la consola recien al recargar.
- Cambios en loader.cpp o game.h que use el cargador SI requieren reiniciar los juegos.
- Pruebas: `coopgod <n> [0|1]`, `cooplook <n> <ent>`, `coopdobj`, `coopbring <n> [x y z]`,
  `tools\run.ps1 shotj <png>` (ventana del que se une, con DPI real).

## Trampas encontradas en el tramo 2
- 0x420d40 es Cmd_AddServerCommand (solo corre dentro del frame del servidor). Cmd_AddCommand = 0x4213d0.
- 0x4254b0 es busqueda de ARCHIVOS ("%s/%s%s", extension en EBX). XModel por nombre = XModelPrecache
  0x4802f0 (ECX = nombre sin "xmodel/", pila (0, 0x431380, 0)).
- Al liberar un DObj con arbol propio: Com_ServerDObjFree, Com_SyncClientDObjs y RECIEN DESPUES liberar
  el arbol (DObjFree 0x477020 lee dobj->tree->anims).
- Com_SyncClientDObjs (0x431150) lo dispara un trap del juego: en el que se une hay que llamarlo a mano.

## Cuerpos de los jugadores (fase 1, tareas 1.2 y 1.3)
- El host crea un DObj de paracaidista (cuerpo + cabeza + casco) en la entidad de cada jugador cuando hay
  algun remoto. El cgame no dibuja eType 1 (jugador): se engancha cgvm->systemCall (primer campo del vm_t,
  `*(0x14bd620)`, original 0x401a50) y el trap 0xf8 devuelve para los OTROS jugadores una copia con eType 0
  (general, dibuja el DObj) y sin pitch. Se rearma cada frame (el motor recrea el cgame) y se saca al descargar.
- Cada modelo de un DObj lleva en +0x38 `-(indice de modelo)` (configstrings CS_MODELS = 31, 256 modelos): sin
  eso el renderer no lo dibuja. Com_ServerDObjCreate resuelve solo el hueso padre si se le pasa el tag.
- entityState: +0 eType (byte), +6 number, +0x10 eFlags, +0x14 pos (trType 3 = LINEAR_STOP, +0x20 trBase),
  +0x38 apos (+0x44 pitch, +0x48 yaw). cg_entities 0x3012adc0 (0x178 c/u, +0x150 lerpOrigin).
- XAnimInfo (pool 0xa6d000, 0x44 c/u): cliente +0x1c tiempo, +0x24 ciclo, +0x28 goalTime, +0x2c goalWeight,
  +0x30 weight, +0x34 rate; servidor +0x38 tiempo nuevo, +0x3c tiempo confirmado, +0x40/+0x42 ciclos,
  +0x44.. igual que el cliente. inGameCall (0xa8f648) elige el lado.
- TRAMPA: el motor saltea las entidades 0..3 en la copia servidor->cliente por snapshot (0x44d8e0 empieza en
  serverObjMap+8) y en la confirmacion del tiempo: para los cuerpos se hace a mano cada frame
  (0x47b010 con el arbol en 0xa6d008; +0x3c = +0x38).
- Animar: 0x47d8b0 (ECX arbol, EAX anim; goalWeight, blendTime, rate, 0, 0, 0) y 0x47ccd0 (ECX DObj; dtime,
  notify) con inGameCall = 1. Sin mezcla: se apagan todos los nodos y se enciende la cadena nueva.
- Postura: ps.pm_flags (+0xc) 1 = cuerpo a tierra, 2 = agachado. Comando `coopanims <texto>` lista las hojas.
- Arma en la mano: ps.weapon (+0xa0) -> WeaponDef* (tabla `*(gamex86 + 0x125650)`) -> worldModel (+0x188,
  texto "xmodel/..."), unida con tag_weapon_right. Si cambia el arma se rearma el DObj.
- Disparo: eFlags 0x400 mientras dispara. ps.ammoclip[64] (+0x1f4) esta indexado por cargador, no por arma.
- G_Damage = gamex86 RVA 0x18800 (targ, inflictor, attacker, dir, point, damage, dflags, mod, hitLoc);
  gentity +0x104 gclient, +0x108 actor, +0x129 takedamage. Enganchado (prologo de 8 bytes) para la opcion
  `coop_friendlyfire` (cvar del host, 0 por defecto). Se desengancha al descargar el nucleo.
## Caido, revivir y checkpoint (fase 2)
- G_Damage enganchado: con coop activo el daño a un jugador se aplica con la vida inflada (+100000) para medir
  el daño real (escala por zona) sin que muera adentro. Letal con otro en pie -> vida 1 + caido (flags |= 2
  notarget). Letal sin nadie en pie -> muere el HOST de verdad (mision fallida del SP). Un remoto nunca muere.
- Caido: SV_GetUsercmd (0x449a10, EAX cliente) y CL_GetUserCmd (0x401050, prediccion local, EAX numero) se
  filtran: sin movimiento ni botones, cuerpo a tierra. usercmd +4 botones (1 disparar, 0x40 usar, 0x4000
  cuerpo a tierra, 0x8000 agachado), +0x14/+0x15/+0x16 adelante/costado/arriba. El host manda 'W' (mascara).
- TRAMPA GRANDE: ClientConnect (gamex86 RVA 0x14800) llama primero a la init de caminos de la IA (0x6fde0,
  call en RVA 0x14808). Llamarlo para un remoto a mitad del nivel deja las listas de enlaces desconectados de
  las puertas (gentity+0x2b6, nodos en *(gamex86+0x2dc1c0)) apuntando a nodos libres y la carga de partida
  se cuelga en G_FreeEntity. Para los remotos ese call se pisa con NOP durante el ClientConnect.
  `cooplists` y el log cada 5 s revisan esas listas.
- Mision fallida (g_reloading 1): el host libera cuerpos y suelta los slots remotos (si no: "Reliable command
  buffer overflow" al cargar) y les manda 'R'; no acepta uniones hasta que g_reloading vuelve a 0. El remoto
  libera sus DObj, recarga el mapa local (devmap) y hace cooprejoin: si siguiera con el cgame viejo cortaria
  con "CG_ProcessSnapshots: Server time went backwards" (el reloj del host retrocede).
- Carteles: el host le manda a cada jugador `cp "texto"` con SV_SendServerCommand (0x44caf0, ECX = client_t,
  0 = todos; cdecl fmt, ...). El cgame (CG_ServerCommand 0x300206d0) lo dibuja cg_centertime (3 s); `\n` (barra
  + n, literal) corta linea, Latin-1 y ^colores andan. Cartel fijo (caido, barra de levantar) se renueva cada
  2 s; aviso suelto (cayo / se levanto) una vez. cgame: texto en 0x30125820, hora en 0x30125814, cg.time
  0x3011e0a4, cgs.serverCommandSequence 0x3010b894. Las capitales con tilde salen sin tilde (fuente).
- Brazos en primera persona = ps+0xac viewmodelIndex (indice de modelo, 8 bits). El script del nivel hace
  setviewmodel solo al jugador 0; los remotos quedaban con viewmodel_hands_us (sin textura en el nivel -> blancos).
  El host copia el suyo a cada remoto antes de cada snapshot.
- Brujula (cgame 0x3000d380): aliados de la IA en una tabla de 32 lugares en cgame+0x12601c (16 bytes: hora
  cg.time, x, y, rumbo), uno por indice de actor (32 actores maximo); se dibuja si la hora tiene < 800 ms. Los
  llena el cgame con las entidades eType 11 del snapshot (trap 0xe0 dice si es aliado con drawoncompass) y
  ps+0x3b4 iCompassFriendInfo (uno lejano por frame, empaquetado). Tanques: tabla siguiente (cgame+0x12621c),
  eType 14, trap 0xe2, ps+0x3b8. Los jugadores van en el lugar 31 - n: el host los escribe con sus datos y a los
  remotos les manda x, y, rumbo de todos en el mensaje 'W'. Riesgo: un aliado de la IA con indice >= 28.
- Pausa del SP = cvar cl_paused (cvar_t* en 0x14e440c): SV_Frame (0x44cf10) no avanza y CL_SendCmd (0x40b3e0)
  no manda usercmds. SV_Frame mismo lo pone en 1 si la UI esta en pantalla completa (0x44d0a3; JNZ en 0x44d0a6).
  Con remotos el host cambia ese JNZ por JMP y repone cl_paused 0 (menu ESC y ventana minimizada no frenan a
  nadie). Cvar_Set = 0x431cf0 (EAX 1 = forzar, cdecl nombre, valor). UI: vm en 0x14bd648, vmMain 7 =
  SetActiveMenu (0 = cerrar todo), 8 = ultimo menu (no confiable).
- "Haz clic para iniciar" (ui/pregame.menu, boton -> uiScript playerstart = cl_paused 0, "fade 0 0 0 0 3",
  cg_norender 0, cerrar menus). Se reconoce por cl_paused 1 + cg_norender 1. El remoto lo hace solo al entrar.
  La carga de un checkpoint no muestra esa pantalla en el host (la carga de partida deja cl_paused 0).
- tools\mem.py <pid> <dir> | s:<dir> | cv:<dir>: lee memoria de un juego abierto. En PowerShell `rd` es borrar.

## Cambios de mapa (tarea 3.2)
- Fin de nivel del script: changelevel(map) / missionSuccess(map) (gamex86 0x200359b0 / 0x20035b00) guardan el
  mapa siguiente en gamex86+0x2dbd18 y piden salida (+0x2dbd00; +0x2dbd08 = con pantalla de victoria). El frame
  (0x2001e790) abre "victoryscreen" (su onOpen pone cl_paused 1; "Siguiente nivel" lo cierra) y despues
  g_reloading 2 + "spmap <mapa>". g_reloading = tipo de salida: 1 mision fallida, 4 nivel terminado, 2 cargando.
- SV_SpawnServer(mapa, x) = 0x44c1b0, unico llamador SV_Map_f en 0x448246 (call cambiado a Hook_SpawnServer):
  antes del servidor nuevo el host suelta a los remotos con M_RELOAD <mapa>. Ellos hacen devmap + cooprejoin.
- M_JOIN lleva el mapa del que entra; si no es el del host, el host contesta M_MAP <mapa> sin dar slot y el
  remoto lo carga y vuelve (sirve tambien para unirse desde cualquier mapa). Respuesta vieja (ya esta en ese
  mapa) se ignora: el host atiende pedidos en cola al terminar de cargar.
- Con fin de nivel en curso el host SI se pausa (pantalla de victoria); el remoto ve la misma pantalla
  (openvictoryscreen va a todos). Prueba: `coopendlevel <mapa>` en el host = missionSuccess.
- TRAMPA: a mitad de partida XModelPrecache (0x4802f0) corta el juego con "model '%s' not precached" si el nivel
  no cargo ese modelo. Los cuerpos usan solo modelos de CS_MODELS (precargados por el nivel): el preferido o el
  primer character_airborne*/british*/soviet* + basehead* (no axis) + casco US si es paracaidista.
- Pruebas: coophealth <n> <vida>, coopkill <n> [daño] (G_Damage del mundo con gvm como VM actual),
  coopcmd <n>, coopent <n>, cooplook <n> <ent> [dist] [pitch]; tools\stack.ps1 <pid> (pila de un proceso
  colgado), tools\peek.ps1 <pid> <dir> <bytes>; run.ps1 click <x> <y>.

- El inicio de pathfinder deja al jugador en z=2370 con pm_type 1 (paracaidas) unos segundos: no mover jugadores
  con coopbring/cooplook antes de que aterrice (quedan flotando fuera del mapa, sin cielo).

## Estado
- Tramo 1 FUNCIONA (2026-10-29, probado con 2 juegos en esta PC): el 2do jugador se conecta al slot 1,
  ve el mundo desde su posicion con los snapshots del host y camina (el host lo mueve con sus usercmds).
- Falta: arma para el jugador 2, su cuerpo visible, modelos/animaciones de la IA en el cliente remoto,
  pausa del host, cambios de mapa, internet.

## Plan por tramos (cada uno se prueba antes del siguiente)
1. Transporte UDP + conexion de un 2do cliente al slot 1 + snapshots + gamestate + estados de entidad.
   El que se une carga el mismo mapa con su propio servidor (para colision y assets), se congela su
   servidor y se redirige su cliente al host ("servidor sombra").
2. DObj/animaciones de IA en el cliente remoto (mandar modelos + estado del arbol de animacion).
3. Cuerpo visible de los jugadores remotos para el host y entre ellos.
4. Cambios de mapa, guardados/checkpoints, muerte/reaparicion, cinematicas.
5. Internet (puertos/NAT), lobby, 4 jugadores, recorrer la campaña.
