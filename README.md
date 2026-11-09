# SuperWonders — Super Mario Bros. Wonder PC Recompilation

SuperWonders is an experimental Windows PC project for running a native recompilation of *Super Mario Bros. Wonder* (Nintendo Switch, version 1.0.0). It builds on the DKB/TOTK recompilation framework and adapts its runtime and graphics work for Wonder. The Windows build runs the recompiled game code as native code and implements the required console-facing services and graphics calls on the PC, using Vulkan.

This repository contains project source code and documentation. It does not contain the game, a game dump, game assets, keys, firmware, or a compiled game.

## Current project work

- A Windows build starts the game and has been used for hands-on play in the tutorial area.
- The Windows runtime has an in-game menu opened with **F1**, divided into Video, Audio, and Controls sections.
- Video options include fullscreen and internal-resolution choices. The 720p and 1440p modes have been checked in the tutorial area.
- Audio volume can be adjusted in the menu.
- The menu language can be set to English, Italian, German, or French.
- Controller layouts include Xbox/DualSense and Nintendo button conventions. The Windows runtime supports keyboard input and XInput or DualSense controllers.
- The game can be launched directly from its executable, which uses the project's orange-flower icon.
- A Windows GUI Builder is packaged separately as a ZIP in [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases). It asks for the user's extracted ExeFS and RomFS folders, then builds a local runnable output. The Builder release does not include game files.

## Testing status

Testing so far has been limited to starting the game and playing in the tutorial area/world. In the sessions reported by the project owner, no visible graphics glitches were noticed there. The rest of the game, including later worlds, courses, cutscenes, and edge cases, has not been comprehensively tested. The current results should not be read as a claim that the full game has been verified.

## AI assistance

AI tools were used extensively to help write and revise code, build scripts, and documentation. The project owner directed the work, reviewed changes, built the project, and performed the gameplay checks described above. AI-generated changes can still contain bugs; the limited testing status is stated explicitly so users can judge the project's maturity.

## Getting a local build

The Builder ZIP in [Releases](https://github.com/vfjoo888/SuperWonders/releases) is the intended path for Windows users. It requires the user's own legally obtained and extracted game files and installs or uses the local development tools needed to compile the project. Do not share the generated game output: it contains material from the user's own game dump.

This repository is intended for source-code development. It does not provide instructions for obtaining game files, keys, or firmware.

## Project layout

- `recomp/` — AArch64 analysis and C++ code-generation tools.
- `runtime/` — native runtime, platform support, audio, input, and graphics implementation.
- `runtime/src/nvn/` — NVN-to-Vulkan graphics implementation.
- `games/wonder/` — Wonder-specific integration code.
- `tools/` — shader, packaging, and development utilities.
- `docs/` — technical notes, findings, and project history.

## Credits and licensing

Project source is licensed under the MIT License in [LICENSE](LICENSE). Third-party components and artwork are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The project's license covers only its own code; it grants no rights to Nintendo's game or trademarks. *Super Mario Bros. Wonder* and *Nintendo Switch* are trademarks of Nintendo. This is an unofficial fan project and is not affiliated with or endorsed by Nintendo.
