# Call of Duty Coop

Play the **Call of Duty 1 (2003)** campaign in co-op with friends: up to 4 players, through Steam, over the internet by IP, or on a local network.

**[⬇ Download the installer](https://github.com/TheTiranix/CoDCoop/releases/latest)**

## Features

- **The original campaign, together.** One player hosts: the mission runs on their PC, with the game's own AI, scripts and objectives. The others join their game and play it with them.
- **Down, not dead.** If you get killed you go down, and a teammate revives you by holding *Use* for 3 seconds. If everyone goes down, you go back to the last checkpoint.
- **Mission after mission.** When the host moves on to the next level, everyone else follows automatically.
- **Vehicles.** Teammates ride along with the host and can shoot from the vehicle. On tanks they ride on top with an infantry weapon.
- **Nobody gets left behind.** Anyone who stays too far from the host for a few seconds reappears right behind them.
- **Steam or IP.** With Steam there are no ports to open: your friends see you in the menu and join with one click.
- **Drop in, drop out.** If someone disconnects, their slot is freed and they can join again.
- **On screen.** Teammates show up on the compass, with messages when someone is down or being revived.
- **Same difficulty as the game.** Friendly fire is off by default and can be turned on in the Co-op menu.
- **No collisions between teammates.** Players can walk through each other, so nobody gets stuck when spawning together.

## Requirements

- **Call of Duty 1 version 1.5** for Windows, on every PC that will play.
- The mod installed on all of those PCs.

## Install

1. Download `Install-CoD-Coop.exe` from the [latest release](https://github.com/TheTiranix/CoDCoop/releases/latest).
2. Run it and accept the game folder (the one with `CoDSP.exe`). If it can't find it, it lets you choose it.
3. If Windows shows "Windows protected your PC", click **More info** → **Run anyway** (the installer is not signed).

The installer copies `ddraw.dll`, `codcoop_core.dll`, `steam_api.dll`, `main\pakz_coop.pk3` and a `CoD Coop README.txt` into the game folder. If there was already a `ddraw.dll` or `steam_api.dll` from another program, it keeps it with `.bak` added to the name.

## Play

1. Start Call of Duty as usual and open **Co-op** in the main menu.
2. **Host:** click **Host game** and then pick **New Game** or **Load Game**.
3. **Everyone else:** click **Join <name>** (Steam friends) or type the host's IP and click **Join**.

In the same menu (also during the game, from the pause menu):
- **Friendly fire: Yes/No** is decided by the host.
- **Disconnect** leaves the co-op game: a player who joined goes back to the main menu; if the host clicks it, everyone else goes back to the menu and the host keeps playing alone.

### Through Steam (no ports to open)

- Everyone needs Steam and the game running.
- When the host starts a game, their Steam friends get a **Join <name>** button in the Co-op menu.
- You can also use the Steam friends list: right-click the host → **Join Game** (with the game already running).
- While the game is running, Steam shows "Playing Spacewar": that is expected, co-op uses that Steam test app (AppID 480) to connect.

### Over the internet by IP

- The host opens **UDP port 28960** on their router (forwarded to their PC) and gives the others their public IP.
- On the same home network the host's local IP is enough (`ipconfig`).
- If you can't open ports: use a virtual network program (for example Radmin VPN or ZeroTier) and the IP it gives the host.

### Console commands

| Command | What it does |
|---|---|
| `coophost [port]` | Hosts a game (port 28960 if not given) |
| `coopjoin <ip>[:port]` | Joins the game at that IP |
| `coopjoin steam:<SteamID>` | Joins that player's game through Steam |
| `set coop_friendlyfire 1` | Turns friendly fire on (on the host) |
| `coopleave` | Leaves the co-op game (same as the Disconnect button) |

## Uninstall

Delete `ddraw.dll`, `codcoop_core.dll`, `steam_api.dll`, `main\pakz_coop.pk3` and the `codcoop_tmp` folder from the game folder. If there are `.bak` files (`ddraw.dll.bak`, `steam_api.dll.bak`), remove the `.bak` from their names.

## How it works

Under the hood, CoD1's single player is a Quake 3 server and client in the same process. The mod replaces `ddraw.dll` to hook into the game: the host runs the real mission and the others connect to its server as clients, over UDP or through the Steam network. Technical notes (in Spanish) are in [notes/ARQUITECTURA.md](notes/ARQUITECTURA.md).

## Building

You need 32-bit MinGW-w64 (i686), Python 3 and the 32-bit `steam_api.dll` from the Steamworks SDK.

1. Put a copy of the game in `game\` (not included in the repo) and set the MinGW and `steam_api.dll` paths in `build.bat`.
2. Run `build.bat`. It builds `build\ddraw.dll`, `build\codcoop_core.dll`, `game\main\pakz_coop.pk3` and the installer `build\Install CoD Coop.exe`.

## License

[MIT](LICENSE). Call of Duty belongs to Activision and `steam_api.dll` belongs to Valve: they are not part of this code.
