#!/usr/bin/env python3
"""Lays out a SuperWonders release folder: the program, ready made, and game-build/, what the player's PC needs to
build the game code from their own files (tools/gamebuild.py). Nothing of the game is in it.

    <out>/wonder.exe, libc++.dll, libunwind.dll       the program (a build with NX_GAME_LIBRARY=ON)
    <out>/game-build/gamebuild.py                      the build (python game-build/gamebuild.py build --exefs ...)
    <out>/game-build/recomp/                           the recompiler
    <out>/game-build/include/nx/, gamelib_entry.cpp    what the generated code includes, and the library's entry
    <out>/game-build/python/                           a Python with capstone and lz4 (Windows)
    <out>/game-build/toolchain/                        a cut-down llvm-mingw: clang and lld for x86-64 only (Windows)
    <out>/game-build/kit-id.txt                        tools/gamebuild.py's id of this kit

usage: pack_gamebuild_kit.py --build <build dir> --out <release folder>
                             [--toolchain <llvm-mingw>] [--python <CPython install>] [--site-packages <dir>]
On Linux, --toolchain and --python are left out: the system's clang++ and python3 (with capstone and lz4) are used.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = "wonder"


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst, ignore=None):
    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


def size(path):
    return sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(path) for f in fs)


def pack_toolchain(src, dst):
    """clang and lld for x86_64-w64-mingw32, the C++ library and the Windows headers and import libraries."""
    bins = ["clang.exe", "clang++.exe", "x86_64-w64-mingw32-clang++.exe", "ld.lld.exe", "libLLVM-*.dll",
            "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll", "clang-[0-9]*.exe",
            "x86_64-w64-windows-gnu.cfg", "x86_64-pc-windows-gnu.cfg", "mingw32-common.cfg"]
    for pat in bins:
        hits = glob.glob(os.path.join(src, "bin", pat))
        if not hits:
            raise SystemExit(f"toolchain: no bin/{pat} in {src}")
        for h in hits:
            copy(h, os.path.join(dst, "bin", os.path.basename(h)))
    copytree(os.path.join(src, "include"), os.path.join(dst, "include"))
    copytree(os.path.join(src, "x86_64-w64-mingw32"), os.path.join(dst, "x86_64-w64-mingw32"),
             ignore=shutil.ignore_patterns("*.pdb", "share", "bin"))
    # clang's own headers and compiler-rt for x86_64 only
    for res in glob.glob(os.path.join(src, "lib", "clang", "*")):
        rel = os.path.relpath(res, src)
        copytree(os.path.join(res, "include"), os.path.join(dst, rel, "include"))
        for lib in glob.glob(os.path.join(res, "lib", "windows", "*x86_64*")):
            copy(lib, os.path.join(dst, rel, "lib", "windows", os.path.basename(lib)))
    for lic in ("LICENSE.TXT",):
        if os.path.isfile(os.path.join(src, lic)):
            copy(os.path.join(src, lic), os.path.join(dst, lic))


def pack_python(src, site, dst):
    """A CPython install, without what a build never uses, plus capstone and lz4."""
    for f in os.listdir(src):
        p = os.path.join(src, f)
        if os.path.isfile(p) and (f.lower().endswith((".dll", ".exe")) or f.upper().startswith("LICENSE")):
            if f.lower() in ("pythonw.exe",):
                continue
            copy(p, os.path.join(dst, f))
    copytree(os.path.join(src, "DLLs"), os.path.join(dst, "DLLs"),
             ignore=shutil.ignore_patterns("_tkinter*", "tcl*", "tk*", "_test*", "*.pdb"))
    copytree(os.path.join(src, "Lib"), os.path.join(dst, "Lib"),
             ignore=shutil.ignore_patterns("test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip",
                                           "site-packages", "__pycache__", "lib2to3", "pydoc_data"))
    for pkg in ("capstone", "lz4"):
        hits = glob.glob(os.path.join(site, pkg)) + glob.glob(os.path.join(site, pkg + "-*.dist-info"))
        if not hits:
            raise SystemExit(f"python: {pkg} not in {site}")
        for h in hits:
            copytree(h, os.path.join(dst, "Lib", "site-packages", os.path.basename(h)),
                     ignore=shutil.ignore_patterns("__pycache__"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, help="a build folder with NX_GAME_LIBRARY=ON")
    ap.add_argument("--out", required=True)
    ap.add_argument("--toolchain", help="llvm-mingw (Windows)")
    ap.add_argument("--python", help="a CPython install (Windows)")
    ap.add_argument("--site-packages", help="where capstone and lz4 are (default: this Python's)")
    a = ap.parse_args()
    win = os.name == "nt"
    out = os.path.abspath(a.out)
    kit = os.path.join(out, "game-build")
    if os.path.exists(kit):
        shutil.rmtree(kit)

    exe = f"{GAME}.exe" if win else GAME
    program = [exe] + (["libc++.dll", "libunwind.dll"] if win else [])
    for f in program:
        if not os.path.isfile(os.path.join(a.build, f)):
            raise SystemExit(f"{a.build}: no {f} (a build with NX_GAME_LIBRARY=ON)")
        copy(os.path.join(a.build, f), os.path.join(out, f))

    copy(os.path.join(REPO, "tools", "gamebuild.py"), os.path.join(kit, "gamebuild.py"))
    for f in glob.glob(os.path.join(REPO, "recomp", "*.py")):
        copy(f, os.path.join(kit, "recomp", os.path.basename(f)))
    for h in ("cpu.h", "module.h", "gamelib.h"):
        copy(os.path.join(REPO, "runtime", "include", "nx", h), os.path.join(kit, "include", "nx", h))
    copy(os.path.join(REPO, "runtime", "src", "gamelib_entry.cpp"), os.path.join(kit, "gamelib_entry.cpp"))

    py = sys.executable
    if win:
        if not a.toolchain or not a.python:
            raise SystemExit("Windows: --toolchain (llvm-mingw) and --python (a CPython install) are needed")
        pack_toolchain(a.toolchain, os.path.join(kit, "toolchain"))
        site = a.site_packages or next(p for p in sys.path if p.endswith("site-packages"))
        pack_python(a.python, site, os.path.join(kit, "python"))
        py = os.path.join(kit, "python", "python.exe")
    kit_id = subprocess.run([py, "-I", os.path.join(kit, "gamebuild.py"), "id"], capture_output=True, text=True,
                            check=True).stdout.strip()
    with open(os.path.join(kit, "kit-id.txt"), "w") as f:
        f.write(kit_id + "\n")
    print(f"{out}: program {sum(os.path.getsize(os.path.join(out, f)) for f in program) / 1e6:.1f} MB, "
          f"game-build {size(kit) / 1e6:.0f} MB, kit {kit_id}")
    for part in ("toolchain", "python"):
        if os.path.isdir(os.path.join(kit, part)):
            print(f"  {part}: {size(os.path.join(kit, part)) / 1e6:.0f} MB")


if __name__ == "__main__":
    sys.exit(main())
