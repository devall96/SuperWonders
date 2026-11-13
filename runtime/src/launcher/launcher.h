// SuperWonders Launcher: the start screen shown before the game boots, after Savage Wind's (github.com/Revivedx/
// SavageWindRecomp, MIT: runtime/ui/launcher.mm), in SuperWonders' own look (overlay/overlay_ui.h).
//
// Two ways it shows:
//   - setup: the game isn't set up (no game code library, no shaders...), or this version needs it set up again (an
//     update brought a new game-build kit). The player picks the folders of their own ExeFS and RomFS and presses
//     Set up game files; tools/setup_game.py does the work (game-build/ in a release), with its progress on screen.
//   - configuration (--config): PLAY, GAME FILES, SETTINGS (the overlay menu over the launcher), MODS, UPDATES, QUIT.
// Once set up, a plain start goes straight into the game. Never shown with --play (what RecompLauncher's profile starts:
// it ran the setup itself, recomplauncher.json), NX_NO_LAUNCHER=1, NX_HEADLESS or a scripted run (NX_INPUT_SCRIPT).
// After an update whose game-build kit builds the game code differently, it builds it again by itself.
#pragma once

namespace nx::launcher {

// Before the game boots, on the thread that will run it (nx::boot, after the guest memory is reserved). Returns when
// the player presses Play (or at once when the launcher isn't wanted); Quit ends the process.
void run();

}  // namespace nx::launcher
