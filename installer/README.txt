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
- The host starts a mission and types coop2host in the console. The others type
  coop2join followed by the host's IP (UDP port 28962), even from the main menu: their
  game loads the host's mission by itself.
- Teammates see each other, shoot, ride the host's vehicles and revive each other (get
  close and hold Use for 3 seconds). There is no Co-op menu yet.

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

Uninstall
- Delete from the game folder: ddraw.dll, codcoop_core.dll, steam_api.dll,
  main\pakz_coop.pk3, uo\pakz_coop.pk3 and the codcoop_tmp folder. If there are .bak files
  (ddraw.dll.bak or steam_api.dll.bak), remove the .bak from their names.
- Call of Duty 2: delete ddraw.dll and cod2coop.dll from the game folder (and rename
  ddraw.dll.bak back to ddraw.dll if there is one).
