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
