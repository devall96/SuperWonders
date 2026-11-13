#!/bin/bash
# play_run.sh: one unattended run of the game with frame-keyed input, the probe and frame dumps (docs/PLAY-TOOLS.md).
# Windows (Git Bash) and Linux.
#
#   tools/play/play_run.sh --out DIR [--route NAME|FILE] [--replay FILE] [--record FILE] [--frames "items"]
#                          [--seconds N] [--dump N] [--exe PATH] [--data-root DIR] [--save DIR] [--cold]
#                          [--display offscreen|window] [--env VAR=VALUE ...]
#
#   --route course11 | worldmap | FILE   a route file (tools/play/routes/<name>.frames): the menus and the play, keyed
#                                        to game frames and anchors (NX_INPUT_FRAMES=@file)
#   --replay FILE / --record FILE        NX_INPUT_REPLAY / NX_INPUT_RECORD
#   --frames "items"                     an inline script (NX_INPUT_FRAMES)
#   --seconds N                          stop after N s (default: the route's "# seconds:" line, else 120); a run also
#                                        stops 8 s after the probe saw the goal or the end of the route
#   --dump N                             a frame every N game frames (NX_DUMP_FRAMES; default 120; 0 = none)
#   --save DIR                           the save the run starts from (default <data root>/fixtures/save-course11)
#   --cold                               no pipeline caches (moved aside and put back), Mesa's disk cache off
#
# Writes DIR: nx.log, probe.csv (one row per game frame), frames/*.png, report.md (tools/play/play_report.py). Exit
# 0 when the route's "# expect:" line holds (e.g. "goal"), else 1.
#
# Safety: refuses to start while a game runs; refuses the data root of the player's own game (wonder-play); the data
# root's user/save is replaced by the fixture save (the data root must be a test copy). Windows: the GPU is shared
# through C:\SuperWonders-private\gpu.lock ("play-tools ... run" in it while running).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=""; ROUTE=""; REPLAY=""; RECORD=""; FRAMES=""; SECS=""; DUMP=120; EXE=""; W=""; SAVE=""; COLD=0
DISPLAY_MODE=offscreen; EXTRA=()
while [ $# -gt 0 ]; do
    case $1 in
    --out) OUT=$2; shift ;; --route) ROUTE=$2; shift ;; --replay) REPLAY=$2; shift ;; --record) RECORD=$2; shift ;;
    --frames) FRAMES=$2; shift ;; --seconds) SECS=$2; shift ;; --dump) DUMP=$2; shift ;; --exe) EXE=$2; shift ;;
    --data-root) W=$2; shift ;; --save) SAVE=$2; shift ;; --cold) COLD=1 ;; --display) DISPLAY_MODE=$2; shift ;;
    --env) EXTRA+=("$2"); shift ;;
    *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
[ -n "$OUT" ] || { echo "--out DIR is required"; exit 2; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1 ;; *) WIN=0 ;; esac
W=${W:-$ROOT/private/wonder}
W=$(cd "$W" && pwd) || exit 2
case "$W" in *wonder-play|*wonder-play/) echo "refusing the player's own data root $W"; exit 2 ;; esac
EXE=${EXE:-$W/build/wonder$([ $WIN = 1 ] && echo .exe)}
SAVE=${SAVE:-$W/fixtures/save-course11}
PY=${PYTHON:-$([ $WIN = 1 ] && echo python || echo python3)}
[ -x "$EXE" ] || { echo "no game at $EXE"; exit 2; }
[ -d "$SAVE" ] || { echo "no save at $SAVE"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

ROUTE_FILE=""
if [ -n "$ROUTE" ]; then
    if [ -f "$ROUTE" ]; then ROUTE_FILE=$(cd "$(dirname "$ROUTE")" && pwd)/$(basename "$ROUTE")
    else ROUTE_FILE=$HERE/routes/$ROUTE.frames; fi
    [ -f "$ROUTE_FILE" ] || { echo "no route $ROUTE_FILE"; exit 2; }
    [ -z "$SECS" ] && SECS=$(sed -n 's/^# *seconds: *\([0-9]*\).*/\1/p' "$ROUTE_FILE" | head -1)
fi
SECS=${SECS:-120}
EXPECT=$([ -n "$ROUTE_FILE" ] && sed -n 's/^# *expect: *\([a-z,]*\).*/\1/p' "$ROUTE_FILE" | head -1)

running() { if [ $WIN = 1 ]; then tasklist //FI "IMAGENAME eq $(basename "$EXE")" 2>/dev/null | grep -qi "$(basename "$EXE")"; else pgrep -x "$(basename "$EXE")" >/dev/null; fi; }
LOCK=""
if [ $WIN = 1 ]; then
    LOCK=/c/SuperWonders-private/gpu.lock
    # Taken atomically: noclobber makes the create fail when another session made it first.
    for i in $(seq 720); do
        (set -C; echo "play-tools $(date +%Y-%m-%dT%H:%M:%S) run" > "$LOCK") 2>/dev/null && break
        [ $i = 720 ] && { echo "gpu.lock held: $(cat "$LOCK")"; exit 1; }; sleep 5
    done
fi
CACHES='pipelines_*.bin pipeline_keys_*.bin shader_prepare_*.done'
ASIDE=$OUT/.caches-aside
cleanup() {
    if [ -d "$ASIDE" ]; then (cd "$W" && for p in $CACHES; do [ -e "$p" ] && rm -f "$p"; done); mv "$ASIDE"/* "$W/" 2>/dev/null; rmdir "$ASIDE"; fi
    [ -n "$LOCK" ] && rm -f "$LOCK"
}
trap cleanup EXIT
running && { echo "a game is running: not starting"; exit 1; }

rm -rf "$W/user/save"; mkdir -p "$W/user"; cp -r "$SAVE" "$W/user/save"
rm -rf "$W/frames" "$OUT/frames"; mkdir -p "$OUT/frames"
mesa=""
if [ $COLD = 1 ]; then
    mkdir -p "$ASIDE"; (cd "$W" && for p in $CACHES; do [ -e "$p" ] && mv "$p" "$ASIDE/"; done)
    [ $WIN = 0 ] && mesa=MESA_SHADER_CACHE_DISABLE=true
fi
disp=(SDL_VIDEO_DRIVER=offscreen)
[ "$DISPLAY_MODE" = window ] && disp=()
[ $WIN = 1 ] && { [ "$DISPLAY_MODE" = window ] && disp=() || disp=(NX_HEADLESS=1 NX_HEADLESS_PRESENT=1); }
P() { [ $WIN = 1 ] && cygpath -w "$1" || echo "$1"; }
inp=()
[ -n "$ROUTE_FILE" ] && inp+=(NX_INPUT_FRAMES="@$(P "$ROUTE_FILE")")
[ -n "$FRAMES" ] && inp+=(NX_INPUT_FRAMES="$FRAMES")
[ -n "$REPLAY" ] && inp+=(NX_INPUT_REPLAY="$(P "$(cd "$(dirname "$REPLAY")" && pwd)/$(basename "$REPLAY")")")
[ -n "$RECORD" ] && inp+=(NX_INPUT_RECORD="$(P "$(mkdir -p "$(dirname "$RECORD")" && cd "$(dirname "$RECORD")" && pwd)/$(basename "$RECORD")")")
[ "$DUMP" != 0 ] && inp+=(NX_DUMP_FRAMES="$DUMP")

echo "== play run: $(basename "$EXE") ${ROUTE:+route $ROUTE }${REPLAY:+replay $REPLAY }$SECS s -> $OUT"
T0=$(date +%s)
(cd "$W" && env ${mesa:+$mesa} "${disp[@]}" NX_DATA_ROOT="$(P "$W")" NX_MUTE=1 NX_MEM_LIMIT_GB=10 NX_LOG="$(P "$OUT/nx.log")" \
    NX_PROBE="$(P "$OUT/probe.csv")" "${inp[@]}" "${EXTRA[@]}" "$EXE" >"$OUT/stdout.txt" 2>&1) &
GAME=$!
# Stop at the time limit, or 8 s after the route is done (the probe's goal, or the log's "route end" mark).
while kill -0 $GAME 2>/dev/null; do
    sleep 2
    now=$(($(date +%s) - T0))
    if [ $now -ge "$SECS" ]; then echo "   time limit ($SECS s)"; break; fi
    if [ -f "$OUT/nx.log" ] && grep -q "play: mark end\|play: event goal" "$OUT/nx.log"; then
        sleep 8; echo "   route done after $(($(date +%s) - T0)) s"; break
    fi
done
if kill -0 $GAME 2>/dev/null; then
    if [ $WIN = 1 ]; then
        for pid in $(tasklist //FI "IMAGENAME eq $(basename "$EXE")" //FO CSV //NH 2>/dev/null | cut -d, -f2 | tr -d '"'); do taskkill //F //PID "$pid" >/dev/null 2>&1; done
    else kill $GAME 2>/dev/null; sleep 1; pkill -x "$(basename "$EXE")" 2>/dev/null; fi
fi
wait $GAME 2>/dev/null
[ -d "$W/frames" ] && { $PY "$ROOT/tools/frames.py" png "$W/frames" "$OUT/frames" >/dev/null 2>&1 || cp "$W/frames"/* "$OUT/frames/"; rm -rf "$W/frames"; }
$PY "$HERE/play_report.py" "$OUT/probe.csv" --log "$OUT/nx.log" ${EXPECT:+--expect "$EXPECT"} > "$OUT/report.md"
CODE=$?
sed -n '1,25p' "$OUT/report.md"
exit $CODE
