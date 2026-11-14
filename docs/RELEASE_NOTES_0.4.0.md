# SuperWonders 0.4.0

This release adds a native Linux x86-64 build alongside Windows, with a launcher for selecting game files, preparing the game, and configuring options. The Windows build can also be started directly from `wonder.exe`.

## Added

- Launcher-based setup using the user's own extracted ExeFS and RomFS folders.
- RecompLauncher profile with settings for frame-rate cap (30, 60, 90, 120, 144), internal resolution, V-Sync, fullscreen, FSR 1 upscaling, FXAA, anisotropic filtering, language, and Low VRAM mode.
- Linux x86-64 packages: AppImage and portable `.tar.gz`.
- Runtime graphics and startup improvements, including asynchronous pipeline compilation and cache saving, graphics pipeline library support where available, Vulkan validation fixes, and shader preparation at startup.
- Performance overlay, frame-time diagnostics, and input record/replay tools for development.
- Controller rumble support and expanded graphics options.

## Fixes and notes

- Fixed shadow alignment and flickering when using a non-original internal resolution.
- Reduced periodic Windows stalls caused by writing large amounts of redirected `stderr` output synchronously.
- The startup shader preparation covers the known shader catalog; shaders first encountered later can still compile during play.
- Hardware testing remains limited. NVIDIA and Intel compatibility have not been verified by the project maintainer; Linux GPU coverage is also limited.
- Use your own legally obtained game data. The source and release packages do not include game files, keys, firmware, or a compiled game.

## Credits

Thanks to [devall96](https://github.com/devall96) for the work and review shared in pull requests #20–#33. Selected ideas and implementations were reviewed and integrated into the 0.4.0 source and packages, including Linux support, asynchronous pipeline preparation, Vulkan fixes, graphics pipeline library support, launcher/release tooling, settings, and performance/replay diagnostics. The changes were integrated into the project source rather than merging the pull requests as a batch. PR #25's feedback-loop copy change was not included.
