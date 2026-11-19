#!/usr/bin/env python3
"""Install CoD Coop into a Call of Duty folder on Linux - without Wine, 7-Zip or Windows.

    # from the latest GitHub release (downloads Install-CoD-Coop.exe and extracts it)
    python3 tools/install-linux.py --game "/path/to/Call of Duty"

    # from a build/ folder produced by ./build.sh
    python3 tools/install-linux.py --game "..." --build build

    # only check an existing installation
    python3 tools/install-linux.py --game "..." --check

Nothing is written without a backup: an existing file is copied to <name>.bak first.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_installer import resources  # noqa: E402  (same folder)

REPO = "TheTiranix/CoDCoop"
RELEASE_URL = "https://github.com/%s/releases/download/%%s/Install-CoD-Coop.exe" % REPO
API_URL = "https://api.github.com/repos/%s/releases" % REPO

EXE_SIZE = 1716224          # Call of Duty 1.5 (CoDSP.exe)
EXE_SHA = "c755c7da411c87039ade462a2299402c3c4ca5c4fc4f1ae5cea6e25b05c69f47"

FILES = ["ddraw.dll", "codcoop_core.dll", "Main/pakz_coop.pk3", "CoD Coop README.txt", "steam_api.dll"]

# v0.5.0, the release this script was written against
KNOWN = {
    "installer": "05dd77bfd15e343fed9d6f8a6d4aefe9eddb8118481fe7404bf657dcee82da6f",
    "ddraw.dll": "8bff9c8a41cf1438e73be84e6c5d09bd88928c7c4ab16a8414efa757bc0fa46c",
    "codcoop_core.dll": "42aeb2439ffed7823f4d1e4bdf355c589f6a5092b856e6f6fc631a7b0c0c4618",
    "steam_api.dll": "e0ccfcdde7bbba55dba4c19007299018a2cf86f92c7bc1df4af134c65dbf8003",
    "Main/pakz_coop.pk3": "01af9d000503a5e49871ba9be89921991ebf010390334b4693551cfa7b387d4f",
    "CoD Coop README.txt": "a53b9b357a4d9e00126ae441b374216ddd6ea6b8d47179ae41f3ec486a33779a",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find(parent, name):
    """Case-insensitive child lookup: on Linux the game may have Main/ or main/."""
    try:
        for entry in os.listdir(parent):
            if entry.lower() == name.lower():
                return os.path.join(parent, entry)
    except OSError:
        pass
    return None


def say(*a):
    print(*a, flush=True)


def die(msg):
    say("ERROR: " + msg)
    sys.exit(2)


def steam_apps_dirs():
    """steamapps folders of the local Steam installation (all libraries)."""
    home = os.path.expanduser("~")
    out = []
    for base in (os.path.join(home, ".local/share/Steam"), os.path.join(home, ".steam/steam")):
        out.append(os.path.join(base, "steamapps"))
        vdf = os.path.join(base, "steamapps/libraryfolders.vdf")
        try:
            with open(vdf, errors="replace") as f:
                for line in f:
                    m = re.search(r'"path"\s+"([^"]+)"', line)
                    if m:
                        out.append(os.path.join(m.group(1), "steamapps"))
        except OSError:
            pass
    return out


def spacewar_installed():
    return any(os.path.exists(os.path.join(d, "appmanifest_480.acf")) for d in steam_apps_dirs())


def offer_spacewar():
    """Spacewar (AppID 480) is what the mod uses to talk to Steam; it is free but has to be installed once."""
    url = "steam://install/480"
    if spacewar_installed():
        say("Spacewar (AppID 480): installed (Steam invites ready once Steam runs).")
        return
    say("Spacewar (AppID 480) is not installed: Steam invites need it once, it is free.")
    if not sys.stdin.isatty():
        say("Install it later with: " + url)
        return
    try:
        ans = input("Open Steam now to install Spacewar? [Y/n] ").strip().lower()
    except EOFError:
        ans = "n"
    if ans not in ("", "y", "yes", "j", "ja"):
        say("Skipped. You can install it later with: " + url)
        return
    for cmd in (["xdg-open", url], ["steam", url]):
        try:
            subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            say("asked Steam to install Spacewar (" + url + ")")
            return
        except FileNotFoundError:
            continue
    say("could not open Steam - open this link: " + url)


def download(url, dest):
    say("downloading %s" % url)
    req = urllib.request.Request(url, headers={"User-Agent": "cod-coop-linux-installer"})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest + ".part", "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(dest + ".part", dest)


def latest_tag():
    try:
        req = urllib.request.Request(API_URL, headers={"User-Agent": "cod-coop-linux-installer"})
        with urllib.request.urlopen(req, timeout=30) as r:
            for rel in json.load(r):
                if not rel.get("draft"):
                    return rel["tag_name"]
    except Exception as e:  # offline / rate limit: fall back to the version this script knows
        say("could not ask GitHub for the latest release (%s), using v0.5.0" % e)
    return "v0.5.0"


def from_release(tag, cache):
    """Download Install-CoD-Coop.exe and extract it into the cache."""
    tag = tag or latest_tag()
    os.makedirs(cache, exist_ok=True)
    exe = os.path.join(cache, "Install-CoD-Coop-%s.exe" % tag)
    if not os.path.exists(exe):
        download(RELEASE_URL % tag, exe)
    if tag == "v0.5.0" and sha256(exe) != KNOWN["installer"]:
        die("%s does not match the v0.5.0 installer (delete it and try again)" % exe)
    out = os.path.join(cache, "mod-%s" % tag)
    shutil.rmtree(out, ignore_errors=True)
    with open(exe, "rb") as f:
        res = resources(f.read())
    dest = {1: "ddraw.dll", 2: "codcoop_core.dll", 3: "Main/pakz_coop.pk3", 4: "CoD Coop README.txt",
            5: "steam_api.dll", 6: "uo/pakz_coop.pk3"}
    for rid, name in dest.items():
        if rid not in res:
            continue
        path = os.path.join(out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(res[rid])
    say("extracted %s to %s" % (tag, out))
    return out, tag


def from_build(build):
    """A build/ folder only has the DLLs; the menu pak and README come from the repository."""
    missing = [f for f in ("ddraw.dll", "codcoop_core.dll") if not os.path.exists(os.path.join(build, f))]
    if missing:
        die("%s is missing - run ./build.sh first" % ", ".join(missing))
    return build


def build_extras(build, pak):
    """Repository files that replace the ones the Windows installer embeds."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    alt = {}
    if pak:
        alt["Main/pakz_coop.pk3"] = pak
    else:
        for rel in ("game/Main/pakz_coop.pk3", "game/main/pakz_coop.pk3"):
            if os.path.exists(os.path.join(here, rel)):
                alt["Main/pakz_coop.pk3"] = os.path.join(here, rel)
                break
        else:
            die("no menu pak found - run ./build.sh --install once (it builds game/Main/pakz_coop.pk3)\n"
                "       or pass --pak /path/to/pakz_coop.pk3")
    if not os.path.exists(alt["Main/pakz_coop.pk3"]):
        die("no such pak: %s" % alt["Main/pakz_coop.pk3"])
    if not pak_ok(alt["Main/pakz_coop.pk3"]):
        die("%s is not a CoD Coop menu pak - rebuild it with ./build.sh --install" % alt["Main/pakz_coop.pk3"])
    alt["CoD Coop README.txt"] = os.path.join(here, "installer/README.txt")
    uo = os.path.join(here, "game/uo/pakz_coop.pk3")
    if os.path.exists(uo):
        alt["uo/pakz_coop.pk3"] = uo
    return alt


def plan(game, source, alt=None):
    """Return (source file, destination relative path) pairs."""
    alt = alt or {}
    todo = []
    for name in FILES:
        src = alt.get(name) or os.path.join(source, name)
        if not os.path.exists(src):
            if name == "steam_api.dll":
                say("NOTE: steam_api.dll is not in the source - Steam invites will be unavailable (IP works)")
                continue
            die("%s is missing in %s" % (name, src))
        todo.append((src, name))
    uo = alt.get("uo/pakz_coop.pk3") or os.path.join(source, "uo/pakz_coop.pk3")
    if find(game, "CoDUOSP.exe") and os.path.exists(uo):
        todo.append((uo, "uo/pakz_coop.pk3"))
    return todo


def backup(path):
    if not os.path.exists(path):
        return None
    bak = path + ".bak"
    n = 1
    while os.path.exists(bak):
        bak = "%s.bak.%d" % (path, n)
        n += 1
    shutil.copy2(path, bak)
    return bak


def target(game, rel):
    """Main/ vs main/: reuse the folder that exists, create Main/ otherwise."""
    parts = rel.split("/")
    if len(parts) == 1:
        return os.path.join(game, rel)
    folder = find(game, parts[0]) or os.path.join(game, parts[0])
    return os.path.join(folder, *parts[1:])


def install(game, todo, dry, force, known=True):
    for src, rel in todo:
        dst = target(game, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        same = os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and sha256(dst) == sha256(src)
        if same:
            say("  unchanged  %s" % rel)
            continue
        if dry:
            say("  [dry-run]  %s -> %s" % (rel, dst))
            continue
        bak = backup(dst)
        shutil.copy2(src, dst)
        say("  installed  %s%s" % (rel, "  (backup: %s)" % os.path.basename(bak) if bak else ""))
        if known and not force and rel in KNOWN and sha256(dst) != KNOWN[rel]:
            say("WARNING: %s does not match the v0.5.0 build" % rel)


def pak_ok(path):
    """The menu pak is the game's main.menu with the Co-op button and window spliced in (make_ui.py)."""
    try:
        with zipfile.ZipFile(path) as z:
            menu = z.read("ui/main.menu").decode("latin-1", "replace")
    except (OSError, KeyError, zipfile.BadZipFile):
        return False
    return "ui_coop_paused" in menu and re.search(r"name\s+coop\b", menu) is not None


def pak_hosting_fix(path):
    """True if the pak also contains the hosting-state fix (mod 0.5.1+); older paks work but mislabel the menu."""
    try:
        with zipfile.ZipFile(path) as z:
            return "ui_coop_hosting" in z.read("ui/main.menu").decode("latin-1", "replace")
    except (OSError, KeyError, zipfile.BadZipFile):
        return False


def check(game):
    ok = True

    def row(name, value, good):
        nonlocal ok
        say("  %-38s %s" % (name, value))
        if not good:
            ok = False

    exe = find(game, "CoDSP.exe")
    if not exe:
        row("CoDSP.exe", "MISSING", False)
    else:
        size = os.path.getsize(exe)
        row("CoDSP.exe (1.5 = %d B)" % EXE_SIZE, "%d B" % size, size == EXE_SIZE)
    for name in ("ddraw.dll", "codcoop_core.dll"):
        p = target(game, name)
        row(name, "ok" if os.path.exists(p) else "MISSING", os.path.exists(p))
    # Optional: without Steam's redistributable the mod still works over IP.
    row("steam_api.dll (Steam invites)", "ok" if os.path.exists(target(game, "steam_api.dll"))
        else "missing (IP games work)", True)
    pak = target(game, "Main/pakz_coop.pk3")
    if not os.path.exists(pak):
        row("Main/pakz_coop.pk3", "MISSING", False)
    else:
        row("Main/pakz_coop.pk3", "ok" if pak_ok(pak) else "not a CoD Coop menu pak", pak_ok(pak))
        if pak_ok(pak) and not pak_hosting_fix(pak):
            say("  %-38s %s" % ("  (menu)", "old menu pak - the hosting fix needs a newer release/build"))
    uo = find(game, "CoDUOSP.exe")
    if uo:
        row("United Offensive", "ok" if os.path.exists(target(game, "uo/pakz_coop.pk3")) else "pak missing",
            os.path.exists(target(game, "uo/pakz_coop.pk3")))
    say("")
    say("  complete" if ok else "  incomplete - see above")
    return 0 if ok else 1


def exe_help(game, size):
    say("""

  ---------------------------------------------------------------------------
  Call of Duty 1.5 is missing

  expected:  %s/CoDSP.exe  (%d bytes)
  found:     %d bytes - that is the version your launcher installed, not 1.5

  A) Official patch (recommended)
     https://www.moddb.com/games/call-of-duty/downloads/call-of-duty-v15-patch
     Run it and choose the game folder above as the target.

  B) Copy CoDSP.exe (1,716,224 bytes) from a PC where the game is already 1.5
     (USB stick / network), then copy it here as CoDSP.exe.

  C) Launcher that already ships 1.5
     Point the launcher at this folder, or use its "verify files" option.

  Then run this script again. --allow-incomplete installs the mod anyway.
  ---------------------------------------------------------------------------
""" % (game, EXE_SIZE, size))


def main():
    ap = argparse.ArgumentParser(description="Install CoD Coop on Linux (no Wine needed)")
    ap.add_argument("--game", required=True, help="the Call of Duty folder (with CoDSP.exe)")
    ap.add_argument("--release", nargs="?", const="", metavar="TAG",
                    help="install from a GitHub release (default tag: latest)")
    ap.add_argument("--build", metavar="DIR", help="install the DLLs from a local build (see ./build.sh)")
    ap.add_argument("--pak", metavar="FILE", help="menu pak for --build (default: game/Main/pakz_coop.pk3)")
    ap.add_argument("--check", action="store_true", help="only verify the installation")
    ap.add_argument("--dry-run", action="store_true", help="show what would be copied")
    ap.add_argument("--allow-incomplete", action="store_true", help="install even if CoDSP.exe is not 1.5")
    ap.add_argument("--force", action="store_true", help="ignore checksum warnings")
    args = ap.parse_args()

    game = os.path.abspath(os.path.expanduser(args.game))
    if not os.path.isdir(game):
        die("no such folder: %s" % game)

    if args.check:
        return check(game)

    exe = find(game, "CoDSP.exe")
    if not exe:
        die("CoDSP.exe not found in %s - is this the Call of Duty folder?" % game)
    size = os.path.getsize(exe)
    if size != EXE_SIZE and not args.allow_incomplete:
        exe_help(game, size)
        return 2
    if size == EXE_SIZE:
        say("CoDSP.exe: 1.5 (%d bytes)" % size)
    else:
        say("WARNING: CoDSP.exe is %d bytes, not the 1.5 build" % size)

    if args.build:
        source = from_build(args.build)
        alt = build_extras(args.build, args.pak and os.path.abspath(os.path.expanduser(args.pak)))
        say("source: %s (local build)" % source)
    else:
        if args.pak:
            die("--pak only applies to --build")
        cache = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "cod-coop")
        source, tag = from_release(args.release, cache)
        alt = {}
        say("source: %s" % tag)

    todo = plan(game, source, alt)
    say("")
    install(game, todo, args.dry_run, args.force, known=not args.build)
    if not args.dry_run:
        say("")
        offer_spacewar()
    say("""
Done. In the game: Co-op -> Host game -> Back -> New Game.
Friends: same CoD 1.5 and same mod version, then Host IP -> Join, or Steam invites.

Steam invites (cross-platform) need all three:
  * Steam running and signed in on both PCs
  * Spacewar (AppID 480) installed once - the installer offers it, or: steam://install/480
  * on Linux the game has to be started through Steam (Add a Non-Steam Game ->
    CoDSP.exe, force Proton); on Windows starting it normally is enough
See docs/linux.md for the exact steps.
""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
