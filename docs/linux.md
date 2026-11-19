# Running CoD Coop on Linux (Proton / Wine)

The mod itself is a Windows mod (DLLs loaded by the game). On Linux it runs
through Proton, and two things are worth knowing.

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

## 4. Steam

Steam friend invites need `steamclient.dll` from a running Steam installation. If
Steam is not available inside the Proton prefix, the log says:

```
steam: no disponible ... solo por IP
```

That is fine — hosting and joining by IP still work. Use **Host IP** in the Co-op
window and open **UDP 28960** (UDP 28962 for Call of Duty 2).

## 5. Both sides need the same versions

Host and client must both be on **Call of Duty 1.5** and the **same mod version**.
The game version matters, not how you got there (official patch or a 1.5 build
shipped by your launcher).

The menu patch from this repository only changes the menus, not the network code,
so a client with the original menus can still join.
