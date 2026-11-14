#!/usr/bin/env python3
"""Rebuild an input recording (NX_INPUT_RECORD format) from a run's probe CSV (NX_PROBE) and log (NX_LOG).

    probe_to_frames.py <probe.csv> <nx.log> [-o out.frames]

The probe has the pad the game got on every frame (the row for frame N+1 holds the pad of game frame N) and the log
where each anchor's segment starts, which is what the recorder writes. For a run whose recording was lost or damaged.
The probe has no right stick, so a rebuilt recording has none (Wonder's courses do not use it). Standard library only.
"""
import argparse
import csv
import re
import sys

# The recorder's button order and names (runtime/src/play.cpp, kButtons and Recorder::flush).
NAMES = [("A", 1 << 0), ("B", 1 << 1), ("X", 1 << 2), ("Y", 1 << 3), ("L", 1 << 6), ("R", 1 << 7), ("ZL", 1 << 8),
         ("ZR", 1 << 9), ("PLUS", 1 << 10), ("MINUS", 1 << 11), ("LS", 1 << 4), ("RS", 1 << 5)]
DPAD = [("DLEFT", 1 << 12), ("DRIGHT", 1 << 14), ("DUP", 1 << 13), ("DDOWN", 1 << 15)]


def names(bits):
    n = [k for k, b in NAMES if bits & b] + [k for k, b in DPAD if bits & b]
    return "+".join(n) if n else "NONE"


def anchors(log):
    """(first frame of the segment, name) per anchor. New logs say it ("play: anchor <name> from frame <N>": the first
    pad read after frame() saw the event); in older ones a file event (title, map, course, goal, death) seen during
    frame f starts at f+1, and "go" (seen in frame() itself) at f."""
    ev = [(int(m.group(2)), m.group(1)) for m in re.finditer(r"play: anchor (\w+) from frame (\d+)", log)]
    if not ev:
        ev = [(int(m.group(2)) + (0 if m.group(1) == "go" else 1), m.group(1))
              for m in re.finditer(r"play: event (\w+) at frame (\d+)", log)]
    return sorted(ev)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("log")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    pad = {}
    for r in csv.DictReader(open(a.csv, newline="")):
        if r.get("mark") is None:  # (the last row of a killed run can be cut short)
            continue
        pad[int(r["frame"]) - 1] = (int(r["buttons"], 16), int(r["lx"]), int(r["ly"]))
    ev = anchors(open(a.log, errors="replace").read())
    out = open(a.out, "w", newline="\n") if a.out else sys.stdout
    out.write("# SuperWonders input recording: game frames (docs/PLAY-TOOLS.md)\n")
    st = {"seg": 0, "run": None, "from": None}

    def flush(to):
        if st["from"] is None or to <= st["from"]:
            return
        b, lx, ly = st["run"]
        if b or lx or ly:
            line = "%d:%s:%d" % (st["from"] - st["seg"], names(b), to - st["from"])
            if lx or ly:
                line += ":%d,%d" % (lx, ly)
            out.write(line + "\n")
        st["from"] = None

    ei = 0
    first, last = min(pad), max(pad)
    for f in range(first, last + 1):
        while ei < len(ev) and ev[ei][0] <= f:  # a segment from this frame on; a held state goes on in it
            held = st["from"] is not None
            flush(f)
            out.write("@%s\n" % ev[ei][1])
            st["seg"] = f
            if held:
                st["from"] = f
            ei += 1
        p = pad.get(f, (0, 0, 0))
        if st["from"] is None or p != st["run"]:
            flush(f)
            st["run"], st["from"] = p, f
    flush(last + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
