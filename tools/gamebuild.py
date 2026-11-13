#!/usr/bin/env python3
"""Builds the game code library on the player's own PC (runtime/include/nx/gamelib.h): their ExeFS main and sdk,
recompiled (recomp/) and compiled into wonder_game.dll (Windows) / libwonder_game.so (Linux), which the program loads.

Nothing of the game ships with SuperWonders: this is how the player's copy becomes the game code. Savage Wind does the
same with its swgame library (runtime/gamebuild.c there): the program ships ready made, the game code is built here.

    gamebuild.py build --exefs <dir with main and sdk> [--data-root DIR] [--jobs N] [--keep-work]
    gamebuild.py check --exefs <dir> [--data-root DIR]     exit 0: up to date, 10: needs a build (says why)
    gamebuild.py id                                         the id of this build kit

The library goes to <data root>/game/ with build-id.txt beside it: the kit's id (the recompiler, the headers the
generated code includes, the compiler and its flags) and the input's (main and sdk). Either changing means a build.
The working files (the generated C++ and the objects, several GB: they hold the player's game) are made in
<data root>/game/work and removed after a good build. --keep-work keeps them, so a build after an interrupted one, or
for a debugger, only redoes what changed. A new kit id starts the objects again, since they can't be reused.

Progress for launchers: lines "progress <0..1> <what>" on stdout (RecompLauncher's prepare protocol), exit 0 when it
worked. It runs from a source checkout (recomp/, runtime/include/) or from a release's game-build/ folder, which holds
the same files beside this script plus a Python and a compiler (tools/pack_gamebuild_kit.py).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

GAME = "wonder"
HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.name == "nt"
LIB = f"{GAME}_game.dll" if WIN else f"lib{GAME}_game.so"
MODULES = (("main", "0x4000000000"), ("sdk", "0x4010000000"))
WARN = ["-Wno-unused-label", "-Wno-unused-value", "-Wno-parentheses-equality", "-Wno-unused-variable",
        "-Wno-unused-but-set-variable", "-Wno-constant-logical-operand", "-Wno-unused-function"]
# The same flags as CMakeLists.txt gives game_main / game_sdk and <game>_game (keep them in step).
if WIN:
    CXXFLAGS = ["-O3", "-DNDEBUG", "-std=gnu++20", *WARN, "-mavx2", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN"]
    LDFLAGS = ["-O3", "-shared"]
else:
    CXXFLAGS = ["-O2", "-DNDEBUG", "-std=gnu++20", *WARN, "-march=x86-64-v3", "-fPIC", "-fvisibility=hidden",
                "-fno-semantic-interposition"]
    LDFLAGS = ["-shared", "-Wl,-Bsymbolic", "-Wl,--no-undefined"]
EXIT_NEEDS_BUILD = 10


class Kit:
    """Where the recompiler, headers, entry file and compiler are: a release's game-build/ folder, or a checkout."""

    def __init__(self):
        if os.path.isdir(os.path.join(HERE, "recomp")):  # a release: game-build/{recomp,include,gamelib_entry.cpp}
            self.root = HERE
            self.include = os.path.join(HERE, "include")
            self.entry = os.path.join(HERE, "gamelib_entry.cpp")
        else:  # a checkout: tools/gamebuild.py
            self.root = os.path.dirname(HERE)
            self.include = os.path.join(self.root, "runtime", "include")
            self.entry = os.path.join(self.root, "runtime", "src", "gamelib_entry.cpp")
        self.recomp = os.path.join(self.root, "recomp")
        self.cxx = self.find_compiler()

    def find_compiler(self):
        exe = "clang++.exe" if WIN else "clang++"
        for c in (os.environ.get("NX_GAMEBUILD_CXX"), os.path.join(HERE, "toolchain", "bin", exe)):
            if c and os.path.isfile(c):
                return c
        return shutil.which(exe) or shutil.which("clang++")

    def files(self):
        """What the generated code's result depends on, besides the input."""
        out = sorted(glob.glob(os.path.join(self.recomp, "*.py")))
        out += [os.path.join(self.include, "nx", h) for h in ("cpu.h", "module.h", "gamelib.h")]
        out.append(self.entry)
        out.append(os.path.abspath(__file__))  # (how it builds: the order it links in, its flags)
        return out

    def compiler_version(self):
        if not self.cxx:
            return "none"
        try:
            out = subprocess.run([self.cxx, "--version"], capture_output=True, text=True, timeout=60).stdout
            return out.splitlines()[0] if out else "none"  # (the version line: the rest names the install folder)
        except OSError:
            return "none"

    def id(self):
        h = hashlib.sha256()
        for f in self.files():
            h.update(os.path.basename(f).encode() + b"\0")
            with open(f, "rb") as fh:
                h.update(fh.read().replace(b"\r\n", b"\n"))  # (a checkout's line endings don't count)
        h.update(" ".join(CXXFLAGS + ["|"] + LDFLAGS).encode())
        h.update(self.compiler_version().encode())
        h.update(LIB.encode())
        return h.hexdigest()[:32]


def input_id(exefs):
    h = hashlib.sha256()
    for name, _ in MODULES:
        with open(os.path.join(exefs, name), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    return h.hexdigest()[:32]


def progress(fraction, what):
    print(f"progress {max(0.0, min(1.0, fraction)):.3f} {what}", flush=True)


def check_exefs(exefs):
    for name, _ in MODULES:
        p = os.path.join(exefs, name)
        if not os.path.isfile(p):
            raise SystemExit(f"error: {p} not found: pick the ExeFS folder of your own copy of the game (it holds main and sdk)")
        with open(p, "rb") as f:
            if f.read(4) != b"NSO0":
                raise SystemExit(f"error: {p} is not an NSO module (an extracted ExeFS has main and sdk as NSO files)")


def default_data_root():
    r = os.environ.get("NX_DATA_ROOT")
    if r:
        return r
    if WIN and os.path.isfile(os.path.join(os.path.dirname(HERE), f"{GAME}.exe")):
        return os.path.join(os.path.dirname(HERE), "data")  # a release: <folder>/wonder.exe, <folder>/data
    raise SystemExit("error: no data root: pass --data-root (or set NX_DATA_ROOT)")


def read_ids(game_dir):
    ids = {}
    try:
        with open(os.path.join(game_dir, "build-id.txt")) as f:
            for line in f:
                k, _, v = line.strip().partition(" ")
                ids[k] = v
    except OSError:
        pass
    return ids


def why_build(kit, exefs, game_dir):
    if not os.path.isfile(os.path.join(game_dir, LIB)):
        return f"no {LIB} yet"
    ids = read_ids(game_dir)
    if ids.get("kit") != kit.id():
        return "this version of SuperWonders builds the game code differently"
    if ids.get("input") != input_id(exefs):
        return "the game files changed"
    return None


def recompile(kit, exefs, gen, done_before, share):
    """python -m recomp for each module, with the recompiler's own progress lines turned into ours."""
    env = dict(os.environ, PYTHONPATH=kit.root + os.pathsep + os.environ.get("PYTHONPATH", ""))
    env.pop("NX_TRACE_SP", None)
    for k, (name, base) in enumerate(MODULES):
        part = share * (0.85 if name == "main" else 0.15)
        start = done_before + (0 if k == 0 else share * 0.85)
        # (-u: the recompiler's progress lines as they come, not all at the end)
        cmd = [sys.executable, "-u", "-m", "recomp", os.path.join(exefs, name), os.path.join(gen, name),
               "--name", name, "--base", base]
        p = subprocess.Popen(cmd, cwd=kit.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        log = []
        for line in p.stdout:
            log.append(line)
            m = re.search(r"emitted (\d+)/(\d+)", line)
            if m:
                progress(start + part * int(m.group(1)) / max(1, int(m.group(2))), f"Recompiling {name}")
        if p.wait():
            sys.stderr.write("".join(log[-40:]))
            raise SystemExit(f"error: recompiling {name} failed (exit {p.returncode})")
        progress(start + part, f"Recompiled {name}")


def compile_all(kit, gen, obj, jobs, done_before, share):
    units = []
    for name, _ in MODULES:
        for src in sorted(glob.glob(os.path.join(gen, name, "*.cpp"))):
            units.append((src, os.path.join(obj, name, os.path.basename(src) + ".o"),
                          [f"-I{kit.include}", f"-I{os.path.join(gen, name)}"]))
    entry_o = os.path.join(obj, "gamelib_entry.o")
    units.append((kit.entry, entry_o, [f"-I{kit.include}", f'-DNX_GAME_NAME="{GAME}"']))
    for name, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)

    todo = [u for u in units if not os.path.isfile(u[1]) or os.path.getmtime(u[1]) < os.path.getmtime(u[0])]
    total = len(todo)
    progress(done_before, f"Compiling the game code ({total} of {len(units)} files)")

    def one(u):
        src, out, extra = u
        tmp = out + ".tmp"
        r = subprocess.run([kit.cxx, *CXXFLAGS, *extra, "-c", src, "-o", tmp], capture_output=True, text=True)
        if r.returncode:
            return src, r.stderr
        os.replace(tmp, out)
        return src, None

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for src, err in pool.map(one, todo):
            if err:
                sys.stderr.write(err[-4000:])
                raise SystemExit(f"error: compiling {os.path.basename(src)} failed")
            done += 1
            if done % 8 == 0 or done == total:
                progress(done_before + share * done / max(1, total), f"Compiling the game code ({done} of {total})")
    return [u[1] for u in units]


def link(kit, objs, game_dir, work):
    tmp = os.path.join(work, LIB)
    rsp = os.path.join(work, "objects.rsp")
    with open(rsp, "w") as f:
        for o in objs:
            f.write('"' + o.replace("\\", "/") + '"\n')
    cmd = [kit.cxx, *LDFLAGS, "-o", tmp, "@" + rsp]
    if WIN:
        cmd.append("-Wl,-Map," + os.path.join(work, f"{GAME}_game.map").replace("\\", "/"))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-4000:])
        raise SystemExit("error: linking the game code failed")
    dest = os.path.join(game_dir, LIB)
    try:
        os.replace(tmp, dest)
    except PermissionError:
        raise SystemExit(f"error: {dest} is in use: close the game first")
    if WIN and os.path.isfile(os.path.join(work, f"{GAME}_game.map")):
        os.replace(os.path.join(work, f"{GAME}_game.map"), os.path.join(game_dir, f"{GAME}_game.map"))


def build(a):
    kit = Kit()
    if not kit.cxx:
        raise SystemExit("error: no C++ compiler (clang++) found: this build of SuperWonders is missing game-build/toolchain")
    exefs = os.path.abspath(a.exefs)
    check_exefs(exefs)
    game_dir = os.path.join(os.path.abspath(a.data_root or default_data_root()), "game")
    reason = why_build(kit, exefs, game_dir)
    if not reason and not a.force:
        progress(1.0, "The game code is up to date")
        return 0
    print(f"building the game code: {reason or 'forced'}", flush=True)
    t0 = time.time()
    work = os.path.join(game_dir, "work")
    gen, obj = os.path.join(work, "gen"), os.path.join(work, "obj")
    os.makedirs(work, exist_ok=True)
    # Objects from another kit (other headers, flags or compiler) can't be reused.
    stamp = os.path.join(work, "kit-id.txt")
    kit_id = kit.id()
    try:
        old = open(stamp).read().strip()
    except OSError:
        old = ""
    if old != kit_id:
        shutil.rmtree(obj, ignore_errors=True)
    with open(stamp, "w") as f:
        f.write(kit_id + "\n")

    recompile(kit, exefs, gen, 0.0, 0.25)
    objs = compile_all(kit, gen, obj, a.jobs, 0.25, 0.70)
    progress(0.96, "Linking the game code")
    link(kit, objs, game_dir, work)
    with open(os.path.join(game_dir, "build-id.txt"), "w") as f:
        f.write(f"kit {kit_id}\ninput {input_id(exefs)}\n")
    if not a.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    progress(1.0, f"The game code is built ({time.time() - t0:.0f} s)")
    return 0


def check(a):
    kit = Kit()
    exefs = os.path.abspath(a.exefs)
    check_exefs(exefs)
    game_dir = os.path.join(os.path.abspath(a.data_root or default_data_root()), "game")
    reason = why_build(kit, exefs, game_dir)
    print(reason or "up to date")
    return EXIT_NEEDS_BUILD if reason else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "check"):
        p = sub.add_parser(name)
        p.add_argument("--exefs", required=True, help="the ExeFS folder of your own copy (main, sdk)")
        p.add_argument("--data-root", help="where the game's data lives (default: NX_DATA_ROOT, or the release's data/)")
        if name == "build":
            p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
            p.add_argument("--keep-work", action="store_true",
                           help="keep the generated code and objects in <data root>/game/work: a later build only "
                                "recompiles what changed")
            p.add_argument("--force", action="store_true", help="build even when up to date")
    sub.add_parser("id")
    a = ap.parse_args()
    if a.cmd == "id":
        print(Kit().id())
        return 0
    return build(a) if a.cmd == "build" else check(a)


if __name__ == "__main__":
    sys.exit(main())
