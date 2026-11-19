# Running CoD Coop on Linux (Proton / Wine)

The mod is a Windows mod (DLLs loaded by the game) and runs on Linux through
Proton. This guide is the setup that was tested end to end:

| | |
|---|---|
| Launcher | AnkerGames (also applies to similar launchers that start the game through `umu-run`/Proton) |
| Proton | 11.0 |
| GPU / driver | NVIDIA (proprietary) and Mesa (llvmpipe) |
| Game | Call of Duty 1.5, mod 0.5.0 |

Three things matter on Linux: the launcher has to keep the modified files, the
driver must not report more OpenGL extensions than the 2003 engine can hold, and
Steam — if you want the friend list — has to be reachable inside the prefix.

## 1. "Buffer overrun detected!" right after the intro

**Symptom:** the game closes with a Microsoft Visual C++ runtime message:
*"Buffer overrun detected!"* It happens before the main menu, with or without the
mod.

**Cause:** the 2003 engine copies the OpenGL extension string into a fixed-size
buffer of roughly 8.7 KB. Modern drivers report much more than that, the stack gets
corrupted, and the runtime aborts:

| Driver / setting | Length of the extension string |
|---|---|
| NVIDIA, default | 11,703 chars → crash |
| NVIDIA, `__GL_ExtensionStringVersion=15000` | 3,885 chars → OK |
| Mesa, `MESA_EXTENSION_MAX_YEAR=2015` | 8,475 chars → OK |
| Mesa, `MESA_EXTENSION_MAX_YEAR=2020` | 8,987 chars → crash |

**Fix:** tell the driver to report only extensions from the game's era. Only the
two environment variables are needed:

```sh
export __GL_ExtensionStringVersion=15000   # NVIDIA
export MESA_EXTENSION_MAX_YEAR=2003        # Mesa (AMD/Intel)
```

### Making it permanent with Proton

Create a small wrapper that sets the variables only for Call of Duty and then
calls the real `umu-run` (or `proton`). Example for a launcher that calls
`~/.local/bin/umu-run`:

```sh
#!/bin/sh
# some launchers export their own libraries; drop them so the host ones are used
unset LD_LIBRARY_PATH LD_PRELOAD PYTHONHOME PYTHONPATH

case "$* $WINEPREFIX $PWD" in
  *CoDSP.exe*|*CoDMP.exe*|*CoDUOSP.exe*|*"Call of Duty"*)
    export __GL_ExtensionStringVersion=15000
    export MESA_EXTENSION_MAX_YEAR=2003
    ;;
esac

exec /usr/bin/umu-run "$@"
```

Put it in place of the launcher's `umu-run` (back up the original first) and
restart the launcher **completely**, including its tray icon — otherwise the old
process keeps the old environment.

## 2. The launcher says the files changed

After installing the mod (or replacing `CoDSP.exe` with the 1.5 build), launchers
that verify their install show:

![Launcher warning, choose "Keep my files"](screenshots/launcher-keep-files.png)

That is expected: the mod adds files and the 1.5 update replaces `CoDSP.exe`. Click
**Keep my files**. If you click *Repair* instead, the launcher puts the original
files back and the mod has to be installed again.

## 3. Starting the game from the launcher

Once the launcher accepts the changed files, pressing **Play** shows a short
check. It is fine as long as it finds `CoDSP.exe` and runs it through Proton:

![Launcher preparing the game with Proton support](screenshots/launcher-starting.png)

What the lines mean:

| Line | Meaning |
|---|---|
| Game files: `CoDSP.exe` found | The 1.5 executable is in place. |
| Proton or Wine: with Proton support | The game starts through Proton, as it should on Linux. |
| Cloud save: needs an eligible plan | Irrelevant for co-op; local saves work. |
| Start `CoDSP.exe`: making sure it keeps running | The launcher waits for the process; this can take a few seconds. |

If the first start closes immediately, see section 1 (the OpenGL extension fix).

## 4. Steam (friend invites)

Steam invites work on Linux and are the same on Windows, but three things are
required — on **every** PC, host and client:

1. **Steam is running and signed in.** The mod loads `steamclient.dll` from the
   running client; a Steam that is closed or still at the login screen is not
   enough.
2. **Spacewar (AppID 480) is installed once.** It is Valve's free test app and
   the co-op uses it to talk to Steam (that is why Steam shows "Playing
   Spacewar"). Install it from a browser or the console:
   `steam://install/480`
3. **The game is started through Steam.** Steam only injects
   `steamclient.dll` into processes it launched, so add the game once:
   **Games → Add a Non-Steam Game → Browse → `CoDSP.exe`**. On Linux also open
   the shortcut's *Properties → Compatibility* and tick **Force the use of a
   specific Steam Play compatibility tool** (Proton). Afterwards always start
   the game from that Steam entry, not from the launcher.

If a 4th step is needed for the OpenGL fix, put the variables in the shortcut's
*Launch Options* (see section 1):

```
__GL_ExtensionStringVersion=15000 MESA_EXTENSION_MAX_YEAR=2003 %command%
```

**Check:** the mod writes `codcoop.log` next to `CoDSP.exe`. When Steam is
reachable it says:

```
steam: listo como 'your Steam name' (7656119...)
```

If it says `steam: no disponible ... solo por IP`, one of the three points is
missing. Joining and hosting by IP still work in that case — use **Host IP** in
the Co-op window and open **UDP 28960** (UDP 28962 for Call of Duty 2).

Linux ↔ Windows works: the invite goes through Steam, not through the game, so
the two sides can run different operating systems, different launchers and
different copies of the game, as long as both are on **Call of Duty 1.5** with
the **same mod version**.

## 5. Building the DLLs on Linux

The sources are plain C++ and only need a MinGW cross compiler — no Wine, no
Visual Studio:

```sh
sudo apt install g++-mingw-w64-i686     # Debian / Ubuntu
# Fedora: sudo dnf install mingw32-gcc-c++      Arch: sudo pacman -S mingw-w64-gcc

./build.sh                 # build/ddraw.dll, build/codcoop_core.dll, build/cod2coop.dll
./build.sh --install       # ... and copy them into game/ (and rebuild the menu pak)
./build.sh --installer     # ... and build "Install CoD Coop.exe" (needs every file in installer/installer.rc)
```

`build.sh` is the same build as `build.bat`: identical flags, identical sources.
The only file that is not in the repository is Valve's 32-bit `steam_api.dll`
(Steam invites); copy it into `build/` from a Windows build or a Steamworks
redistributable. Without it the mod still works over IP.

## 6. Installing without Wine (`tools/install-linux.py`)

The Windows installer only extracts its files, so on Linux you can do the same
with a Python script that needs nothing but `python3`:

```sh
# from the latest GitHub release (downloads Install-CoD-Coop.exe and extracts it)
python3 tools/install-linux.py --game "/path/to/Call of Duty"

# from a local build/ produced by ./build.sh (menu pak taken from game/Main/)
python3 tools/install-linux.py --game ".../Call of Duty" --build build

# only verify an existing installation
python3 tools/install-linux.py --game ".../Call of Duty" --check
```

It checks **Call of Duty 1.5 first** (`CoDSP.exe` must be 1,716,224 bytes) and
stops with the download options if it is not — nothing is written in that case.
Existing files are kept as `<name>.bak`. `--dry-run` shows what would be copied,
`--allow-incomplete` installs anyway (multiplayer-only copies).

## 7. Both sides need the same versions

Host and client must both be on **Call of Duty 1.5** and the **same mod version**.
The game version matters, not how you got there (official patch or a 1.5 build
shipped by your launcher).

The menu patch from this repository only changes the menus, not the network code,
so a client with the original menus can still join.
