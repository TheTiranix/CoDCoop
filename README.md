# Call of Duty Coop

Jugá la campaña de **Call of Duty 1 (2003)** en cooperativo con amigos: hasta 4 jugadores, por Steam, por internet con IP o en red local.

**[⬇ Descargar el instalador](https://github.com/TheTiranix/CoDCoop/releases/latest)**

## Qué hace

- **Campaña original en compañía.** Uno es el anfitrión: la misión corre en su PC, con la IA, los scripts y los objetivos del juego. Los demás se unen a su partida y la juegan con él.
- **Caído, no muerto.** Si te matan quedás en el piso y un compañero te levanta manteniendo *Usar* 3 segundos. Si caen todos, se vuelve al último punto de control.
- **Misión tras misión.** Cuando el anfitrión pasa de nivel, los demás pasan con él solos.
- **Vehículos.** Los compañeros viajan con el anfitrión y pueden disparar desde el vehículo. En los tanques van montados con un arma de infantería.
- **Nadie se queda atrás.** El que queda muy lejos del anfitrión durante unos segundos aparece detrás de él.
- **Por Steam o por IP.** Con Steam no hace falta abrir puertos: tus amigos te ven en el menú y se unen con un clic.
- **Entrar y salir.** Si alguien se desconecta, su lugar se libera y puede volver a entrar.
- **En pantalla.** Los compañeros aparecen en la brújula, y hay avisos de quién está caído o lo están levantando.
- **Misma dificultad del juego.** El daño entre jugadores viene apagado y se activa en el menú Cooperativo.
- **Sin choques entre compañeros.** Los jugadores se pueden atravesar, así nadie queda trabado al aparecer juntos.

## Requisitos

- **Call of Duty 1 versión 1.5** para Windows, en todas las PCs que van a jugar.
- El mod instalado en todas esas PCs.
- El menú del mod está en español.

## Instalar

1. Bajá `Instalar-CoD-Coop.exe` de la [última versión](https://github.com/TheTiranix/CoDCoop/releases/latest).
2. Abrilo y aceptá la carpeta del juego (la que tiene `CoDSP.exe`). Si no la encuentra solo, te deja elegirla.
3. Si Windows muestra "Windows protegió su PC", tocá **Más información** → **Ejecutar de todas formas** (el instalador no está firmado).

El instalador copia `ddraw.dll`, `codcoop_core.dll`, `steam_api.dll`, `main\pakz_coop.pk3` y un `LEEME CoD Coop.txt` en la carpeta del juego. Si ya había un `ddraw.dll` o un `steam_api.dll` de otro programa, lo guarda con `.bak` al final del nombre.

## Jugar

1. Abrí el Call of Duty como siempre y entrá en **Cooperativo**, en el menú principal.
2. **Anfitrión:** tocá **Crear partida** y después elegí **Nueva Partida** o **Cargar Partida**.
3. **Los demás:** tocá **Unirse a <nombre>** (amigos de Steam) o escribí la IP del anfitrión y tocá **Unirse**.

En el mismo menú (también con la partida en curso, desde la pausa):
- **Daño entre jugadores: Sí/No** lo decide el anfitrión.
- **Desconectarse** saca de la partida cooperativa: el que se unió vuelve al menú principal; si lo toca el anfitrión, los demás vuelven al menú y él sigue jugando solo.

### Por Steam (sin abrir puertos)

- Todos con Steam abierto y el juego abierto.
- Cuando el anfitrión crea la partida, a sus amigos de Steam les aparece **Unirse a <nombre>** en el menú Cooperativo.
- También desde la lista de amigos de Steam: clic derecho en el anfitrión → **Unirse a la partida** (con el juego ya abierto).
- Mientras el juego está abierto, Steam muestra "Jugando a Spacewar": es normal, el coop usa esa app de prueba de Steam (AppID 480) para conectarse.

### Por internet con IP

- El anfitrión abre el puerto **UDP 28960** en su router (dirigido a su PC) y les pasa su IP pública.
- En la misma casa o red alcanza con la IP local del anfitrión (`ipconfig`).
- Sin poder abrir puertos: usen un programa de red virtual (por ejemplo Radmin VPN o ZeroTier) y la IP que ese programa le da al anfitrión.

### Comandos de consola

| Comando | Qué hace |
|---|---|
| `coophost [puerto]` | Crea la partida (puerto 28960 si no se indica) |
| `coopjoin <ip>[:puerto]` | Se une a la partida de esa IP |
| `coopjoin steam:<SteamID>` | Se une por Steam a la partida de ese jugador |
| `set coop_friendlyfire 1` | Activa el daño entre jugadores (en el anfitrión) |
| `coopleave` | Sale de la partida cooperativa (como el botón Desconectarse) |

## Desinstalar

Borrá de la carpeta del juego `ddraw.dll`, `codcoop_core.dll`, `steam_api.dll`, `main\pakz_coop.pk3` y la carpeta `codcoop_tmp`. Si hay archivos `.bak` (`ddraw.dll.bak`, `steam_api.dll.bak`), sacales el `.bak` del nombre.

## Cómo funciona

El modo historia de CoD1 es, por dentro, un servidor y un cliente de Quake 3 en el mismo proceso. El mod reemplaza `ddraw.dll` para engancharse al juego: el anfitrión corre la misión real y los demás se conectan a su servidor como clientes, por UDP o por la red de Steam. Detalles técnicos en [notes/ARQUITECTURA.md](notes/ARQUITECTURA.md).

## Compilar

Hace falta MinGW-w64 de 32 bits (i686), Python 3 y el `steam_api.dll` de 32 bits del Steamworks SDK.

1. Poné una copia del juego en `game\` (no está en el repo) y ajustá en `build.bat` la ruta de MinGW y la de `steam_api.dll`.
2. Corré `build.bat`. Arma `build\ddraw.dll`, `build\codcoop_core.dll`, `game\main\pakz_coop.pk3` y el instalador `build\Instalar CoD Coop.exe`.
