#!/usr/bin/env python3
"""Build a personal Windows Wonder Recomp folder from an extracted ExeFS and RomFS.

The program only reads game files from the selected folders. Intermediate and generated files stay under private/.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import uuid


ROOT = Path(__file__).resolve().parent.parent
PRIVATE = ROOT / "private" / "wonder"
WORK = PRIVATE / "builder_work"
CACHE_PATH = PRIVATE / "builder_cache.json"
MARKER = ".wonder-recomp-builder"
CURRENT_STAGE = None
RYUBING_REVISION = "a82350bb77"


def log(message):
    print(message, flush=True)


def report_progress(percent):
    print(f"@@BUILD_PROGRESS:{max(0, min(100, int(percent)))}", flush=True)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint_files(files, root):
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda item: item.as_posix().lower()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        digest.update(bytes.fromhex(sha256_file(path)))
    return digest.hexdigest()


def fingerprint_tree(root):
    files = (path for path in root.rglob("*") if path.is_file() and
             not any(part.lower() in ("bin", "obj", "__pycache__") for part in path.relative_to(root).parts))
    return fingerprint_files(files, root)


def combine_fingerprints(*values):
    digest = hashlib.sha256()
    for value in values:
        encoded = str(value).encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
    return digest.hexdigest()


def shader_input_files(romfs):
    patterns = (
        "Shader/*.zs", "Effect/*.esetb.byml.zs", "Layout/*Nin_NX_NVN*.blarc*",
        "Lib/agl/*.sarc.zs", "Lib/gsys/*.sarc.zs",
    )
    return [path for pattern in patterns for path in romfs.glob(pattern) if path.is_file()]


def tree_has_files(path):
    return path.is_dir() and any(item.is_file() for item in path.rglob("*"))


def build_cache_keys(exefs, romfs):
    exefs_key = fingerprint_files((exefs / "main", exefs / "sdk"), exefs)
    romfs_shader_key = fingerprint_files(shader_input_files(romfs), romfs)

    nso_tool = ROOT / "tools" / "nso2bin.py"
    recomp_tree_key = fingerprint_tree(ROOT / "recomp")
    nso_tool_key = sha256_file(nso_tool)
    recomp_source_key = combine_fingerprints(recomp_tree_key, nso_tool_key)
    analysis_key = combine_fingerprints(exefs_key, sha256_file(nso_tool))
    analysis_modules = {
        module: combine_fingerprints(fingerprint_files((exefs / module,), exefs), sha256_file(nso_tool))
        for module in ("main", "sdk")
    }
    generated_modules = {
        module: combine_fingerprints(fingerprint_files((exefs / module,), exefs), recomp_source_key)
        for module in ("main", "sdk")
    }
    extractor_key = combine_fingerprints(
        exefs_key, romfs_shader_key,
        sha256_file(ROOT / "tools" / "maxwell" / "extract_romfs_shaders.py"),
        sha256_file(ROOT / "tools" / "maxwell" / "exefs_shaders.py"),
        sha256_file(ROOT / "tools" / "maxwell" / "containers.py"),
    )
    ryu_src = ROOT / "private" / "third_party" / "ryubing" / "src"
    translator_key = combine_fingerprints(
        fingerprint_tree(ROOT / "tools" / "maxwell" / "spirv_harness"),
        sha256_file(ROOT / "tools" / "maxwell" / "apply_patch.py"),
        fingerprint_tree(ryu_src / "Ryujinx.Graphics.Shader"),
        fingerprint_tree(ryu_src / "Ryujinx.Common"),
        fingerprint_tree(ryu_src / "Spv.Generator"),
        RYUBING_REVISION,
    )
    return {
        "exefs": exefs_key,
        "romfs_shaders": romfs_shader_key,
        "analysis": analysis_key,
        "analysis_main": analysis_modules["main"],
        "analysis_sdk": analysis_modules["sdk"],
        "generated_main": generated_modules["main"],
        "generated_sdk": generated_modules["sdk"],
        "generated": combine_fingerprints(generated_modules["main"], generated_modules["sdk"]),
        "generated_legacy": combine_fingerprints(exefs_key, recomp_tree_key, nso_tool_key),
        "shader_extract": extractor_key,
        "spirv": combine_fingerprints(extractor_key, translator_key),
    }


def load_cache():
    if not CACHE_PATH.is_file():
        return {"schema": 1}
    try:
        state = json.loads(CACHE_PATH.read_text(encoding="utf-8"))
        if state.get("schema") == 1:
            return state
    except (OSError, ValueError, AttributeError):
        pass
    log("The local build cache is missing or invalid; affected stages will be rebuilt.")
    return {"schema": 1}


def save_cache(state):
    CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = CACHE_PATH.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    temporary.replace(CACHE_PATH)


def remove_tree(path):
    if path.exists():
        shutil.rmtree(path)


def cache_ready(path, marker):
    return path.is_dir() and (path / marker).is_file()


def create_cache_marker(path, marker, key):
    path.mkdir(parents=True, exist_ok=True)
    (path / marker).write_text(key + "\n", encoding="ascii")


def adopt_legacy_build_cache(python, exefs, romfs, output, keys, state):
    if any(state.get(name) for name in ("analysis_key", "analysis_main_key", "analysis_sdk_key", "generated_key",
                                        "generated_main_key", "generated_sdk_key", "shader_extract_key", "spirv_key")):
        return state
    if not (output / MARKER).is_file():
        return state

    analysis_output = output / "data" / "analysis"
    if not (analysis_output / "main.bin").is_file() or not (analysis_output / "sdk.bin").is_file():
        return state
    generated = WORK / "gen"
    shaders = WORK / "shaders_extracted"
    spirv = WORK / "spirv"
    if not tree_has_files(generated / "main") or not tree_has_files(generated / "sdk"):
        return state

    log("Checking the previous managed output so its generated C++ can be reused.")
    check = WORK / "cache_adoption_check"
    remove_tree(check)
    check.mkdir(parents=True, exist_ok=True)
    run([python, "tools/nso2bin.py", exefs / "main", check / "main.bin"])
    run([python, "tools/nso2bin.py", exefs / "sdk", check / "sdk.bin"])
    matches = all(sha256_file(check / f"{module}.bin") == sha256_file(analysis_output / f"{module}.bin")
                  for module in ("main", "sdk"))
    if not matches:
        log("The selected ExeFS differs from the previous output; cached generated C++ will be rebuilt.")
        remove_tree(check)
        return state

    analysis = WORK / "analysis"
    remove_tree(analysis)
    check.rename(analysis)
    state["analysis_key"] = keys["analysis"]
    state["analysis_main_key"] = keys["analysis_main"]
    state["analysis_sdk_key"] = keys["analysis_sdk"]
    state["generated_key"] = keys["generated"]
    state["generated_main_key"] = keys["generated_main"]
    state["generated_sdk_key"] = keys["generated_sdk"]
    log("Previous ExeFS matches; adopting its decompressed modules and generated C++.")

    previous_romfs = output / "data" / "extract" / "romfs"
    if previous_romfs.is_dir() and fingerprint_files(shader_input_files(previous_romfs), previous_romfs) == keys["romfs_shaders"]:
        if tree_has_files(shaders):
            state["shader_extract_key"] = keys["shader_extract"]
            if ((spirv / "results.tsv").is_file() and any(spirv.glob("*.spv")) and
                    (output / "data" / "spirv" / "results.tsv").is_file()):
                state["spirv_key"] = keys["spirv"]
                log("Previous RomFS and SPIR-V data match; adopting the shader caches too.")
    save_cache(state)
    return state


def run(args, cwd=ROOT, env=None, progress_range=None):
    log("$ " + subprocess.list2cmdline([str(x) for x in args]))
    process = subprocess.Popen([str(x) for x in args], cwd=cwd, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1)
    assert process.stdout is not None
    progress_total = None
    progress_min, progress_max = progress_range or (0, 0)
    for line in process.stdout:
        line = line.rstrip("\r\n")
        if progress_range:
            total_match = re.search(r"(\d+) distinct code blocks", line)
            item_match = re.search(r"(?:^|\s)(\d+)/(\d+)(?:\s|$)", line)
            ninja_match = re.search(r"\[(\d+)/(\d+)\]", line)
            match = item_match or ninja_match
            if total_match:
                progress_total = int(total_match.group(1))
            elif match:
                done = int(match.group(1))
                total = int(match.group(2)) or progress_total
                if total:
                    report_progress(progress_min + (progress_max - progress_min) * done / total)
        print(line, flush=True)
    code = process.wait()
    if code:
        raise RuntimeError(f"Command failed with exit code {code}: {args[0]}")


def run_dev(commands, progress_range=None):
    ps = shutil.which("powershell.exe") or shutil.which("pwsh.exe")
    if not ps:
        raise RuntimeError("PowerShell was not found.")
    # dev.ps1 prepends the project's pinned Clang, CMake, Ninja, and Vulkan SDK tools to PATH.
    script = ". .\\dev.ps1; " + "; ".join(commands) + "; exit $LASTEXITCODE"
    run([ps, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        progress_range=progress_range)


def validate(exefs, romfs, output):
    if not exefs.is_dir() or not (exefs / "main").is_file() or not (exefs / "sdk").is_file():
        raise RuntimeError("ExeFS must contain the main and sdk files.")
    for module in ("main", "sdk"):
        with (exefs / module).open("rb") as f:
            if f.read(4) != b"NSO0":
                raise RuntimeError(f"ExeFS/{module} is not an NSO executable.")
    if not romfs.is_dir() or not (romfs / "Shader").is_dir() or not (romfs / "Effect").is_dir():
        raise RuntimeError("RomFS must be extracted and contain at least Shader and Effect folders.")
    out = output.resolve()
    if out == ROOT or ROOT in out.parents or PRIVATE in out.parents:
        raise RuntimeError("Choose an output folder outside the repository and its private build directory.")
    for source in (exefs.resolve(), romfs.resolve()):
        if source == out or source in out.parents or out in source.parents:
            raise RuntimeError("The output folder must not contain or overlap either selected game folder.")
    if output.exists() and any(output.iterdir()) and not (output / MARKER).is_file():
        raise RuntimeError("The output folder is not empty and was not created by this builder. Choose an empty folder.")
    ryubing = ROOT / "private" / "third_party" / "ryubing"
    required_ryubing_files = (
        ryubing / "src" / "Ryujinx.Graphics.Shader" / "Ryujinx.Graphics.Shader.csproj",
        ryubing / "src" / "Ryujinx.Common" / "Ryujinx.Common.csproj",
        ryubing / "src" / "Spv.Generator" / "Spv.Generator.csproj",
        ryubing / "src" / "Ryujinx.Graphics.Shader" / "CodeGen" / "Spirv" / "SpirvGenerator.cs",
    )
    ryubing_marker = ryubing / ".wonderbuilder-ryubing-commit"
    pinned_ryubing_commit = "a82350bb774f70fcbd41c9987bf67a3775409963"
    # Older setup versions extracted this exact pinned archive but did not write the marker.
    # Accept a complete unmarked tree; if a marker exists, it must still match the pin.
    if (not all(path.is_file() for path in required_ryubing_files)
            or (ryubing_marker.is_file()
                and ryubing_marker.read_text(encoding="ascii").strip() != pinned_ryubing_commit)):
        raise RuntimeError(
            "The Ryubing shader translator source is missing or incomplete. "
            "Run Install prerequisites and wait for the source download to finish, then build again."
        )
    if not shutil.which("dotnet"):
        raise RuntimeError(".NET SDK 10 is required to translate the game's shaders.")
    python = ROOT / "private" / "venv" / "Scripts" / "python.exe"
    if not python.is_file():
        raise RuntimeError("The project Python environment is missing. Set it up using the repository's Windows instructions.")
    return python


def ensure_local_button_font(python):
    font = PRIVATE / "fonts" / "shared_5.ttf"
    if font.is_file():
        return
    windows = Path(os.environ.get("WINDIR", r"C:\Windows"))
    base_font = windows / "Fonts" / "arial.ttf"
    if not base_font.is_file():
        base_font = windows / "Fonts" / "segoeui.ttf"
    if not base_font.is_file():
        raise RuntimeError("Could not find Arial or Segoe UI in the Windows Fonts folder to generate the local button font.")
    log("The local button font is missing; generating it from a Windows system font.")
    run([python, "tools/fonts/make_button_font.py", "--base", base_font, "--out", font])
    if not font.is_file():
        raise RuntimeError("Button font generation finished without creating private/wonder/fonts/shared_5.ttf.")


def spirv_val_executable():
    env_root = os.environ.get("VULKAN_SDK")
    candidates = [Path(env_root) / "Bin" / "spirv-val.exe"] if env_root else []
    candidates += sorted(Path(r"C:\VulkanSDK").glob("*/Bin/spirv-val.exe"), reverse=True)
    found = next((path for path in candidates if path.is_file()), None)
    if found:
        return found
    found = shutil.which("spirv-val.exe") or shutil.which("spirv-val")
    if found:
        return found
    raise RuntimeError("spirv-val was not found. Install the Vulkan SDK and set VULKAN_SDK.")


def reuse_or_copy(source, destination, previous=None):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if previous is not None and previous.is_file():
        source_stat = source.stat()
        previous_stat = previous.stat()
        if source_stat.st_size == previous_stat.st_size and source_stat.st_mtime_ns == previous_stat.st_mtime_ns:
            try:
                os.link(previous, destination)
            except OSError:
                shutil.copy2(previous, destination)
            return True
    shutil.copy2(source, destination)
    return False


def copy_tree(source, destination, label, progress_range=None, reference=None):
    files = sorted(p for p in source.rglob("*") if p.is_file())
    destination.mkdir(parents=True, exist_ok=True)
    log(f"Copying {label}: {len(files)} files")
    progress_min, progress_max = progress_range or (0, 0)
    progress_every = max(1, len(files) // 20)
    reused = 0
    for index, source_file in enumerate(files, 1):
        target = destination / source_file.relative_to(source)
        old_file = reference / source_file.relative_to(source) if reference is not None else None
        reused += reuse_or_copy(source_file, target, old_file)
        if progress_range and (index % progress_every == 0 or index == len(files)):
            report_progress(progress_min + (progress_max - progress_min) * index / max(1, len(files)))
        if index % 1000 == 0 or index == len(files):
            log(f"{label}: {index}/{len(files)} files")
    log(f"{label}: {reused} unchanged files reused from the previous output.")


def redist_files():
    roots = [Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"),
             Path(r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC")]
    hits = sorted(p for root in roots if root.is_dir() for p in root.glob("*/x64/Microsoft.VC14*.CRT"))
    if not hits:
        raise RuntimeError("The Visual Studio x64 C++ runtime redistributable is missing.")
    folder = hits[-1]
    names = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
    files = [folder / name for name in names]
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        raise RuntimeError("Missing Visual C++ runtime DLLs: " + ", ".join(missing))
    return files


def replace_managed_output(stage, output):
    output.mkdir(parents=True, exist_ok=True)
    backup = output.parent / (".wonder-builder-backup-" + uuid.uuid4().hex)
    backup.mkdir()
    targets = ("wonder.exe", "Play Wonder.bat", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
               "data/analysis", "data/extract/romfs", "data/spirv", "data/shader_programs.bin", "data/fonts")
    moved_old = []
    moved_new = []
    try:
        for relative in targets:
            destination = output / relative
            source = stage / relative
            if destination.exists():
                saved = backup / relative
                saved.parent.mkdir(parents=True, exist_ok=True)
                destination.rename(saved)
                moved_old.append((saved, destination))
            if source.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                source.rename(destination)
                moved_new.append(destination)
        (output / MARKER).write_text("Created by WonderBuilder. Managed game output; user saves are preserved.\n", encoding="utf-8")
    except Exception:
        for destination in reversed(moved_new):
            if destination.is_dir():
                shutil.rmtree(destination, ignore_errors=True)
            else:
                destination.unlink(missing_ok=True)
        for saved, destination in reversed(moved_old):
            destination.parent.mkdir(parents=True, exist_ok=True)
            if saved.exists():
                saved.rename(destination)
        raise
    finally:
        shutil.rmtree(backup, ignore_errors=True)


def main():
    global CURRENT_STAGE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", required=True)
    parser.add_argument("--romfs", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--workers", default="auto",
                        help="Auto or a worker count from 1 to the number of available logical processors")
    args = parser.parse_args()
    processor_count = max(1, os.cpu_count() or 1)
    if args.workers.lower() == "auto":
        # Keep the default responsive on typical PCs while leaving headroom for the OS and GPU driver.
        workers = max(1, min(8, processor_count // 2))
        log(f"Auto workers selected {workers} of {processor_count} available logical processors.")
    else:
        try:
            workers = int(args.workers)
        except ValueError:
            parser.error("--workers must be Auto or a positive integer")
        if not 1 <= workers <= processor_count:
            parser.error(f"--workers must be between 1 and {processor_count} on this PC")
    exefs, romfs, output = Path(args.exefs).resolve(), Path(args.romfs).resolve(), Path(args.output).resolve()
    python = validate(exefs, romfs, output)
    report_progress(1)
    ensure_local_button_font(python)
    report_progress(2)
    output.parent.mkdir(parents=True, exist_ok=True)
    state = load_cache()
    has_existing_cache = any(state.get(name) for name in
                             ("analysis_key", "analysis_main_key", "analysis_sdk_key", "generated_key",
                              "generated_main_key", "generated_sdk_key", "shader_extract_key", "spirv_key"))
    if not has_existing_cache and not (output / MARKER).is_file():
        remove_tree(WORK)
    WORK.mkdir(parents=True, exist_ok=True)
    keys = build_cache_keys(exefs, romfs)
    state = adopt_legacy_build_cache(python, exefs, romfs, output, keys, state)

    harness = ROOT / "private" / "build" / "spirv_harness" / "bin" / "Release" / "net10.0" / "spirv_harness.dll"
    log(f"Checking the local shader translator (parallel workers: {workers}).")
    run(["dotnet", "build", "tools/maxwell/spirv_harness/SpirvHarness.csproj", "-c", "Release", f"-m:{workers}"])
    if not harness.is_file():
        raise RuntimeError("The shader translator did not produce spirv_harness.dll.")
    report_progress(3)

    analysis = WORK / "analysis"
    generated = WORK / "gen"
    shaders = WORK / "shaders_extracted"
    spirv = WORK / "spirv"
    stage = output.parent / (".wonder-builder-stage-" + uuid.uuid4().hex)
    CURRENT_STAGE = stage
    (stage / "data" / "analysis").mkdir(parents=True)
    log("Step 1/6: checking the decompressed ExeFS module cache.")
    analysis.mkdir(parents=True, exist_ok=True)
    for module in ("main", "sdk"):
        analysis_file = analysis / f"{module}.bin"
        module_key = f"analysis_{module}_key"
        # Older cache files used one key for both modules. Upgrade those entries
        # without decompressing again when the aggregate fingerprint still matches.
        if (state.get(module_key) != keys[f"analysis_{module}"] and
                state.get("analysis_key") == keys["analysis"] and analysis_file.is_file()):
            state[module_key] = keys[f"analysis_{module}"]
        if state.get(module_key) == keys[f"analysis_{module}"] and analysis_file.is_file():
            log(f"Cache hit: reusing the decompressed {module} module.")
            continue
        log(f"Cache miss: decompressing {module}; its source or tool fingerprint changed, or its output is missing.")
        run([python, "tools/nso2bin.py", exefs / module, analysis_file])
        state[module_key] = keys[f"analysis_{module}"]
        save_cache(state)
        report_progress(5 if module == "main" else 8)
    state["analysis_key"] = keys["analysis"]
    save_cache(state)
    report_progress(10)

    log("Step 2/6: checking generated C++ module caches.")
    generated.mkdir(parents=True, exist_ok=True)
    legacy_generated_valid = (state.get("generated_key") in (keys["generated"], keys["generated_legacy"]) and
                              tree_has_files(generated / "main") and tree_has_files(generated / "sdk"))
    for module, base in (("main", "0x4000000000"), ("sdk", "0x4010000000")):
        module_output = generated / module
        module_key = f"generated_{module}_key"
        if (state.get(module_key) != keys[f"generated_{module}"] and legacy_generated_valid):
            state[module_key] = keys[f"generated_{module}"]
        if state.get(module_key) == keys[f"generated_{module}"] and tree_has_files(module_output):
            log(f"Cache hit: reusing generated C++ for {module}.")
            continue
        log(f"Cache miss: recompiling {module}; its source, generator, or output fingerprint changed.")
        remove_tree(module_output)
        run([python, "-m", "recomp", exefs / module, module_output,
             "--name", module, "--base", base])
        state[module_key] = keys[f"generated_{module}"]
        save_cache(state)
        report_progress(25 if module == "main" else 40)
    state["generated_key"] = keys["generated"]
    save_cache(state)
    report_progress(40)

    shaders_valid = (state.get("shader_extract_key") == keys["shader_extract"] and
                     any(shaders.rglob("code_*.bin")) and any(shaders.rglob("ctrl_*.bin")) and
                     (shaders / "shader_programs.bin").is_file())
    if shaders_valid:
        log("Cache hit: reusing extracted shader programs.")
    else:
        log("Step 3/6: extracting shader programs from the selected RomFS and main module.")
        remove_tree(shaders)
        shaders.mkdir(parents=True)
        run([python, "tools/maxwell/extract_romfs_shaders.py", romfs, shaders])
        report_progress(45)
        run([python, "tools/maxwell/exefs_shaders.py", analysis / "main.bin", "--extract", shaders / "exefs"])
        state["shader_extract_key"] = keys["shader_extract"]
        save_cache(state)
    report_progress(50)

    spirv_valid = (state.get("spirv_key") == keys["spirv"] and
                   (spirv / "results.tsv").is_file() and any(spirv.glob("*.spv")))
    if spirv_valid:
        log("Cache hit: reusing translated SPIR-V shaders.")
    else:
        log(f"Step 4/6: translating shaders with {workers} workers (this can take several minutes).")
        remove_tree(spirv)
        spirv.mkdir(parents=True)
        run(["dotnet", harness, "--batch", shaders, "--out", spirv, "--jobs", str(workers), "--spirv-val",
             spirv_val_executable()], progress_range=(50, 70))
        if not (spirv / "results.tsv").is_file() or not any(spirv.glob("*.spv")):
            raise RuntimeError("Shader translation finished without SPIR-V output.")
        state["spirv_key"] = keys["spirv"]
        save_cache(state)
    report_progress(71)

    log("Step 5/6: compiling the Windows executable.")
    build_dir = PRIVATE / "build"
    gen_dir = generated
    config = ["cmake -S . -B " + '"' + str(build_dir) + '"' + " -G Ninja -DNX_GAME=wonder "
              + "-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl "
              + "-DNX_GEN_DIR=" + '"' + str(gen_dir) + '"' + "; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"]
    build = ["cmake --build " + '"' + str(build_dir) + '"' + " --target wonder --parallel " + str(workers)]
    run_dev(config + build, progress_range=(71, 90))
    executable = build_dir / "wonder.exe"
    if not executable.is_file():
        raise RuntimeError("CMake completed but wonder.exe was not created.")
    report_progress(90)

    log("Step 6/6: assembling the runnable output folder.")
    (stage / "data" / "analysis").mkdir(parents=True, exist_ok=True)
    copy_tree(analysis, stage / "data" / "analysis", "analysis modules", reference=output / "data" / "analysis")
    copy_tree(romfs, stage / "data" / "extract" / "romfs", "RomFS", progress_range=(91, 96),
              reference=output / "data" / "extract" / "romfs")
    copy_tree(spirv, stage / "data" / "spirv", "SPIR-V", progress_range=(96, 99),
              reference=output / "data" / "spirv")
    catalog = shaders / "shader_programs.bin"
    if not catalog.is_file():
        raise RuntimeError("Shader program catalog is missing; rebuild the local shader archive cache.")
    shutil.copy2(catalog, stage / "data" / "shader_programs.bin")
    copy_tree(PRIVATE / "fonts", stage / "data" / "fonts", "fonts", reference=output / "data" / "fonts")
    reuse_or_copy(executable, stage / "wonder.exe", output / "wonder.exe")
    for dll in redist_files():
        reuse_or_copy(dll, stage / dll.name, output / dll.name)
    replace_managed_output(stage, output)
    report_progress(100)
    shutil.rmtree(stage, ignore_errors=True)
    CURRENT_STAGE = None
    log(f"Build complete. Start the game directly with: {output / 'wonder.exe'}")
    log("The selected game dump and all generated content remain local to your PC.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        log("Build cancelled.")
        raise SystemExit(130)
    except Exception as exc:
        if CURRENT_STAGE is not None:
            shutil.rmtree(CURRENT_STAGE, ignore_errors=True)
        log(f"BUILD FAILED: {exc}")
        raise SystemExit(1)
