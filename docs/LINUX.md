# Wonder on Linux (x86-64)

A native Linux build of the runtime: `runtime/src/platform_linux.cpp` (SDL3 window, input, audio and Vulkan surface,
Wayland or X11) and `runtime/src/memguard_linux.cpp`. Tested on Ubuntu 26.04, Ryzen 7 7800X3D, RX 9070 XT (RADV),
GNOME on Wayland.

## What it needs

```sh
sudo apt install clang lld cmake ninja-build python3-venv libsdl3-dev libvulkan-dev mesa-vulkan-drivers
```

The Vulkan headers have to match the registry `vk_gen.{h,cpp}` were generated from (1.4.357). Distributions ship
older ones, so CMake fetches Khronos' Vulkan-Headers at tag `v1.4.357` unless `VULKAN_SDK` points to a LunarG SDK.

## Data

The same `private/wonder` layout as on Windows (from your own dump only; never upload or share it):

| Path | From |
|---|---|
| `exefs/{main,sdk,main.npdm}` | the game's ExeFS |
| `extract/romfs/` | the game's RomFS |
| `analysis/{main,sdk}.bin` | `tools/nso2bin.py` |
| `spirv/*.spv`, `spirv/results.tsv`, `shader_programs.bin` | the shader harness (`docs/nvn/SHADERS.md`) |
| `fonts/shared_5.ttf` | the button glyph font |

`analysis`, `spirv`, `shader_programs.bin` and `fonts` do not depend on the host: copying them from a Windows or Mac
checkout works. Without `fonts/`, the shared font falls back to Noto Sans, Liberation Sans or DejaVu Sans.

## Recompile and build

```sh
python3 -m venv private/venv && private/venv/bin/pip install capstone lz4 pillow
private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000
private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
cmake -S . -B private/wonder/build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
ninja -C private/wonder/build -j12 wonder
```

Keep `-j` at or below (RAM in GB) / 2. Everything is built with `-march=x86-64-v3` (AVX2, as on Windows, plus
CMPXCHG16B: without it the recompiled 128-bit `__atomic_compare_exchange_n` becomes an unresolved libatomic call).

## Run

```sh
NX_LOG=private/wonder/nx.log private/wonder/build/wonder
```

Settings are `NX_<KEY>` environment variables, as on macOS (`NX_RESOLUTION=1920x1080`, `NX_FULLSCREEN=1`,
`NX_MUTE=1`, `NX_HEADLESS=1`, `NX_MEM_LIMIT_GB`, ...). F11 toggles fullscreen. Keyboard: WASD or arrows, Space A,
Shift B, J X, K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -; the first SDL gamepad is player 1.

`tools/wonder_course.sh <name> [seconds] [--windowed] [--course11]` is the scripted course run
(`tools/wonder_course.ps1` on Windows).

## Differences from Windows

- **No F1 settings menu.** `pc_menu_win.cpp` is Win32 drawing code; there is no Linux menu yet, and no metrics
  overlay. `pc-settings.ini` is not read. A portable menu (SDL or an in-frame overlay) is a follow-up.
- **Memory guard** reads `RssAnon + VmSwap` from `/proc/self/status` (the nearest thing to Windows' private commit;
  it leaves out the GPU driver's mappings).
- **Address space.** The guest ranges are the Windows/Android ones (`NX_BASE` 0x40_0000_0000, heap 0x50, alias 0x58,
  stacks 0x68), reserved with `MAP_FIXED_NOREPLACE`. The executable is linked `-no-pie`, so it loads at 0x40_0000.
- `plat::profile` is not implemented (use `perf`).

## AMD GPUs (RADV)

Upstream has only been tested on NVIDIA. Found on an RX 9070 XT and the Ryzen 7800X3D's RDNA2 iGPU:

- **Fixed:** empty shadow-map slots got the colour dummy texture (`nvn_dummy.cpp`: dummies keyed by view type only),
  and shadow samplers read colour textures (`nvn_compare_twin.cpp`: D32 twins). The opening's first scene was black
  before; it is right now, and the validation layer reports no depth-compare errors.
- **Open: course 1-1 and the later opening scenes draw their lit foreground black** (the ground, the characters, the
  enemies). The background, coins, blocks, HUD and world map are right. Findings so far:
  - the colour-pass draws run and write (0, 0, 0): the depth test passes, the shading gives black;
  - not depth precision (`NX_EQUAL_SLACK`, `radv_invariant_geom`), not sync (`NX_ASYNC_GPU=0 NX_PIPELINED=0`), not
    compression or memory (`RADV_DEBUG=nodcc,nohiz,nofastclears,zerovram`), not the shader compiler
    (`RADV_DEBUG=llvm`), not NaN from FMin/FMax/FClamp (rewriting them to NMin/NMax/NClamp changed nothing), no
    `OpUndef`, no subgroup operations in those shaders;
  - the same on RDNA4 and RDNA2, so AMD-wide, and the runtime has no vendor-specific paths.
  - Next: dump the colour pass's lighting inputs (64x64 / 128x128 R11G11B10F render targets, the R8 64x64 one) on an
    NVIDIA GPU and compare; or run the course on llvmpipe as a reference.
