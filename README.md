# Call of Duty Coop

Jugá la campaña de **Call of Duty 1 (2003)** en cooperativo con amigos: hasta 4 jugadores, por internet o en red local.

**[⬇ Descargar el instalador](https://github.com/TheTiranix/CoDCoop/releases/latest)**

## Qué hace

- **Campaña original en compañía.** Uno es el anfitrión: la misión corre en su PC, con la IA, los scripts y los objetivos del juego. Los demás se unen a su partida y la juegan con él.
- **Caído, no muerto.** Si te matan quedás en el piso y un compañero te levanta manteniendo *Usar* 3 segundos. Si caen todos, se vuelve al último punto de control.
- **Misión tras misión.** Cuando el anfitrión pasa de nivel, los demás pasan con él solos.
- **Vehículos.** Los compañeros viajan con el anfitrión y pueden disparar desde el vehículo. En los tanques van montados con un arma de infantería.
- **Nadie se queda atrás.** El que queda muy lejos del anfitrión durante unos segundos aparece detrás de él.
- **Entrar y salir.** Si alguien se desconecta, su lugar se libera y puede volver a entrar.
- **En pantalla.** Los compañeros aparecen en la brújula, y hay avisos de quién está caído o lo están levantando.
- **Misma dificultad del juego.** El daño entre jugadores viene apagado y se puede activar.

## Requisitos

- **Call of Duty 1 versión 1.5** para Windows, en todas las PCs que van a jugar.
- El mod instalado en todas esas PCs.
- El menú del mod está en español.

## Instalar

1. Bajá `Instalar-CoD-Coop.exe` de la [última versión](https://github.com/TheTiranix/CoDCoop/releases/latest).
2. Abrilo y aceptá la carpeta del juego (la que tiene `CoDSP.exe`). Si no la encuentra solo, te deja elegirla.
3. Si Windows muestra "Windows protegió su PC", tocá **Más información** → **Ejecutar de todas formas** (el instalador no está firmado).

El instalador copia `ddraw.dll`, `codcoop_core.dll`, `main\pakz_coop.pk3` y un `LEEME CoD Coop.txt` en la carpeta del juego. Si ya había un `ddraw.dll` de otro programa, lo guarda como `ddraw.dll.bak`.

## Jugar

1. Abrí el Call of Duty como siempre y entrá en **Cooperativo**, en el menú principal.
2. **Anfitrión:** tocá **Crear partida** y después elegí **Nueva Partida** o **Cargar Partida**.
3. **Los demás:** escribí la IP del anfitrión y tocá **Unirse**.

### Por internet

- El anfitrión abre el puerto **UDP 28960** en su router (dirigido a su PC) y les pasa su IP pública.
- En la misma casa o red alcanza con la IP local del anfitrión (`ipconfig`).
- Sin poder abrir puertos: usen un programa de red virtual (por ejemplo Radmin VPN o ZeroTier) y la IP que ese programa le da al anfitrión.

### Comandos de consola

| Comando | Qué hace |
|---|---|
| `coophost [puerto]` | Crea la partida (puerto 28960 si no se indica) |
| `coopjoin <ip>[:puerto]` | Se une a la partida de esa IP |
| `set coop_friendlyfire 1` | Activa el daño entre jugadores (en el anfitrión) |

## Desinstalar

Borrá de la carpeta del juego `ddraw.dll`, `codcoop_core.dll`, `main\pakz_coop.pk3` y la carpeta `codcoop_tmp`. Si hay un `ddraw.dll.bak`, cambiale el nombre a `ddraw.dll`.

## Cómo funciona

El modo historia de CoD1 es, por dentro, un servidor y un cliente de Quake 3 en el mismo proceso. El mod reemplaza `ddraw.dll` para engancharse al juego: el anfitrión corre la misión real y los demás se conectan a su servidor como clientes, con su propio transporte UDP. Detalles técnicos en [notes/ARQUITECTURA.md](notes/ARQUITECTURA.md).

## Compilar

Hace falta MinGW-w64 de 32 bits (i686) y Python 3.

1. Poné una copia del juego en `game\` (no está en el repo) y ajustá la ruta de MinGW en `build.bat`.
2. Corré `build.bat`. Arma `build\ddraw.dll`, `build\codcoop_core.dll`, `game\main\pakz_coop.pk3` y el instalador `build\Instalar CoD Coop.exe`.
