# Performance suite: frame times, not averages

An average of 60 fps can hold a 160 ms frame. Players feel the slow frames: stutter, hitches and poor 1% lows. This
suite records every presented frame and reports those. Use it for every performance claim and every change that could
affect frame pacing, and compare against a baseline.

The design follows Savage Wind's frame trace and hitch analysis (Revivedx/SavageWindRecomp: `runtime/perfcount.h`,
`tools/perf_routes.py`, `docs/PERF-ROUTES.md`): record what each frame did, then name the cause of each slow frame.

## Recording: `NX_FRAMETRACE=<file.csv>`

One row per frame presented to the display (`runtime/src/frametrace.h`). The counters are always on (one atomic add
at events that cost far more); nothing is written without the variable.

| column | meaning |
|---|---|
| `frame`, `t_ms`, `frame_ms` | row number, time since the first present, time since the previous present |
| `acquire_ms`, `queue_present_ms` | `vkAcquireNextImageKHR` and `vkQueuePresentKHR` in this frame (swapchain blocking) |
| `replay_ms`, `encode_ms`, `fence_ms` | the presenting thread recording the game's commands, `vkQueueSubmit`, waiting for the GPU |
| `upload_mb` | bytes through the upload buffer |
| `present_cpu_ms` | CPU time of the presenting thread |
| `guest_presents`, `guest_gap_ms`, `guest_cpu_ms` | the game's own presents since the last row, the gap between its last two, its thread's CPU time |
| `guest_replay_wait_ms`, `guest_pace_ms`, `guest_interval` | the game waiting for the replay (at most two frames ahead), waiting for the vsync pacing, and its present interval (2 = 30 fps) |
| `pipelines`, `pipeline_ms`, `skipped` | pipelines created (any thread) and their time, draws skipped while compiling |
| `astc`, `astc_ms` | ASTC textures decoded on the CPU |
| `file_reads`, `file_mb`, `file_ms` | guest file and RomFS reads |
| `minflt`, `majflt` | page faults (Windows: one count, in `minflt`) |
| `log_line` | log lines written so far: the report prints a hitch's log lines from the run's `NX_LOG` |

**On screen.** With a real window and a GPU that has `VK_KHR_present_id` and `VK_KHR_present_wait`, the time each frame
reached the display goes to `<file.csv>.display.csv` (`frame,displayed_ms`, same clock). A present that is late by a few
ms only stutters if it misses the vblank; these times say whether it did. Not with SDL's offscreen driver (Mesa's
headless surface offers present wait, but waiting crashes in the driver).

## Reporting: `tools/perf/frametrace_report.py`

    python tools/perf/frametrace_report.py run.csv --log run.log [--from-s 5] [--to-s 80] [--target-fps 60]
                                           [--json run.json] [--baseline base.json]

- **avg fps**, **1% low** and **0.1% low** (the average fps of the slowest 1% / 0.1% of frames), frame-time
  p50/p90/p99/p99.9/max.
- **Hitches** by size (frames over 25, 33, 50 and 100 ms), and frames over twice the median and 25 ms with a **cause**:
  - `swapchain_acquire` or `queue_present`: the swapchain blocked;
  - `gpu_wait`: waiting for the GPU;
  - `pipeline_build`, `astc_decode`, `replay_cpu`: the presenting thread's work;
  - `game_file_read`, `game_logic_cpu`, `game_waits_replay`, `game_late_other`: the game's own frame came late;
  - `game_asked_30fps`: the game's own present interval 2, as on the Switch, not a stall;
  - otherwise `pipelines_in_frame`, `page_faults`, `upload` or `unknown`.

  A stall names the cause when it explains a quarter of the frame's time over the median.
- **Stutter**: frames more than 4 ms longer than the one before, per minute.
- **Pacing**: the share of frames within 2 ms of a whole number of target intervals, and the missed vsyncs.
- The worst 10 s windows, and the worst hitches with their log lines.
- `--baseline` exits 1 when the run is worse: a 1% or 0.1% low more than 5% lower, more frames over 50 ms, or more
  stutter (over 20% plus 1 a minute).

## The suite: `tools/perf/perf_suite.sh`

    tools/perf/perf_suite.sh --out DIR [--runs 2] [--scenarios "warm cold idle"] [--display offscreen|window]
                             [--ini FILE] [--baseline DIR] [--env VAR=VALUE ...]

Scenarios, each from `fixtures/save-course11` along the quick_course timeline (title, user, character, world map,
course 1-1 to the right):

- `warm`: caches as they are, 90 s.
- `cold`: no pipeline caches (put back afterwards) and, on Linux, Mesa's shader cache off; the input starts 30 s later
  for the startup preparation; 130 s.
- `idle`: the title screen, no input, 40 s.

Each run gets a report of its "play" window (from 5 s after the first frame) and of the whole run, then a summary (the
median over runs) and a baseline verdict. The player's save and `pc-settings.ini` are set aside and put back after
every run (checked byte for byte), and the suite refuses to start while the game runs. `--display window` shows the
game fullscreen on the desktop: the only way to see compositor and display timing, so ask first on a shared PC. On
Windows (MSYS2 bash) the default is `NX_HEADLESS_PRESENT=1`; take the GPU lock as for any game run.

## Rules

- Two runs per scenario at least; compare medians.
- Report the 1% and 0.1% lows and the hitch causes, never an average alone.
- A change that touches frame pacing, the present path, pipelines, uploads or loading runs the suite against the
  build before it, and must not be worse.
