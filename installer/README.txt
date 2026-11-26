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
- Patch 1.2 or 1.3 (the Steam version is already 1.3), with co-op installed on every PC.
  Single player shows 1.2 even with patch 1.3, because 1.3 only changed multiplayer.
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
- Everyone with Steam and the game running.
- When the host clicks "Host game", their Steam friends get a "Join <name>" button
  in the Co-op menu.
- You can also use the Steam friends list: right-click the host -> "Join Game"
  (with the game already running).
- While the game is running, Steam shows "Playing Spacewar": that is expected, co-op
  uses that Steam test app to connect.

Over the internet by IP
- The host has to open UDP port 28960 on their router (forwarded to their PC) and
  give the others their public IP (search "what is my IP" online).
- On the same home network the host's local IP is enough (ipconfig command).
- If you cannot open the port: use a virtual network program (for example Radmin VPN
  or ZeroTier) and the IP that program gives the host.

Reporting a problem
- The mod writes a log in the game folder: codcoop.log (Call of Duty 1 and United
  Offensive) or cod2coop.log (Call of Duty 2). Send it with your report.
- If the game crashed and you opened it again, also send codcoop_previous.log (or
  cod2coop_previous.log): it is the log of the session before.
- If the game is in Program Files and the log is not there, look in
  %LOCALAPPDATA%\VirtualStore\Program Files (x86)\ followed by the game's folder.

Uninstall
- Delete from the game folder: ddraw.dll, codcoop_core.dll, steam_api.dll,
  main\pakz_coop.pk3, uo\pakz_coop.pk3 and the codcoop_tmp folder. If there are .bak files
  (ddraw.dll.bak or steam_api.dll.bak), remove the .bak from their names.
- Call of Duty 2: delete ddraw.dll, cod2coop.dll, steam_api.dll and main\zz_coop2.iwd from
  the game folder (and remove the .bak from ddraw.dll.bak or steam_api.dll.bak if any).
