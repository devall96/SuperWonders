#!/bin/bash
# perf_suite.sh: the frame-time performance suite (docs/PERF-SUITE.md). Runs scripted scenarios with NX_FRAMETRACE,
# reports each run (tools/perf/frametrace_report.py) and writes a summary across runs, optionally gated on a baseline.
#
#   tools/perf/perf_suite.sh --out DIR [--runs 2] [--scenarios "warm cold"] [--display offscreen|window]
#                            [--ini FILE] [--baseline DIR] [--exe PATH] [--data-root DIR] [--route FILE]
#                            [--env VAR=VALUE ...]
#
# Scenarios that play course 1-1 for real (docs/PLAY-TOOLS.md): a recorded run replayed per game frame
# (NX_INPUT_REPLAY, default tools/play/routes/course11.frames, --route to change), from the fixture save, through the
# menus and the world map to the goal. If the replay dies (it can leave the recorded path: the game is not fully
# deterministic), the probe bot takes over (NX_PLAY_FALLBACK=bot) so the run still reaches the goal; the report says so.
# The "play" report covers the player's first move (@go) to the goal; the run stops 3 s after the goal.
#   course11       the caches as they are; at most 400 s
#   course11-cold  no pipeline caches (as cold below); at most 460 s
#
# The older scenarios (wall-clock input, course 1-1 played to the right; the player mostly stands near the start):
#   warm   the caches as they are; 90 s
#   cold   no pipeline caches (moved aside, put back after) and, on Linux, Mesa's disk cache off; startup shader
#          preparation runs first, so the input script starts 30 s later; 130 s
#   idle   the title screen, no input; 40 s (pacing with nothing loading)
# Each run's report covers the frames from 5 s after the first frame to the end ("play"), and the whole run ("all").
#
# Safety: refuses to run while a game is running; the player's save (user/save) and pc-settings.ini are moved aside
# and put back after every run (checked byte for byte); --ini gives the run its own settings (else the defaults).
# --display window shows the game on the desktop (real compositor timing); the default is offscreen (no window).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=""; RUNS=2; SCEN="course11 course11-cold"; DISPLAY_MODE=offscreen; INI=""; BASE=""; EXE=""; W=""; EXTRA=()
ROUTE=$ROOT/tools/play/routes/course11.frames
while [ $# -gt 0 ]; do
    case $1 in
    --out) OUT=$2; shift ;; --runs) RUNS=$2; shift ;; --scenarios) SCEN=$2; shift ;;
    --display) DISPLAY_MODE=$2; shift ;; --ini) INI=$2; shift ;; --baseline) BASE=$2; shift ;;
    --exe) EXE=$2; shift ;; --data-root) W=$2; shift ;; --env) EXTRA+=("$2"); shift ;; --route) ROUTE=$2; shift ;;
    *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
[ -n "$OUT" ] || { echo "--out DIR is required"; exit 2; }
W=${W:-$ROOT/private/wonder}
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1 ;; *) WIN=0 ;; esac
EXE=${EXE:-$W/build/wonder$([ $WIN = 1 ] && echo .exe)}
PY=${PYTHON:-python3}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
running() { if [ $WIN = 1 ]; then tasklist //FI "IMAGENAME eq wonder.exe" 2>/dev/null | grep -qi wonder.exe; else pgrep -x wonder >/dev/null; fi; }
# Windows: the GPU is shared with other sessions through C:\SuperWonders-private\gpu.lock (taken for the whole suite,
# "perf" in it, removed on exit). An existing lock means someone else is running: wait, then give up after an hour.
LOCK=""
if [ $WIN = 1 ]; then
    LOCK=/c/SuperWonders-private/gpu.lock
    # Taken atomically (noclobber: created only if absent), so two waiters never both see it free and both start.
    for i in $(seq 360); do
        (set -C; echo "perf-suite $(date +%Y-%m-%dT%H:%M:%S) perf" > "$LOCK") 2>/dev/null && break
        [ $i = 360 ] && { echo "gpu.lock held: $(cat "$LOCK")"; exit 1; }; sleep 10
    done
    trap 'rm -f "$LOCK"' EXIT
fi
running && { echo "a game is running: not starting"; exit 1; }
[ -d "$W/fixtures/save-course11" ] || { echo "no fixture save at $W/fixtures/save-course11"; exit 1; }

# The player's save and settings, aside for the suite and put back however it ends.
KEEP=$OUT/.player; mkdir -p "$KEEP"
[ -e "$KEEP/save" ] && { echo "$KEEP/save exists: an earlier suite did not finish; put it back first"; exit 1; }
cp -r "$W/user/save" "$KEEP/save"
[ -e "$W/pc-settings.ini" ] && mv "$W/pc-settings.ini" "$KEEP/pc-settings.ini"
CACHES='pipelines_*.bin pipeline_keys_*.bin shader_prepare_*.done'
restore() {
    [ -n "$LOCK" ] && rm -f "$LOCK"  # (trap restore EXIT replaces the lock trap above)
    rm -rf "$W/user/save"; cp -r "$KEEP/save" "$W/user/save"
    diff -r "$W/user/save" "$KEEP/save" >/dev/null && rm -rf "$KEEP/save" || echo "WARNING: save restore differs; kept in $KEEP/save"
    rm -f "$W/pc-settings.ini"; [ -e "$KEEP/pc-settings.ini" ] && mv "$KEEP/pc-settings.ini" "$W/pc-settings.ini"
    if [ -d "$KEEP/caches" ]; then (cd "$W" && for p in $CACHES; do [ -e "$p" ] && rm -f "$p"; done); mv "$KEEP/caches"/* "$W/" 2>/dev/null; rmdir "$KEEP/caches"; fi
}
trap restore EXIT

script() {  # the quick_course timeline, shifted by $1 s
    local o=$1 S="" t b d e
    for e in 6:L:0.4 6:R:0.4 10:A:0.25 12:A:0.25 14:A:0.25 16:A:0.25 20:RIGHT:0.6 23:A:0.25 26:A:0.25 29:A:0.25 32:A:0.25 40:RIGHT:200; do
        IFS=: read t b d <<<"$e"; S="$S;$(awk "BEGIN{print $t+$o}"):$b:$d"
    done
    for t in $(seq 41 1.5 240); do S="$S;$(awk "BEGIN{print $t+$o}"):A:0.3"; done
    echo "${S#;}"
}

run_one() {  # scenario run#
    local sc=$1 i=$2 tag=$1-$2 secs off input mesa=""
    case $sc in
    warm) secs=90; off=0 ;; cold) secs=130; off=30 ;; idle) secs=40; off=-1 ;;
    course11) secs=400; off=-1 ;; course11-cold) secs=460; off=-1 ;;
    *) echo "unknown scenario $sc"; return ;;
    esac
    input=""; [ $off -ge 0 ] && input=$(script $off)
    rm -rf "$W/user/save"; cp -r "$W/fixtures/save-course11" "$W/user/save"
    rm -f "$W/pc-settings.ini"; [ -n "$INI" ] && cp "$INI" "$W/pc-settings.ini"
    local EXTRA_RUN=() rp pp
    case $sc in course11*)
        [ -f "$ROUTE" ] || { echo "no route $ROUTE"; return; }
        if [ $WIN = 1 ]; then rp=$(cygpath -w "$ROUTE"); pp=$(cygpath -w "$OUT/$tag.probe.csv"); else rp=$ROUTE; pp=$OUT/$tag.probe.csv; fi
        EXTRA_RUN=(NX_INPUT_REPLAY="$rp" NX_PROBE="$pp" NX_PLAY_FALLBACK=bot) ;;
    esac
    if [ $sc = cold ] || [ $sc = course11-cold ]; then
        mkdir -p "$KEEP/caches"; (cd "$W" && for p in $CACHES; do [ -e "$p" ] && mv "$p" "$KEEP/caches/"; done)
        [ $WIN = 0 ] && mesa=MESA_SHADER_CACHE_DISABLE=true
    fi
    local disp=(SDL_VIDEO_DRIVER=offscreen)
    [ "$DISPLAY_MODE" = window ] && disp=(NX_FULLSCREEN=1)
    # Windows offscreen: a hidden window (NX_HEADLESS) that still presents (NX_HEADLESS_PRESENT), so the trace has
    # real presents and nothing shows on the desktop. The data root is given explicitly (the default is beside the exe).
    [ $WIN = 1 ] && [ "$DISPLAY_MODE" != window ] && disp=(NX_HEADLESS=1 NX_HEADLESS_PRESENT=1)
    [ $WIN = 1 ] && disp+=(NX_DATA_ROOT="$(cygpath -w "$W")")
    echo "== $tag ($secs s)"
    (cd "$ROOT" && env ${mesa:+$mesa} "${disp[@]}" NX_MUTE=1 NX_LOG="$OUT/$tag.log" NX_FRAMETRACE="$OUT/$tag.csv" \
        NX_INPUT_SCRIPT="$input" "${EXTRA_RUN[@]}" "${EXTRA[@]}" timeout $secs "$EXE" >"$OUT/$tag.out" 2>&1) &
    local game=$! t0=$(date +%s)
    # A course run ends 3 s after the goal.
    while kill -0 $game 2>/dev/null; do
        sleep 2
        if [ ${#EXTRA_RUN[@]} -gt 0 ] && grep -q "play: event goal" "$OUT/$tag.log" 2>/dev/null; then
            sleep 3
            if [ $WIN = 1 ]; then
                for pid in $(tasklist //FI "IMAGENAME eq $(basename "$EXE")" //FO CSV //NH 2>/dev/null | cut -d, -f2 | tr -d '"'); do taskkill //F //PID "$pid" >/dev/null 2>&1; done
            else pkill -x "$(basename "$EXE")"; fi
            break
        fi
    done
    wait $game 2>/dev/null
    if [ ${#EXTRA_RUN[@]} -gt 0 ]; then
        echo "   $(($(date +%s) - t0)) s$(grep -q 'play: event goal' "$OUT/$tag.log" && echo ', reached the goal' || echo ', NO GOAL')$(grep -q 'play: the replay died' "$OUT/$tag.log" && echo ' (the replay died; the bot finished)')"
    fi
    if [ $sc = cold ] || [ $sc = course11-cold ]; then
        (cd "$W" && for p in $CACHES; do [ -e "$p" ] && rm -f "$p"; done); mv "$KEEP/caches"/* "$W/" 2>/dev/null; rmdir "$KEEP/caches"
    fi
    # "play": from 5 s after the first frame (skips the boot and the startup preparation)
    local first=$($PY -c "import csv,sys;r=[x for x in csv.DictReader(open(sys.argv[1]))];print(float(r[1]['t_ms'])/1000 if len(r)>1 else 0)" "$OUT/$tag.csv" 2>/dev/null || echo 0)
    local from=$(awk "BEGIN{print $first+5}") to=""
    if [ ${#EXTRA_RUN[@]} -gt 0 ]; then  # the player's first move (@go) to the goal
        read from to _ _ < <($PY "$HERE/game_window.py" "$OUT/$tag.csv" "$OUT/$tag.log")
        [ "$to" = "-1.000" ] && to=""
        $PY "$ROOT/tools/play/play_report.py" "$OUT/$tag.probe.csv" --log "$OUT/$tag.log" --expect goal > "$OUT/$tag.route.md"
    fi
    $PY "$HERE/frametrace_report.py" "$OUT/$tag.csv" --log "$OUT/$tag.log" --from-s "$from" ${to:+--to-s "$to"} --label "$tag play" --json "$OUT/$tag.play.json" > "$OUT/$tag.play.md"
    $PY "$HERE/frametrace_report.py" "$OUT/$tag.csv" --log "$OUT/$tag.log" --label "$tag all" --json "$OUT/$tag.all.json" > "$OUT/$tag.all.md"
    sed -n 5p "$OUT/$tag.play.md"
}

for sc in $SCEN; do
    for i in $(seq 1 $RUNS); do run_one $sc $i; done
done

# Summary: median over runs per scenario; gate against --baseline (exit 1 when worse)
$PY - "$OUT" "$BASE" $SCEN <<'EOF'
import json, os, statistics, sys, glob
out, base, scen = sys.argv[1], sys.argv[2], sys.argv[3:]
keys = ["avg_fps", "low1_fps", "low01_fps", "p99_ms", "p999_ms", "max_ms", "over_33_ms", "over_50_ms", "over_100_ms", "stutters_per_min", "paced_pct", "hitches"]
def summary(d):
    res = {}
    for sc in scen:
        runs = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(d, sc + "-*.play.json")))]
        if runs:
            res[sc] = {k: statistics.median(r[k] for r in runs) for k in keys}
            res[sc]["runs"] = len(runs)
            causes = {}
            for r in runs:
                for c, v in r["hitch_causes"].items(): causes[c] = causes.get(c, 0) + v["count"]
            res[sc]["causes"] = causes
    return res
cur = summary(out)
json.dump(cur, open(os.path.join(out, "summary.json"), "w"), indent=1)
print("\n## Suite summary (median of runs, play window)\n")
print("| scenario | runs | avg fps | 1% low | 0.1% low | p99 ms | p99.9 ms | max ms | >33 | >50 | >100 | stutters/min | paced | hitch causes |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for sc, s in cur.items():
    print("| %s | %d | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %g | %g | %g | %.1f | %.1f%% | %s |" % (sc, s["runs"], s["avg_fps"], s["low1_fps"], s["low01_fps"], s["p99_ms"], s["p999_ms"], s["max_ms"], s["over_33_ms"], s["over_50_ms"], s["over_100_ms"], s["stutters_per_min"], s["paced_pct"], " ".join("%s:%d" % kv for kv in sorted(s["causes"].items(), key=lambda kv: -kv[1]))))
worse = []
if base:
    b = json.load(open(os.path.join(base, "summary.json")))
    for sc, s in cur.items():
        if sc not in b: continue
        o = b[sc]
        if s["low1_fps"] < o["low1_fps"] * 0.95: worse.append("%s 1%% low %.1f -> %.1f" % (sc, o["low1_fps"], s["low1_fps"]))
        if s["low01_fps"] < o["low01_fps"] * 0.95: worse.append("%s 0.1%% low %.1f -> %.1f" % (sc, o["low01_fps"], s["low01_fps"]))
        if s["over_50_ms"] > o["over_50_ms"]: worse.append("%s frames >50 ms %g -> %g" % (sc, o["over_50_ms"], s["over_50_ms"]))
        if s["stutters_per_min"] > o["stutters_per_min"] * 1.2 + 1: worse.append("%s stutters/min %.1f -> %.1f" % (sc, o["stutters_per_min"], s["stutters_per_min"]))
    print("\nAgainst the baseline %s: %s" % (base, "WORSE: " + "; ".join(worse) if worse else "not worse"))
sys.exit(1 if worse else 0)
EOF
