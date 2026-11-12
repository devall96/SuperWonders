# SuperWonders — Super Mario Bros. Wonder PC Recompilation

I'm working on SuperWonders, an experimental Windows x64 static recompilation of *Super Mario Bros. Wonder* (Nintendo Switch, base version 1.0.0) for native execution on PC. I started from the DKB/TOTK recompilation framework and have been adapting its runtime and graphics support for Wonder. The Windows graphics backend uses Vulkan.

This repository contains source code and project documentation. It does not contain the builder package, the game, game assets, a game dump, keys, firmware, or a compiled game. The separate Windows GUI Builder is distributed through [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases).

## What I've done

- I got a Windows build running and tested it by playing in the game's tutorial area.
- I added a first-launch shader preparation screen. It compiles the known shader program catalog before the first game frame; the current catalog contains about 12,000 programs. The Vulkan pipeline cache is saved locally for later launches. Shader variants outside the catalog can still compile the first time they are encountered, so some first-use stutter may remain.
- I fixed the misaligned and flickering shadows I saw when changing the internal resolution, and checked the result at multiple internal resolutions in the tutorial area.
- I added an in-game menu opened with **F1**, organized into Video, Audio, Controls, Overlay, and System sections.
- The Video section includes window/output size, fullscreen, always-on-top, and internal-resolution options.
- The Audio section lets me adjust game volume and select the game language. The settings menu itself is available in English, Italian, German, and French.
- The Controls section has Xbox/DualSense and Nintendo button-layout presets. It also lets me remap keyboard keys and controller buttons. The Windows runtime includes keyboard, XInput, and DualSense input support.
- The Overlay section has separate toggles for FPS, 1% lows, game speed, and a frametime graph, with four corner-position options.
- The System section includes a configurable process memory limit and a Low VRAM profile. The memory limit controls the process's system-memory guard; it is not a dedicated VRAM limit. The Low VRAM profile can store decoded ASTC textures as BC3 instead of expanding them to RGBA8 when the GPU does not support ASTC. This can reduce texture memory use, with a possible image-quality tradeoff.
- I prepared a separate Windows GUI Builder package. Users can provide extracted ExeFS/RomFS folders or a base NSP/XCI plus their own `prod.keys`; the builder performs extraction locally.
- I changed the Windows build so the game can be started directly from its executable, with an orange-flower application icon.

## Testing and compatibility

I have tested the project on a Windows PC with an **NVIDIA GeForce RTX 5060 Ti**. So far, I have played only in the tutorial area and checked several internal-resolution settings there. I haven't comprehensively tested the rest of the game, including later worlds, courses, cutscenes, and edge cases.

**AMD and Intel graphics cards have not been tested**, so I can't confirm compatibility with them. Other systems and GPUs may behave differently. Controller rumble is not currently supported.

The recompilation targets the game's base version **1.0.0**. The builder accepts NSP/XCI files but does not yet identify and display the game version automatically. Updates and DLC are not supported by this input path.

## Minimum hardware (provisional)

I have not established a formally tested minimum yet. The current Windows build expects:

- **CPU:** a 64-bit x86 processor with AVX2 support.
- **GPU:** a graphics card and driver supporting Vulkan 1.3. The minimum amount of video memory has not been verified.
- **System RAM:** 16 GB as a practical baseline. 32 GB is recommended when building the project, especially with a higher worker count; lowering the worker count can reduce build memory use.

These are preliminary requirements inferred from the build and runtime, not a guarantee that every compatible-looking PC will run the game well. Testing is still limited to the hardware and game area described above.

## Future features

I'm planning to add support for ultrawide **21:9** and super-ultrawide **32:9** displays.

A Linux port may be a future project after I have improved and tested the Windows experience. I don't have a Linux system, so I will need someone to test it actively. Android is substantially more complicated and may be considered later.

## AI assistance

I used AI tools extensively to help write and revise the code, build scripts, and documentation. I directed the work and ran the builds and gameplay checks described above. AI-assisted code can still have bugs; the limited testing coverage is part of the project's current status.

## Getting a local build

Download the Windows GUI Builder ZIP from [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases), extract it to a writable folder, and run `Start WonderBuilder.cmd`.

In the builder, choose either your own extracted ExeFS and RomFS folders, or choose **NSP / XCI file** and provide your own base-game dump plus the `prod.keys` file from your own console. The builder extracts NSP/XCI data locally. Choose an output folder and click **Build game**; when it finishes, start `wonder.exe` from that output folder.

The builder ZIP includes its portable Windows build tools, but no game files or keys. You must provide your own legally obtained game files. I don't provide instructions for obtaining game files, keys, or firmware. Do not share the generated game output, because it contains material from your own game dump.

To update a build, keep the builder folder and output folder, extract the newer builder package over the old builder files, and build again with the same inputs and output path. The builder can reuse unchanged extraction and compilation caches.

## Project layout

- `recomp/` — AArch64 analysis and C++ code-generation tools.
- `runtime/` — native runtime, platform support, audio, input, and graphics implementation.
- `runtime/src/nvn/` — NVN-to-Vulkan graphics implementation.
- `games/wonder/` — Wonder-specific integration code.
- `tools/` — shader, packaging, and development utilities.
- `docs/` — technical notes, findings, and project history.

## Credits and licensing

Project source is licensed under the MIT License in [LICENSE](LICENSE). Third-party components and artwork are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The project's license covers only its own code; it grants no rights to Nintendo's game or trademarks. *Super Mario Bros. Wonder* and *Nintendo Switch* are trademarks of Nintendo. This is an unofficial fan project and is not affiliated with or endorsed by Nintendo.
