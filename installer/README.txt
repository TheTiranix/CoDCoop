CALL OF DUTY COOP - the Call of Duty 1 campaign for up to 4 players
===================================================================

How it works
- One player is the host: the mission runs on their PC. The others join their game.
- If you get killed you go down: a teammate revives you by holding Use for 3 seconds.
  If everyone goes down, you go back to the last checkpoint.
- When the host moves on to the next mission, everyone else follows.

What you need
- Call of Duty 1 version 1.5 on every PC, with co-op installed on all of them.
- United Offensive (version 1.41, same folder) works too: start CoDUOSP.exe and use
  its Co-op menu. The compass does not show teammates there yet.

Install
- Run "Install CoD Coop.exe" and accept the game folder (the one with CoDSP.exe, or
  CoD2SP_s.exe for Call of Duty 2). It detects the game and installs the matching files.

Call of Duty 2 (experimental)
- Version 1.3 (CoD2SP_s.exe), with co-op installed on every PC.
- Main menu -> "Co-op". The host clicks "Host game" and then starts or loads a mission
  as usual. The others click "Join <name>" (Steam friends, no ports to open) or type the
  host's IP and click "Join" (UDP port 28962): their game loads the host's mission by
  itself. In the console, coop2host and coop2join <IP> do the same.
- Teammates see each other, shoot, ride the host's vehicles, show up on the compass and
  revive each other (get close and hold Use for 3 seconds).
- The pause menu does not stop the game while teammates are connected. The Co-op window
  has "Friendly fire" (the host decides) and "Disconnect".

Play
1. Start Call of Duty as usual and open "Co-op" (main menu).
2. The host clicks "Host game" and then picks New Game or Load Game.
3. The others type the host's IP and click "Join".
- In the same menu (also from the pause menu): "Friendly fire" Yes/No (the host
  decides) and "Disconnect" to leave the co-op game.

Through Steam (no ports to open)
- Prerequisites on EVERY PC (Windows and Linux):
  1. Steam installed, running and signed in.
  2. Spacewar (AppID 480) installed once - it is free: steam://install/480
     (the co-op uses this free test app of Valve to talk to Steam). The installer
     offers to open it when it finishes.
  3. The game has to see Steam:
     - Linux (Proton): start the game THROUGH Steam ("Add a Non-Steam Game" ->
       CoDSP.exe, and set Proton for that entry). Starting it only from the
       launcher does not see Steam and cannot invite.
     - Windows: starting it normally is enough (also from AnkerGames or the
       desktop); the mod finds the running Steam by itself.
- When the host clicks "Host game", their Steam friends get a "Join <name>" button
  in the Co-op menu.
- You can also use the Steam friends list: right-click the host -> "Join Game"
  (with the game already running).
- While the game is running, Steam shows "Playing Spacewar": that is expected, co-op
  uses that Steam test app to connect.
- The game log (codcoop.log) says "steam: listo como 'your name'" when the
  connection to Steam works. If it says "solo por IP", one of the three points
  above is missing.

Over the internet by IP
- The host has to open UDP port 28960 on their router (forwarded to their PC) and
  give the others their public IP (search "what is my IP" online).
- On the same home network the host's local IP is enough (ipconfig command).
- If you cannot open the port: use a virtual network program (for example Radmin VPN
  or ZeroTier) and the IP that program gives the host.

Uninstall
- Delete from the game folder: ddraw.dll, codcoop_core.dll, steam_api.dll,
  main\pakz_coop.pk3, uo\pakz_coop.pk3 and the codcoop_tmp folder. If there are .bak files
  (ddraw.dll.bak or steam_api.dll.bak), remove the .bak from their names.
- Call of Duty 2: delete ddraw.dll, cod2coop.dll, steam_api.dll and main\zz_coop2.iwd from
  the game folder (and remove the .bak from ddraw.dll.bak or steam_api.dll.bak if any).
