#!/bin/sh
# Build the CoD Coop DLLs on Linux (or Windows with MSYS2) using a MinGW cross compiler.
#
#   ./build.sh                 build ddraw.dll, codcoop_core.dll and cod2coop.dll into build/
#   ./build.sh --install       additionally copy the DLLs into game/ (and build the menu pak if possible)
#   ./build.sh --installer     also build "Install CoD Coop.exe" (needs every file listed in installer/installer.rc)
#
# The sources are the same as build.bat; nothing here is Linux-specific. Requirements:
#   Debian/Ubuntu: sudo apt install g++-mingw-w64-i686
#   Fedora:        sudo dnf install mingw32-gcc-c++
#   Arch:          sudo pacman -S mingw-w64-gcc
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"

CXX=${CXX:-i686-w64-mingw32-g++}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
INSTALL=0
INSTALLER=0
for arg in "$@"; do
    case "$arg" in
        --install)   INSTALL=1 ;;
        --installer) INSTALLER=1 ;;
        -h|--help)   sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (--help)" >&2; exit 2 ;;
    esac
done

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "ERROR: $CXX not found." >&2
    echo "  Debian/Ubuntu: sudo apt install g++-mingw-w64-i686" >&2
    echo "  Fedora:        sudo dnf install mingw32-gcc-c++" >&2
    echo "  Arch:          sudo pacman -S mingw-w64-gcc" >&2
    exit 1
fi

OPT="-std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp"
mkdir -p build

echo "== ddraw.dll"
"$CXX" $OPT -o build/ddraw.dll src/loader.cpp src/ddraw.def
echo "== codcoop_core.dll"
"$CXX" $OPT -o build/codcoop_core.dll src/core.cpp -lws2_32
echo "== cod2coop.dll"
"$CXX" $OPT -o build/cod2coop.dll src/cod2.cpp -lws2_32
echo "BUILD OK: $(ls -l build/*.dll | awk '{print $9, $5}' | tr '\n' ' ')"
echo

# steam_api.dll is Valve's 32-bit redistributable (SteamVR/steamworks) and is not in the repository.
if [ ! -f build/steam_api.dll ]; then
    echo "NOTE: build/steam_api.dll is missing (Steam invites need it, IP games do not)."
    echo "      Copy it from a 32-bit Steamworks redistributable or from your Windows build."
    echo
fi

if [ "$INSTALL" = 1 ]; then
    if [ -d game ] && [ -f game/CoDSP.exe ]; then
        cp build/ddraw.dll build/codcoop_core.dll game/
        [ -f build/steam_api.dll ] && cp build/steam_api.dll game/ || true
        echo "copied the DLLs to game/"
        # The menu pak is built from the game's own localized pak (Spanish by default, like build.bat).
        if [ -f game/Main/localized_spanish_pak0.pk3 ]; then
            python3 -I tools/make_ui.py || echo "WARNING: could not build the menu pak (is the game open?)" >&2
        elif [ -f game/Main/pakz_coop.pk3 ]; then
            echo "menu pak already present, not rebuilt (put localized_spanish_pak0.pk3 in game/Main to rebuild)"
        else
            echo "WARNING: no menu pak in game/Main (see docs/linux.md)" >&2
        fi
    else
        echo "WARNING: game/ not found - install the game files there or copy build/*.dll by hand" >&2
    fi
fi

if [ "$INSTALLER" = 1 ]; then
    missing=""
    for f in build/steam_api.dll game/Main/pakz_coop.pk3 game/uo/pakz_coop.pk3 game2/main/zz_coop2.iwd; do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -n "$missing" ]; then
        echo "WARNING: not building the installer, these files are missing:$missing" >&2
        echo "         (installer/installer.rc embeds all of them)" >&2
    elif ! command -v "$WINDRES" >/dev/null 2>&1; then
        echo "WARNING: $WINDRES not found, not building the installer" >&2
    else
        echo "== Install CoD Coop.exe"
        "$WINDRES" -i installer/installer.rc -o build/installer_res.o
        "$CXX" -std=c++17 -O2 -static -mwindows -municode -o "build/Install CoD Coop.exe" \
            installer/installer.cpp build/installer_res.o -lole32 -lshell32
        echo "BUILD OK: build/Install CoD Coop.exe"
    fi
fi
