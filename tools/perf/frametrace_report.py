#!/usr/bin/env python3
"""Frame-time report for an NX_FRAMETRACE csv (runtime/src/frametrace.h; docs/PERF-SUITE.md).

What players feel, not averages: 1% and 0.1% lows, frame-time percentiles, hitches (counted by size and named by
cause), stutter (frame-to-frame swings), pacing against the target, and the worst windows of the run. Each of the worst
hitches is listed with the log lines written during it (--log, the run's NX_LOG file).

  frametrace_report.py TRACE.csv [--log NX.log] [--from-s S] [--to-s S] [--target-fps 60] [--json OUT.json]
                       [--baseline BASE.json] [--top 12] [--label NAME]

--baseline compares with an earlier run's --json and exits 1 when this run is worse (lows down more than 5%, or more
hitches over 50 ms, or more stutter): the suite's regression gate.

Hitch causes after Savage Wind's tools/perf_routes.py (a stall that explains a quarter of the frame's excess over the
median, else what else happened in the frame), adapted to this runtime's two sides: the game's thread (guest: its
logic, its file reads, its waits) and the presenting thread (replay, pipelines, the GPU, the swapchain).
"""
import argparse
import csv
import json
import math
import statistics
import sys

HITCH_MS = (25.0, 33.4, 50.0, 100.0)
STUTTER_MS = 4.0  # a frame this much longer than the one before it: a visible swing


def load(path, t_from, t_to):
    with open(path, encoding="utf-8") as f:
        rows = []
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items()})
            except (TypeError, ValueError):  # a row cut short by a crash or a kill
                continue
    rows = rows[1:]  # (the first frame began before the trace)
    return [r for r in rows if (t_from is None or r["t_ms"] >= t_from * 1000) and (t_to is None or r["t_ms"] <= t_to * 1000)]


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def low_fps(ft_sorted_desc, share):
    """The 'x% low': the average fps over the slowest x% of frames (what CapFrameX and PresentMon tools show)."""
    n = max(1, int(len(ft_sorted_desc) * share))
    worst = ft_sorted_desc[:n]
    return 1000.0 / (sum(worst) / n)


def stats(rows, target_ms):
    ft = [r["frame_ms"] for r in rows if r["frame_ms"] > 0]
    if len(ft) < 10:
        return None
    s = sorted(ft)
    desc = s[::-1]
    dur = sum(ft) / 1000.0
    swings = [b - a for a, b in zip(ft, ft[1:])]
    # Pacing: each frame against the nearest whole number of target intervals (a 30 fps frame is 2 intervals).
    paced = sum(1 for x in ft if abs(x - max(1, round(x / target_ms)) * target_ms) <= 2.0)
    missed = sum(max(0, round(x / target_ms) - 1) for x in ft)
    return {
        "frames": len(ft), "seconds": round(dur, 2), "avg_fps": round(len(ft) / dur, 2),
        "low1_fps": round(low_fps(desc, 0.01), 2), "low01_fps": round(low_fps(desc, 0.001), 2),
        "p50_ms": round(pct(s, 50), 2), "p90_ms": round(pct(s, 90), 2), "p99_ms": round(pct(s, 99), 2),
        "p999_ms": round(pct(s, 99.9), 2), "max_ms": round(s[-1], 2),
        **{"over_%d_ms" % int(h): sum(1 for x in ft if x > h) for h in HITCH_MS},
        "stutters": sum(1 for d in swings if d > STUTTER_MS),
        "stutters_per_min": round(sum(1 for d in swings if d > STUTTER_MS) / max(dur / 60.0, 1e-9), 1),
        "paced_pct": round(100.0 * paced / len(ft), 1), "missed_vsyncs": int(missed),
    }


def classify(rows):
    """Hitches (frames over 2x the median and over 25 ms) with a cause each."""
    med = statistics.median(r["frame_ms"] for r in rows)
    keys = ("acquire_ms", "queue_present_ms", "replay_ms", "fence_ms", "present_cpu_ms", "guest_cpu_ms",
            "guest_replay_wait_ms", "guest_gap_ms", "minflt")
    medk = {k: statistics.median(r[k] for r in rows) for k in keys}
    out = []
    for i, r in enumerate(rows):
        if r["frame_ms"] <= max(2.0 * med, 25.0):
            continue
        excess = r["frame_ms"] - med
        x = lambda k: max(0.0, r[k] - medk[k])
        # Presenting side: stalls timed in this frame.
        stalls = [("swapchain_acquire", x("acquire_ms")), ("queue_present", x("queue_present_ms")),
                  ("gpu_wait", x("fence_ms")), ("pipeline_build", r["pipeline_ms"]), ("astc_decode", r["astc_ms"]),
                  ("replay_cpu", x("replay_ms"))]
        name, ms = max(stalls, key=lambda t: t[1])
        # Game side: the game's own frame came late (its gap over the median), split into what it did.
        guest_late = x("guest_gap_ms")
        guest = [("game_file_read", r["file_ms"]), ("game_logic_cpu", x("guest_cpu_ms")),
                 ("game_waits_replay", x("guest_replay_wait_ms"))]
        gname, gms = max(guest, key=lambda t: t[1])
        if r["guest_interval"] >= 2 and abs(r["frame_ms"] - r["guest_interval"] * 16.667) < 4:
            cause = "game_asked_30fps"  # the game's own present interval 2 (as on the Switch), not a stall
        elif ms >= 0.25 * excess and ms >= guest_late * 0.5:
            cause = name
        elif guest_late >= 0.25 * excess:
            cause = gname if gms >= 0.25 * guest_late else "game_late_other"
        elif r["file_ms"] >= 0.25 * excess:
            cause = "game_file_read"
        elif r["pipelines"] > 0 or r["skipped"] > 0:
            cause = "pipelines_in_frame"
        elif r["minflt"] > 4 * max(1.0, medk["minflt"]) and r["minflt"] > 500 or r["majflt"] > 0:
            cause = "page_faults"
        elif r["upload_mb"] >= 4:
            cause = "upload"
        else:
            cause = "unknown"
        out.append(dict(r, index=i, cause=cause, excess_ms=excess))
    return out, med


def log_lines(path):
    if not path:
        return None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return None


def windows(rows, target_ms, width_s=10.0):
    out, cur, start = [], [], None
    for r in rows:
        if start is None:
            start = r["t_ms"]
        if r["t_ms"] - start >= width_s * 1000 and cur:
            out.append((start / 1000.0, stats(cur, target_ms)))
            cur, start = [], r["t_ms"]
        cur.append(r)
    if len(cur) >= 10:
        out.append((start / 1000.0, stats(cur, target_ms)))
    return [(t, s) for t, s in out if s]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--log")
    ap.add_argument("--from-s", type=float)
    ap.add_argument("--to-s", type=float)
    ap.add_argument("--target-fps", type=float, default=60.0)
    ap.add_argument("--json")
    ap.add_argument("--baseline")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    target_ms = 1000.0 / a.target_fps
    rows = load(a.trace, a.from_s, a.to_s)
    st = stats(rows, target_ms)
    if not st:
        print("too few frames in %s" % a.trace)
        return 2
    hitches, med = classify(rows)
    causes = {}
    for h in hitches:
        causes.setdefault(h["cause"], [0, 0.0])
        causes[h["cause"]][0] += 1
        causes[h["cause"]][1] += h["excess_ms"]
    st["hitches"] = len(hitches)
    st["hitch_causes"] = {k: {"count": v[0], "excess_ms": round(v[1], 1)} for k, v in sorted(causes.items(), key=lambda kv: -kv[1][1])}
    st["label"] = a.label or a.trace
    tot = lambda k: sum(r[k] for r in rows)
    st["totals"] = {k: round(tot(k), 1) for k in ("pipelines", "pipeline_ms", "skipped", "astc", "astc_ms", "file_reads",
                                                   "file_mb", "file_ms", "majflt")}

    print("## %s\n" % st["label"])
    print("| frames | s | avg fps | 1% low | 0.1% low | p50 | p99 | p99.9 | max ms | >25 | >33 | >50 | >100 | stutters/min | paced | missed vsyncs |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    print("| {frames} | {seconds} | {avg_fps} | **{low1_fps}** | **{low01_fps}** | {p50_ms} | {p99_ms} | {p999_ms} | {max_ms} | "
          "{over_25_ms} | {over_33_ms} | {over_50_ms} | {over_100_ms} | {stutters_per_min} | {paced_pct}% | {missed_vsyncs} |".format(**st))
    print("\nHitches (frames over 2x the median %.2f ms and over 25 ms): %d" % (med, len(hitches)))
    for k, v in st["hitch_causes"].items():
        print("  %-20s %4d frames, %8.1f ms over the median" % (k, v["count"], v["excess_ms"]))
    print("\nIn the window: %(pipelines)d pipelines built (%(pipeline_ms).0f ms), %(skipped)d draws skipped while compiling, "
          "%(astc)d ASTC decodes (%(astc_ms).0f ms), %(file_reads)d file reads (%(file_mb).1f MB, %(file_ms).0f ms), "
          "%(majflt)d major faults" % st["totals"])

    # On screen: when each frame reached the display (<trace>.display.csv, windowed runs with present wait).
    disp = None
    try:
        with open(a.trace + ".display.csv", encoding="utf-8") as f:
            shown = sorted(float(r["displayed_ms"]) for r in csv.DictReader(f))
        lo, hi = (a.from_s or 0) * 1000, (a.to_s or 1e12) * 1000
        shown = [t for t in shown if lo <= t <= hi]
        drows = [{"frame_ms": b - x} for x, b in zip(shown, shown[1:])]
        disp = stats(drows, target_ms) if len(drows) >= 10 else None
    except (OSError, KeyError, ValueError):
        pass
    if disp:
        st["display"] = disp
        print("\nOn screen (present wait: when each frame reached the display):")
        print("| frames | avg fps | 1% low | 0.1% low | p99 | p99.9 | max ms | >25 | >33 | >50 | stutters/min | paced | missed vsyncs |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        print("| {frames} | {avg_fps} | **{low1_fps}** | **{low01_fps}** | {p99_ms} | {p999_ms} | {max_ms} | {over_25_ms} | "
              "{over_33_ms} | {over_50_ms} | {stutters_per_min} | {paced_pct}% | {missed_vsyncs} |".format(**disp))
    else:
        print("\nOn screen: no display times (offscreen run, or no present wait); the numbers above are presents.")

    ws = windows(rows, target_ms)
    if ws:
        print("\nWorst 10 s windows by 1% low:")
        print("| from s | avg fps | 1% low | max ms | >33 ms | stutters/min |")
        print("|---|---|---|---|---|---|")
        for t, s in sorted(ws, key=lambda w: w[1]["low1_fps"])[:5]:
            print("| %.0f | %.1f | %.1f | %.1f | %d | %.0f |" % (t, s["avg_fps"], s["low1_fps"], s["max_ms"], s["over_33_ms"], s["stutters_per_min"]))

    lines = log_lines(a.log)
    if hitches:
        print("\nWorst %d hitches:" % min(a.top, len(hitches)))
        prev_line = {int(r["frame"]): int(r["log_line"]) for r in rows}
        for h in sorted(hitches, key=lambda h: -h["frame_ms"])[:a.top]:
            print("- frame %d at %.2f s: %.1f ms, **%s** (acquire %.1f, present %.1f, fence %.1f, replay %.1f, pipelines %d/%.1f ms, "
                  "skipped %d, astc %.1f ms, files %d/%.1f ms, game gap %.1f cpu %.1f waits-replay %.1f, faults %d/%d)" % (
                      h["frame"], h["t_ms"] / 1000, h["frame_ms"], h["cause"], h["acquire_ms"], h["queue_present_ms"],
                      h["fence_ms"], h["replay_ms"], h["pipelines"], h["pipeline_ms"], h["skipped"], h["astc_ms"],
                      h["file_reads"], h["file_ms"], h["guest_gap_ms"], h["guest_cpu_ms"], h["guest_replay_wait_ms"],
                      h["minflt"], h["majflt"]))
            if lines:
                hi = int(h["log_line"])
                lo = prev_line.get(int(h["frame"]) - 1, hi)
                for ln in lines[max(lo, hi - 6):hi][:6]:
                    print("    log: " + ln[:150])

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=1)
    if a.baseline:
        with open(a.baseline, encoding="utf-8") as f:
            b = json.load(f)
        worse = []
        if st["low1_fps"] < b["low1_fps"] * 0.95:
            worse.append("1%% low %.1f -> %.1f" % (b["low1_fps"], st["low1_fps"]))
        if st["low01_fps"] < b["low01_fps"] * 0.95:
            worse.append("0.1%% low %.1f -> %.1f" % (b["low01_fps"], st["low01_fps"]))
        if st["over_50_ms"] > b["over_50_ms"]:
            worse.append("frames over 50 ms %d -> %d" % (b["over_50_ms"], st["over_50_ms"]))
        if st["stutters_per_min"] > b["stutters_per_min"] * 1.2 + 1:
            worse.append("stutters/min %.1f -> %.1f" % (b["stutters_per_min"], st["stutters_per_min"]))
        print("\nAgainst the baseline %s: %s" % (b.get("label", a.baseline), "WORSE: " + "; ".join(worse) if worse else "not worse"))
        return 1 if worse else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
