#!/usr/bin/env python3
"""Extract the files embedded in Install-CoD-Coop.exe (Windows PE resources).

The installer is not an archive: installer/installer.cpp stores each file as an
RT_RCDATA resource. This script reads the PE resource table with the standard
library, so it works on Linux without Wine, 7-Zip or pefile.

    python3 tools/extract_installer.py Install-CoD-Coop.exe -o mod/
    python3 tools/extract_installer.py Install-CoD-Coop.exe --list

Resource ids (see installer/installer.cpp):
    1 ddraw.dll            5 steam_api.dll
    2 codcoop_core.dll     6 uo/pakz_coop.pk3
    3 Main/pakz_coop.pk3   7 cod2coop.dll
    4 CoD Coop README.txt  8 main/zz_coop2.iwd
"""
import argparse
import hashlib
import os
import struct
import sys

RT_RCDATA = 10
DEST = {
    1: "ddraw.dll",
    2: "codcoop_core.dll",
    3: "Main/pakz_coop.pk3",
    4: "CoD Coop README.txt",
    5: "steam_api.dll",
    6: "uo/pakz_coop.pk3",
    7: "cod2coop.dll",
    8: "main/zz_coop2.iwd",
}


def resources(data):
    """Return {resource id: bytes} for RT_RCDATA."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (MZ signature missing)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (PE signature missing)")
    nsections, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    ddir = opt + (112 if magic == 0x20B else 96)
    res_rva, _ = struct.unpack_from("<II", data, ddir + 2 * 8)
    if not res_rva:
        raise ValueError("the file has no resources")

    sections = []
    sec = opt + optsize
    for i in range(nsections):
        off = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def offset(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        raise ValueError("RVA 0x%x is in no section" % rva)

    base = offset(res_rva)

    def entries(off):
        named, ids = struct.unpack_from("<HH", data, off + 12)
        return [struct.unpack_from("<II", data, off + 16 + i * 8) for i in range(named + ids)]

    out = {}
    for name, target in entries(base):
        if name & 0x80000000 or name != RT_RCDATA:  # named type, or not RCDATA
            continue
        for rid, rtarget in entries(base + (target & 0x7FFFFFFF)):
            if rid & 0x80000000:
                continue
            lang = base + (rtarget & 0x7FFFFFFF)
            for _, ltarget in entries(lang):
                if ltarget & 0x80000000:
                    continue
                data_rva, size = struct.unpack_from("<II", data, base + ltarget)
                start = offset(data_rva)
                out[rid] = data[start:start + size]
                break
    return out


def main():
    ap = argparse.ArgumentParser(description="Extract the files embedded in Install-CoD-Coop.exe")
    ap.add_argument("exe")
    ap.add_argument("-o", "--out", help="destination directory")
    ap.add_argument("--list", action="store_true", help="only list the resources and their sha256")
    args = ap.parse_args()

    with open(args.exe, "rb") as f:
        data = f.read()
    found = resources(data)
    if not found:
        sys.exit("no RCDATA resources found - is this the CoD Coop installer?")

    missing = [i for i in (1, 2, 3, 4, 5) if i not in found]
    if missing:
        sys.exit("unexpected installer: resources %s are missing" % missing)
    if not args.list and not args.out:
        sys.exit("-o/--out is required (or use --list)")

    for rid in sorted(found):
        blob = found[rid]
        dest = DEST.get(rid, "resource_%d.bin" % rid)
        if args.list:
            print("%-24s %8d B  %s" % (dest, len(blob), hashlib.sha256(blob).hexdigest()))
            continue
        path = os.path.join(args.out, dest)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "wb") as f:
            f.write(blob)
        print("%-24s %8d B" % (dest, len(blob)))


if __name__ == "__main__":
    main()
