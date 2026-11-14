// Test play tooling (play.cpp, docs/PLAY-TOOLS.md): input keyed to the game's own frames, recording and replaying a
// player's input per game frame, a probe of the game's state (course, player position, deaths, goal) and a memory
// scanner to find that state in a new game version.
#pragma once

#include <cstdint>
#include <string>

#include "platform.h"

namespace nx::play {

// hle_hid: after the platform's pad (and NX_INPUT_SCRIPT) is read, on every pad read of the game. Adds the frame
// script / replay (NX_INPUT_FRAMES, NX_INPUT_REPLAY) and records what the game gets (NX_INPUT_RECORD=<path>).
void pad(plat::PadState& p);
// nvn_queue: after each game present (g_frame was just counted). Samples the probe, fires anchors, runs scan steps.
void frame();
// nvn frame dumps: the name of a shot due now (NX_PLAY_SHOTS), once; nullptr otherwise. The presenting thread.
const char* take_shot();
// hle_fs: a guest file was opened (course and world names come from the files a scene loads).
void file_opened(const std::string& guest_path);

}  // namespace nx::play
