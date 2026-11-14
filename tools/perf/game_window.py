#!/usr/bin/env python3
"""The play window of a course run in frame-trace seconds: from the player's first move (@go) to the goal.

    game_window.py <frametrace.csv> <nx.log>      prints "<from_s> <to_s> <go frame> <goal frame>"

The log gives the game frames ("play: anchor go from frame N", "play: event goal at frame M"; runtime/src/play.cpp);
the trace's guest_presents column, summed, turns a game frame into its row and time. Without a goal the window runs
to the end of the trace (to_s = -1); without a go it starts at the course start, else at 0. Standard library only.
"""
import csv
import re
import sys


def main():
    trace, log = sys.argv[1], open(sys.argv[2], errors="replace").read()
    go = re.search(r"play: anchor go from frame (\d+)", log) or re.search(r"play: anchor course from frame (\d+)", log)
    goal = re.search(r"play: event goal at frame (\d+)", log)
    gf, ef = int(go.group(1)) if go else 0, int(goal.group(1)) if goal else None
    total, t_go, t_goal = 0, None, None
    for r in csv.DictReader(open(trace, newline="")):
        try:
            total += int(r["guest_presents"])
            t = float(r["t_ms"]) / 1000
        except (KeyError, ValueError, TypeError):
            continue
        if t_go is None and total >= gf:
            t_go = t
        if ef is not None and t_goal is None and total >= ef:
            t_goal = t
    print("%.3f %.3f %d %d" % (t_go or 0.0, t_goal if t_goal is not None else -1, gf, ef if ef is not None else -1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
