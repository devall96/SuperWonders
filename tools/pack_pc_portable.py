#!/usr/bin/env python3
"""Pack a ready-to-run Wonder for another Windows PC of the owner's (docs/nvn/WINDOWS-PLAN.md, "Another PC").

The zip holds the built game and its data, nothing to install or build there:

    WonderPC/wonder.exe                      the build (private/wonder/build)
    WonderPC/msvcp140.dll, vcruntime140*.dll the C++ runtime, app-local (Visual Studio's redistributable files)
    WonderPC/data/                           data root discovered beside wonder.exe at startup
    WonderPC/README.txt                      requirements and direct-launch instructions
    WonderPC/data/analysis, extract/romfs, spirv (with results.tsv), fonts, user/save (unless --no-save)

The data comes from the owner's own cartridge: for the owner's machines only, never to be shared.

Usage: pack_pc_portable.py [--root private/wonder] [--out private/wonder/pc/WonderPC-portable.zip] [--no-save]
"""
import argparse
import glob
import os
import sys
import zipfile

TOP = "WonderPC/"
REDIST = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"

README = """Super Mario Bros. Wonder recompilation for Windows

Extract this folder to a short path (for example C:\\WonderPC), then double-click wonder.exe. No command script is
needed. Keep the data folder beside wonder.exe; the executable finds it automatically, regardless of the current
working directory. Deep paths can exceed Windows path limits for some game files.

Requirements: Windows 10/11 64-bit, an AVX2-capable CPU, a GPU and driver supporting Vulkan 1.3, and 16 GB of RAM
recommended. The first launch may take a few minutes while Windows Defender scans the files.

Use the in-game F1 menu for settings. F11 or Alt+Enter toggles fullscreen. Logs are written to data\\nx.log; saves are
in data\\user\\save.

This personal package contains files generated from the owner's own game dump and must not be shared.
"""


def redist_dlls():
    hits = sorted(glob.glob(os.path.join(REDIST, "*", "x64", "Microsoft.VC14*.CRT")))
    if not hits:
        sys.exit(f"no Visual C++ redistributable under {REDIST}")
    crt = hits[-1]
    for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        p = os.path.join(crt, name)
        if not os.path.isfile(p):
            sys.exit(f"missing {p}")
        yield p, name


def tree(src, dst, only=None):
    for d, _, files in os.walk(src):
        for f in sorted(files):
            if only and not only(f):
                continue
            p = os.path.join(d, f)
            yield p, dst + "/" + os.path.relpath(p, src).replace(os.sep, "/")


def entries(root, save):
    exe = os.path.join(root, "build", "wonder.exe")
    if not os.path.isfile(exe):
        sys.exit(f"missing {exe} (build it first)")
    yield exe, "wonder.exe"
    # A build with the game code in its own library (NX_GAME_LIBRARY, the default): the library and the C++ runtime
    # it shares with the program, next to it.
    for f in ("wonder_game.dll", "libc++.dll", "libunwind.dll"):
        if os.path.isfile(os.path.join(root, "build", f)):
            yield os.path.join(root, "build", f), f
    yield from redist_dlls()
    yield from tree(os.path.join(root, "analysis"), "data/analysis")
    yield from tree(os.path.join(root, "extract", "romfs"), "data/extract/romfs")
    spirv = os.path.join(root, "spirv")
    if not os.path.isfile(os.path.join(spirv, "results.tsv")):
        sys.exit("missing spirv/results.tsv (docs/nvn/TRANSLATOR.md)")
    yield from tree(spirv, "data/spirv", lambda f: f.endswith(".spv") or f == "results.tsv")
    yield from tree(os.path.join(root, "fonts"), "data/fonts")
    if save and os.path.isdir(os.path.join(root, "user", "save")):
        yield from tree(os.path.join(root, "user", "save"), "data/user/save")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="private/wonder")
    ap.add_argument("--out", default="private/wonder/pc/WonderPC-portable.zip")
    ap.add_argument("--no-save", action="store_true", help="leave the save out (a new game on the other PC)")
    a = ap.parse_args()
    files = list(entries(a.root, not a.no_save))
    total = sum(os.path.getsize(p) for p, _ in files)
    print(f"{len(files)} files, {total / 2**30:.2f} GB -> {a.out}")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
        z.writestr(TOP + "README.txt", README.replace("\n", "\r\n"))
        for k, (p, name) in enumerate(files):
            z.write(p, TOP + name)
            if k % 2000 == 0:
                print(f"  {k}/{len(files)}")
    print(f"done: {os.path.getsize(a.out) / 2**30:.2f} GB")


if __name__ == "__main__":
    main()
