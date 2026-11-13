#!/usr/bin/env python3
"""What a play run did, from its probe CSV (NX_PROBE, runtime/src/play.cpp; docs/PLAY-TOOLS.md).

    play_report.py <probe.csv> [--log nx.log] [--expect goal|course|map[,..]] [--json out.json]

Reports the segments the script reached, the game events (course entered, deaths, goal, world map) with their frames,
the player's track in each course (start, furthest x, distance travelled) and where the player was stuck (holding a
direction for 5 s without getting anywhere). Exit 1 when an --expect event did not happen.
Standard library only.
"""
import argparse
import csv
import json
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--log")
    ap.add_argument("--expect", default="")
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        # (the last row of a killed run can be cut short)
        rows = [r for r in csv.DictReader(open(a.csv, newline="")) if r.get("mark") is not None]
    except OSError as e:
        print("# Play run\n\nno probe CSV: %s" % e)
        return 1
    print("# Play run: %s\n" % a.csv)
    if not rows:
        print("no frames")
        return 1
    last = rows[-1]
    print("- game frames: %s, %.1f s; script segment reached: %s" % (last["frame"], float(last["t_ms"]) / 1000, last["seg"]))
    events = [(int(r["frame"]), float(r["t_ms"]) / 1000, e, r["course"]) for r in rows for e in r["event"].split("|") if e]
    print("- events: " + (", ".join("%s at frame %d (%.1f s%s)" % (e, f, t, ", " + c if c else "") for f, t, e, c in events) or "none"))
    marks = [(int(r["frame"]), m) for r in rows for m in r["mark"].split("|") if m]
    if marks:
        print("- marks: " + ", ".join("%s at %d" % (m, f) for f, m in marks))
    # The player's track, per stretch of frames with a valid player.
    tracks, cur = [], None
    for r in rows:
        if r["player"] not in ("", "0"):
            x, y = float(r["px"]), float(r["py"])
            if cur is None or cur["player"] != r["player"]:
                cur = {"player": r["player"], "course": r["course"], "from": int(r["frame"]), "x0": x, "y0": y, "xmax": x, "dist": 0.0, "px": x, "py": y, "rows": []}
                tracks.append(cur)
            cur["dist"] += abs(x - cur["px"]) + abs(y - cur["py"])
            cur["xmax"] = max(cur["xmax"], x)
            cur["px"], cur["py"], cur["to"] = x, y, int(r["frame"])
            cur["rows"].append((int(r["frame"]), x, int(r["buttons"], 16), int(r["lx"])))
        else:
            cur = None
    stuck = []
    for t in tracks:
        rs = t["rows"]
        k = 0
        while k < len(rs):
            f0, x0 = rs[k][0], rs[k][1]
            j = k
            while j + 1 < len(rs) and abs(rs[j + 1][1] - x0) < 1.0 and (rs[j + 1][3] != 0 or rs[j + 1][2] & 0x5000):
                j += 1
            if rs[j][0] - f0 >= 300:
                stuck.append((f0, rs[j][0], x0))
            k = j + 1
    if tracks:
        print("\n| from frame | to | course | start x | furthest x | end x | travelled |\n|---|---|---|---|---|---|---|")
        for t in tracks:
            print("| %d | %d | %s | %.1f | %.1f | %.1f | %.1f |" % (t["from"], t["to"], t["course"], t["x0"], t["xmax"], t["px"], t["dist"]))
    else:
        print("- player: never found (no probe layout, or the course was not reached)")
    if stuck:
        print("\nStuck (a direction held 5 s or more, x within 1 unit): " + ", ".join("frames %d-%d at x %.1f" % s for s in stuck))
    if a.log:
        try:
            log = open(a.log, errors="replace").read()
            for pat in ("fatal", "GPU hang", "VK_ERROR_DEVICE_LOST", "unimplemented"):
                n = len(re.findall(pat, log))
                if n:
                    print("- log: %d x '%s'" % (n, pat))
        except OSError:
            pass
    want = [w for w in a.expect.split(",") if w]
    got = {e for _, _, e, _ in events}
    missing = [w for w in want if w not in got]
    if want:
        print("\nExpected %s: %s" % ("+".join(want), "MISSING " + ", ".join(missing) if missing else "ok"))
    if a.json:
        json.dump({"frames": int(last["frame"]), "events": events, "marks": marks, "stuck": stuck,
                   "tracks": [{k: v for k, v in t.items() if k != "rows"} for t in tracks], "missing": missing}, open(a.json, "w"), indent=1)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
