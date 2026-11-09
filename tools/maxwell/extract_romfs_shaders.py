#!/usr/bin/env python3
"""Extract unique Maxwell shader stages from an already extracted Wonder RomFS.

Usage: python tools/maxwell/extract_romfs_shaders.py <romfs> <output-under-private>
"""
import glob
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, REPO)
from tools.maxwell import containers


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
    print(f"RomFS shader extraction complete: {total_archives} archives, {total_stages} stages", flush=True)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    romfs, output = map(os.path.abspath, sys.argv[1:])
    if not os.path.isdir(romfs):
        raise SystemExit(f"RomFS directory does not exist: {romfs}")
    private_root = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", "..", "private"))
    output_real = os.path.realpath(output)
    if os.path.commonpath((private_root, output_real)) != private_root:
        raise SystemExit(f"Shader extraction output must stay under {private_root}")
    os.makedirs(output, exist_ok=True)
    extract_romfs(romfs, output)


if __name__ == "__main__":
    main()
