# SuperWonders — Super Mario Bros. Wonder PC Recompilation

I'm working on SuperWonders, an experimental Windows PC project that recompiles *Super Mario Bros. Wonder* (Nintendo Switch, version 1.0.0) for native execution on PC. I started from the DKB/TOTK recompilation framework and have been adapting its runtime and graphics support for Wonder. The Windows graphics backend uses Vulkan.

This repository contains source code and project documentation. It does not contain the game, a game dump, game assets, keys, firmware, or a compiled game.

## What I've done

- I got a Windows build running and tested it by playing in the game's tutorial area.
- I added an in-game menu opened with **F1**, organized into Video, Audio, and Controls sections.
- The Video section includes fullscreen and internal-resolution options. I checked the 720p and 1440p settings in the tutorial area.
- The Audio section lets me adjust the game volume.
- The menu can be displayed in English, Italian, German, or French.
- The Controls section has Xbox/DualSense and Nintendo button-layout presets. The Windows runtime includes keyboard, XInput, and DualSense input support.
- I changed the Windows build so the game can be started directly from its executable, with an orange-flower application icon.
- I prepared a separate Windows GUI Builder package. It will be distributed as a ZIP in [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases); it builds a local game output from ExeFS and RomFS folders supplied by the user.

## Testing and compatibility

I have tested the project on a Windows PC with an **NVIDIA GeForce RTX 5060 Ti**. So far, I have played only in the tutorial area. I haven't noticed visible graphics glitches there, but I have not comprehensively tested the rest of the game, including later worlds, courses, cutscenes, and edge cases.

**AMD and Intel graphics cards have not been tested**, so I can't confirm compatibility with them. Other systems and GPUs may behave differently.

## AI assistance

I used AI tools extensively to help write and revise the code, build scripts, and documentation. I directed the work and ran the builds and gameplay checks described above. AI-assisted code can still have bugs; the limited testing coverage is part of the project's current status.

## Getting a local build

The Windows GUI Builder ZIP will be available from [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases). It asks for the user's extracted ExeFS and RomFS folders and uses local development tools to compile a runnable output. The ZIP does not include game files.

You must provide your own legally obtained game files. I don't provide instructions for obtaining game files, keys, or firmware. Do not share the generated game output, because it contains material from your own game dump.

## Project layout

- `recomp/` — AArch64 analysis and C++ code-generation tools.
- `runtime/` — native runtime, platform support, audio, input, and graphics implementation.
- `runtime/src/nvn/` — NVN-to-Vulkan graphics implementation.
- `games/wonder/` — Wonder-specific integration code.
- `tools/` — shader, packaging, and development utilities.
- `docs/` — technical notes, findings, and project history.

## Credits and licensing

Project source is licensed under the MIT License in [LICENSE](LICENSE). Third-party components and artwork are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The project's license covers only its own code; it grants no rights to Nintendo's game or trademarks. *Super Mario Bros. Wonder* and *Nintendo Switch* are trademarks of Nintendo. This is an unofficial fan project and is not affiliated with or endorsed by Nintendo.
