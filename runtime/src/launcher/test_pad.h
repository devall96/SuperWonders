// A scripted controller for tests (launcher/test_pad.cpp): an SDL virtual gamepad, which SDL and the platforms' pad
// code (read_pad) see as a real one, pressed by NX_TEST_PAD, frame by frame:
//   NX_TEST_PAD="<frame>:<button>[:<frames held>];..."     e.g. "30:down;40:east;60:start"
// Buttons by position (SDL's names): south east west north up down left right l r zl zr start back, and the left stick
// pushed: stick-left stick-right stick-up stick-down. Held 6 frames unless given. Off without the variable.
#pragma once

#include <cstdint>

namespace nx::launcher {

void test_pad_start();                // attaches the virtual gamepad when NX_TEST_PAD is set
void test_pad_frame(uint64_t frame);  // sets its buttons for this frame (call once per frame, before reading the pad)

}  // namespace nx::launcher
