#!/usr/bin/env python3
"""Find the Steam shortcut for a game and print the id64 used by steam://rungameid/.

Steam stores non-Steam shortcuts in userdata/<id>/config/shortcuts.vdf (binary
VDF). The 32-bit appid in there is what Steam generated when the shortcut was
added; the launch URL needs

    id64 = ((appid | 0x80000000) << 32) | 0x02000000

Usage:
    tools/steam-shortcut.py --exe "/path/to/CoDSP.exe"   # prints the id64
    tools/steam-shortcut.py --list                     # all shortcuts

Exit code 1 when no matching shortcut exists.
"""
import argparse
import os
import struct
import sys


def _cstr(data, pos):
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "replace"), end + 1


def _parse_map(data, pos):
    """Parse one binary-VDF map; pos points at its key (after the 0x00 type)."""
    _key, pos = _cstr(data, pos)
    out = {}
    while pos < len(data):
        kind = data[pos]
        pos += 1
        if kind == 0x08:  # end of map
            break
        key_pos = pos
        key, pos = _cstr(data, pos)
        if kind == 0x00:
            value, pos = _parse_map(data, key_pos)
        elif kind == 0x01:
            value, pos = _cstr(data, pos)
        elif kind == 0x02:
            value = struct.unpack_from("<i", data, pos)[0]
            pos += 4
        elif kind == 0x03:
            value = struct.unpack_from("<f", data, pos)[0]
            pos += 4
        elif kind == 0x04:
            value = struct.unpack_from("<i", data, pos)[0]
            pos += 4
        elif kind == 0x05:  # UTF-16LE string
            end = pos
            while data[end:end + 2] != b"\0\0":
                end += 2
            value = data[pos:end].decode("utf-16-le", "replace")
            pos = end + 2
        elif kind == 0x06:  # colour
            value = data[pos:pos + 4]
            pos += 4
        elif kind == 0x07:
            value = struct.unpack_from("<Q", data, pos)[0]
            pos += 8
        else:
            raise ValueError("unknown VDF type 0x%02x at %d" % (kind, pos - 1))
        out[key] = value
    return out, pos


def shortcuts():
    """All non-Steam shortcuts of all Steam users, newest file first."""
    homes = [
        os.path.expanduser("~/.local/share/Steam"),
        os.path.expanduser("~/.steam/steam"),
        os.path.expanduser("~/.var/app/com.valvesoftware.Steam/.local/share/Steam"),
    ]
    files = []
    for home in homes:
        userdata = os.path.join(home, "userdata")
        try:
            users = os.listdir(userdata)
        except OSError:
            continue
        for user in users:
            path = os.path.join(userdata, user, "config", "shortcuts.vdf")
            if os.path.isfile(path):
                files.append(path)
    files = sorted(set(os.path.realpath(p) for p in files),
                   key=os.path.getmtime, reverse=True)

    found = []
    for path in files:
        try:
            with open(path, "rb") as f:
                data = f.read()
            if not data or data[0] != 0x00:
                continue
            entries, _ = _parse_map(data, 1)
        except (OSError, ValueError, IndexError):
            continue
        for entry in entries.values():
            if not isinstance(entry, dict):
                continue
            found.append(entry)
    return found


def launch_id(entry):
    appid = int(entry.get("appid", 0)) & 0xFFFFFFFF
    return ((appid | 0x80000000) << 32) | 0x02000000


def same_exe(a, b):
    if not a or not b:
        return False
    return os.path.realpath(a) == os.path.realpath(b)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", help="path to the game's .exe")
    parser.add_argument("--list", action="store_true", help="print every shortcut")
    args = parser.parse_args()
    if not args.exe and not args.list:
        parser.error("give --exe PATH or --list")

    entries = shortcuts()
    if args.list:
        for entry in entries:
            print("%s\t%s\t%s" % (launch_id(entry),
                                  entry.get("AppName", ""),
                                  entry.get("Exe", "")))
        return 0

    for entry in entries:
        exe = str(entry.get("Exe", "")).strip('"')
        if same_exe(exe, args.exe):
            print(launch_id(entry))
            return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
