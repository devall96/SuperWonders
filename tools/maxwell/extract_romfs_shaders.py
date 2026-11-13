#!/usr/bin/env python3
"""Extract unique Maxwell shader stages from an already extracted Wonder RomFS.

Usage: python tools/maxwell/extract_romfs_shaders.py <romfs> <output-under-private>
"""
import glob
import hashlib
import os
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, REPO)
from tools.maxwell import containers


CATALOG_MAGIC = b"WONSHD01"


def write_program_catalog(romfs, output):
    """Record actual shader-stage combinations from the game's own shader archives.

    This is local build output. The runtime uses it to prepare native Vulkan pipelines
    before the first frame instead of guessing combinations from a list of SPIR-V files.
    """
    groups = (
        "Shader/*.zs", "Effect/*.esetb.byml.zs", "Layout/*Nin_NX_NVN*.blarc*",
        "Lib/agl/*.sarc.zs", "Lib/gsys/*.sarc.zs",
    )
    stages, stage_index, programs = [], {}, set()
    for pattern in groups:
        for path in sorted(glob.glob(os.path.join(romfs, pattern))):
            data = containers.read_file(path)
            root = containers.open_blob(data, 0, len(data), os.path.basename(path))
            if not root:
                continue
            for node in containers.walk(root):
                for _, program_stages in node.programs:
                    refs = []
                    for stage in program_stages:
                        stage_id = {"vertex": 1, "pixel": 2, "compute": 3}.get(stage.kind)
                        if stage_id is None:
                            continue
                        control = data[stage.ctrl_off:stage.ctrl_off + stage.ctrl_size]
                        shared = struct.unpack_from("<I", control, 0x754)[0] if len(control) >= 0x758 else 0
                        local = struct.unpack_from("<3I", control, 0x748) if stage.kind == "compute" and len(control) >= 0x754 else (1, 1, 1)
                        signature = (stage.hash, stage_id, shared, *local)
                        index = stage_index.get(signature)
                        if index is None:
                            index = len(stages)
                            stage_index[signature] = index
                            stages.append(signature)
                        refs.append(index)
                    if refs:
                        programs.add(tuple(refs))

    # Keep the catalog deterministic and compact. Each stage entry is the SPIR-V key,
    # NVN stage, shared-memory size and compute workgroup dimensions; programs refer to it.
    ordered_programs = sorted(programs)
    with open(output, "wb") as f:
        f.write(CATALOG_MAGIC)
        f.write(struct.pack("<II", len(ordered_programs), len(stages)))
        for digest, stage, shared, x, y, z in stages:
            f.write(bytes.fromhex(digest))
            f.write(struct.pack("<B5xIIII", stage, shared, x, y, z))
        for refs in ordered_programs:
            f.write(struct.pack("<I", len(refs)))
            f.write(struct.pack("<%dI" % len(refs), *refs))
    print(f"Shader program catalog: {len(ordered_programs)} programs, {len(stages)} stage variants, "
          f"{os.path.getsize(output) >> 20} MiB (SHA-256 {hashlib.sha256(open(output, 'rb').read()).hexdigest()[:16]})",
          flush=True)


def extract_romfs(romfs, output):
    groups = (
        ("shader", "Shader/*.zs"),
        ("effect", "Effect/*.esetb.byml.zs"),
        ("layout", "Layout/*Nin_NX_NVN*.blarc*"),
        ("agl", "Lib/agl/*.sarc.zs"),
        ("gsys", "Lib/gsys/*.sarc.zs"),
    )
    total_archives = 0
    total_stages = 0
    recognized_groups = set()
    for group, pattern in groups:
        paths = sorted(glob.glob(os.path.join(romfs, pattern)))
        recognized = 0
        for path in paths:
            data = containers.read_file(path)
            archive = containers.open_blob(data, 0, len(data), os.path.basename(path))
            if archive is None:
                continue
            folder = os.path.join(output, group, os.path.basename(path).replace(".", "_"))
            files = containers.extract(archive, folder, data, unique=True)
            stages = sum(1 for _ in containers.all_stages(archive))
            print(f"{group}: {os.path.basename(path)} -> {stages} stages, {files} files", flush=True)
            recognized += 1
            total_archives += 1
            total_stages += stages
        print(f"{group}: recognized {recognized}/{len(paths)} archives", flush=True)
        if paths and recognized == 0:
            raise RuntimeError(f"No supported shader containers found in RomFS {group} files")
        if recognized:
            recognized_groups.add(group)
    missing_groups = {group for group, _ in groups} - recognized_groups
    if missing_groups:
        raise RuntimeError("RomFS is missing shader archive groups: " + ", ".join(sorted(missing_groups)))
    if total_archives == 0 or total_stages == 0:
        raise RuntimeError("No Wonder shader containers found; check that this is an extracted RomFS")
    write_program_catalog(romfs, os.path.join(output, "shader_programs.bin"))
    print(f"RomFS shader extraction complete: {total_archives} archives, {total_stages} stages", flush=True)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    romfs, output = map(os.path.abspath, sys.argv[1:])
    if not os.path.isdir(romfs):
        raise SystemExit(f"RomFS directory does not exist: {romfs}")
    # Game data never goes into the repository: inside it, only under private/ (a player's data root, outside the
    # repository or a release's game-build/ folder, is fine: tools/setup_game.py).
    repo_root = os.path.realpath(REPO)
    private_root = os.path.join(repo_root, "private")
    output_real = os.path.realpath(output)
    inside = lambda root: os.path.commonpath((root, output_real)) == root
    if inside(repo_root) and not inside(private_root):
        raise SystemExit(f"Shader extraction output inside the repository must stay under {private_root}")
    os.makedirs(output, exist_ok=True)
    extract_romfs(romfs, output)


if __name__ == "__main__":
    main()
