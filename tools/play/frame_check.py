#!/usr/bin/env python3
"""Does a run render right? Compares its shots (NX_PLAY_SHOTS, runtime/src/play.cpp) with reference images.

    frame_check.py --log nx.log --frames DIR --reference DIR [--json out.json]     check; exit 1 when a shot fails
    frame_check.py --log nx.log --frames DIR --save-reference DIR                   make the reference from this run
                                                                                    (look at the shots first!)

The log names each shot's file ("play: shot <name> in frames/NNNNN.ppm"); DIR holds the run's NNNNN.ppm or .png.
Each shot is compared with <reference>/<name>.png at 192x108, at the best horizontal and vertical shift (replays drift a
few blocks, so the camera does too): it FAILS when, at that best shift, more than 12% of the pixels differ by more than
60 (of 255) or the mean difference is over 30. Every shot also FAILS on its own when more than 60% of it is near black
or more than 1% is magenta (the dummy texture). A missing shot fails; a shot without a reference is reported as
unchecked. The thresholds are NOT calibrated yet: offline, two good replays' frames of nearly the same moment differed
by 2.8% (mean 15.5) and the same frame with 25% of it in synthetic 32x32 blocks by 14.8% (mean 28.9). Calibrate them on
the first two good on-screen runs and on a run with the real corruption, and fix the numbers here.

Needs Pillow (private venv: pip install pillow).
"""
import argparse
import json
import os
import re
import shutil
import sys

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    print("frame check: FAILED: Pillow is missing (pip install pillow), nothing was checked")
    sys.exit(1)

W, H = 192, 108
MAX_DX, MAX_DY, STEP = 40, 10, 2
BAD_SHARE, BAD_MEAN, BAD_LEVEL = 0.12, 30.0, 60


def shots(log, frames):
    out = {}
    for m in re.finditer(r"play: shot (\S+) in frames/(\d+)\.ppm", open(log, errors="replace").read()):
        name, n = m.group(1), m.group(2)
        for ext in (".png", ".ppm"):
            p = os.path.join(frames, n + ext)
            if os.path.exists(p):
                out[name] = p
                break
        else:
            out.setdefault(name, None)
    return out


def small(path):
    return Image.open(path).convert("RGB").resize((W, H), Image.BILINEAR)


def self_check(img):
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    black = sum(1 for r, g, b in px if r < 16 and g < 16 and b < 16) / len(px)
    magenta = sum(1 for r, g, b in px if r > 200 and b > 200 and g < 60) / len(px)
    why = []
    if black > 0.60:
        why.append("%.0f%% near black" % (black * 100))
    if magenta > 0.01:
        why.append("%.1f%% magenta" % (magenta * 100))
    return why, black, magenta


def compare(a, b):
    """Best (share of pixels off by more than BAD_LEVEL, mean difference, dx, dy) over the shifts."""
    best = None
    for dy in range(-MAX_DY, MAX_DY + 1, STEP):
        for dx in range(-MAX_DX, MAX_DX + 1, STEP):
            ax, ay, bx, by = max(dx, 0), max(dy, 0), max(-dx, 0), max(-dy, 0)
            w, h = W - abs(dx), H - abs(dy)
            d = ImageChops.difference(a.crop((ax, ay, ax + w, ay + h)), b.crop((bx, by, bx + w, by + h))).convert("L")
            mean = ImageStat.Stat(d).mean[0]
            if best is not None and mean >= best[1]:
                continue
            hist = d.histogram()
            share = sum(hist[BAD_LEVEL + 1:]) / float(w * h)
            best = (share, mean, dx, dy)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--reference")
    ap.add_argument("--save-reference")
    ap.add_argument("--expect", default="title,map,start,mid,goal", help="shots that must be there")
    ap.add_argument("--json")
    a = ap.parse_args()
    got = shots(a.log, a.frames)
    if a.save_reference:
        os.makedirs(a.save_reference, exist_ok=True)
        for name, p in got.items():
            if p:
                Image.open(p).convert("RGB").save(os.path.join(a.save_reference, name + ".png"))
                print("reference %s <- %s" % (name, p))
        return 0
    rows, failed = [], []
    for name in [n for n in a.expect.split(",") if n] + [n for n in got if n not in a.expect.split(",")]:
        p = got.get(name)
        if not p:
            rows.append((name, "-", "FAILED: no shot"))
            failed.append(name)
            continue
        img = small(p)
        why, black, magenta = self_check(img)
        ref = os.path.join(a.reference, name + ".png") if a.reference else None
        detail = "black %.0f%%, magenta %.1f%%" % (black * 100, magenta * 100)
        if ref and os.path.exists(ref):
            share, mean, dx, dy = compare(img, small(ref))
            detail += "; vs reference: %.1f%% of pixels off, mean %.1f (shift %d,%d)" % (share * 100, mean, dx, dy)
            if share > BAD_SHARE or mean > BAD_MEAN:
                why.append("differs from the reference")
        else:
            detail += "; no reference: unchecked against one"
        verdict = ("FAILED: " + ", ".join(why)) if why else "ok"
        if why:
            failed.append(name)
        rows.append((name, os.path.basename(p), verdict + " (" + detail + ")"))
    print("| shot | frame | verdict |\n|---|---|---|")
    for r in rows:
        print("| %s | %s | %s |" % r)
    print("\nframe check: " + ("FAILED (" + ", ".join(failed) + ")" if failed else "ok"))
    if a.json:
        json.dump({"rows": rows, "failed": failed}, open(a.json, "w"), indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
