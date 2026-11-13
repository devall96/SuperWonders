# Play tools: frame-keyed input, record and replay, a game-state probe

`runtime/src/play.cpp`, `tools/play/`. Design after Savage Wind (`SAVAGEWIND_INPUT="frame:button:frames"`, the control
socket's `mem scan` / `mem path` and named state cells: Revivedx/SavageWindRecomp `docs/CONTROL.md`) and the TotK recomp
(`TOTK_INPUT_SCRIPT` / `TOTK_INPUT_RECORD`: game ticks, a clock that restarts when the world has loaded, recordings that
replay as scripts: `docs/controls.md`).

## The clock: game frames and anchors

Input is keyed to the game's own frames (`g_frame`, the game's presents; the game reads the pad exactly once per frame,
loading screens included), not to wall time, so a slow PC or a shader compile does not move an input. A load still takes
a different number of frames from run to run, so a script is cut into segments by **anchors**; the frames of a segment
count from its anchor, and a segment starts only when its anchor happens (in order).

| anchor | when (from the files the game opens, and the probe) |
|---|---|
| `@title` | the title music starts (`Sound/Resource/Stream/BGM_Title`) |
| `@map` | a world map's music starts after `Stage/StageLoadInfo/World*` |
| `@course` | a course's music starts after `StageLoadInfo/Course*`, or after a death (the "Welcome" card goes then) |
| `@go` | after `@course`, the first frame the player moves (the probe): the game's own moment, exact to the frame |
| `@goal` | `BGM_Jingle_CourseClear` |
| `@death` | `BGM_Jingle_PlayerDown` |
| `@frame=N` | game frame N |

`@course+30` starts 30 frames after the event.

## Scripts: `NX_INPUT_FRAMES="<items>"` or `NX_INPUT_FRAMES=@<file>`

One item per line (or `;`-separated), `#` comments:

    <frame>:<BUTTONS>:<frames>[:<lx>,<ly>[,<rx>,<ry>]]     e.g. 120:B+RIGHT:20   or  40:NONE:30:0.5,0
    <frame>/<every>x<count>:<BUTTONS>:<frames>              e.g. 540/16x5:DLEFT:8 (five presses, 16 frames apart)

Buttons: `A B X Y L R ZL ZR PLUS MINUS LS RS`; `LEFT RIGHT UP DOWN` press the D-pad and push the left stick, `DLEFT
DRIGHT DUP DDOWN` the D-pad only; join with `+`. Sticks: integers are raw (-32767..32767), numbers with a point are
fractions. Added on top of the real pad. The old `NX_INPUT_SCRIPT` (wall-clock seconds) still works.
In a course, **B** jumps (A does not), Y dashes.

## Record and replay

- `NX_INPUT_RECORD=<path>` writes what the game got from the pad, per game frame, in the script format above: one item
  per run of frames with the same state, and an anchor line whenever an anchor event happens. `NX_INPUT_RECORD=1`
  still writes the old seconds format to `<data root>/input_record.txt` (Android `config.txt`).
- `NX_INPUT_REPLAY=<path>` plays a recording (or any script file) back. Start from the same save as the recording.
- Pick a fixed character, not Random: Random is a random pick, so a replay can get another character.

Determinism (Windows, RX 9070 XT, fixture save): menus and the `@title`/`@map`/`@course` anchors land on the same frames
(±3) run to run, and the same inputs from a standing start give the same position to 5 digits. But `@course` (a stream
opening) jitters by a frame or two against the game, and Wonder amplifies that: two replays of one 3400-frame course run
anchored on `@course` reached the goal 150-320 frames apart. Recordings therefore also carry `@go` (the player's first
movement), which the probe ties to the game's own state.

Measured since (2026-11-13, segments starting at the first pad read after the event, `@go` anchoring the course):
- The user's take 2 of 1-1 (Ubuntu), replayed twice on Windows: same pad input every frame; both reached the goal,
  2899 and 2901 frames after `@go` (the user's: 2899), at most 3.5 blocks apart mid-course and within 0.1 at the goal.
- The user's take 1 (a turn while dashing at go+317) left the recorded path at go+332 in every replay, and two
  Windows replays left it differently, so this is the game (thread or actor timing; suspected, not proven), not Linux
  vs Windows. Neither of those replays reached the goal. The Linux session saw take 1 with its 2.5-minute title wait
  trimmed match until go+398: time spent on the title may matter (a per-frame RNG?), so start recordings promptly.
  Not hunted further yet.
- `@go` fires when the player's x first changes, which in practice is the frame the course places the player (x leaves
  0.0): two replays saw x 20.71 and 20.94 there, about one frame of movement apart.

`NX_PLAY_FALLBACK=bot` (opt-in, not used by any script; only with the user's approval): when a replay dies, the bot
plays the rest (the log and the probe's `mark` say `fallback_bot`).

## Shots and the frame check: `NX_PLAY_SHOTS`

`NX_PLAY_SHOTS="<name>=<event>+<frames>,..."` (e.g. `start=go+30,goal=goal+90`) dumps the presented frame at those
moments (from the first time each event happened) to `<data root>/frames/NNNNN.ppm` and logs
`play: shot <name> in frames/NNNNN.ppm`; with `NX_DUMP_FSR=1` the dump is the image after FXAA and FSR, as shown in the
window. `tools/play/frame_check.py` compares the shots with reference images (see `docs/PERF-SUITE.md`).

## The probe: `NX_PROBE=<file.csv>`

One row per game frame: `frame, t_ms, seg, rel, pad_reads, buttons, lx, ly, scene, course, player, px, py, pz, vx, vy,
event, opens, open, mark`. `scene`/`course` come from `StageLoadInfo` (Title, World001, Course001_Course...).

The player (Wonder 1.0.0): the course player object has the vtable `main+0x96e400` (one live object) and its position
(x, y, z; blocks, y up) at `+0xc00`. The heap is laid out the same in every run, so it is first looked for where it was
last (course 1-1: `0x503a5c23d8`), else searched for by vtable on a background thread (about 1.5 s for the 2.5 GB heap,
only in a course or its loading). `NX_PROBE_PLAYER="vt=<hex>,pos=<hex>"` overrides the layout.

`tools/play/play_report.py <probe.csv>` reports the events, the player's track (start, furthest x, distance) and where
the player was stuck; `--expect goal` fails a run without the goal.

## Finding the player again (another game version): `NX_PROBE_SCAN=<out.txt>`

Script steps `<frame>:SNAP`, `<frame>:KEEP:inc|dec|same|changed` (32-bit floats that went up / down / stayed since the
last SNAP), `<frame>:DUMP` (candidates, the vtables before each and how many objects have each vtable).
`tools/play/routes/scan-player.frames` walks right, stands, walks left, stands, walks right; the player's class is the
one with a single object. `NX_PROBE_OBJ=<file>` dumps the bytes around the player object every frame, to find more
fields (a jump shows y at `+0xc04`).

## Runs: `tools/play/play_run.sh`

    tools/play/play_run.sh --out DIR [--route NAME|FILE] [--replay FILE] [--record FILE] [--seconds N] [--dump N]
                           [--exe PATH] [--data-root DIR] [--save DIR] [--cold] [--env VAR=VALUE ...]

Copies the fixture save into the data root (refuses the player's own `wonder-play`), takes `gpu.lock` on Windows, runs
in a visible window (`--display offscreen` for none), writes `nx.log`, `probe.csv`, `frames/*.png` and `report.md`, and stops 12 s after the goal or a `MARK:end`. Sound is on unless `--mute`.
`tools/play/routes/menus-course11.frames`: title to course 1-1 as Mario from the fixture save.
