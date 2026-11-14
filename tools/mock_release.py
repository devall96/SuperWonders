#!/usr/bin/env python3
"""A stand-in for GitHub's releases on 127.0.0.1, to test SuperWonders Update without publishing anything.

    mock_release.py --dir <folder> [--port 8765] [--repo test/superwonders] [--bad-digest NAME] [--log FILE]

Every file in <folder> named SuperWonders-v<version>-<Platform>-x64.<zip|tar.gz|AppImage> becomes a release tagged
v<version> (a
version with -alpha/-beta/-rc a pre-release), with GitHub's fields the updater reads: tag_name, html_url, draft,
prerelease, and per asset name, size, browser_download_url and digest ("sha256:<hex>"). An <archive>.sig beside it is
listed too. Serves:
    GET /repos/<repo>/releases           the list (newest first)
    GET /download/<tag>/<name>           an asset
--bad-digest NAME: that asset's digest is wrong (the updater must refuse it). Every request is logged (--log), so a test
can tell the updater asked nothing (a launcher install).
Then: SUPERWONDERS_UPDATE_API=http://127.0.0.1:<port> SUPERWONDERS_UPDATE_REPO=<repo> wonder.exe --config
"""
import argparse
import hashlib
import http.server
import json
import os
import re
import sys
import time

NAME = re.compile(r"^SuperWonders-v(\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)\.\d+)?)-(Windows|Linux)-x64\.(zip|tar\.gz|AppImage)$")


def vkey(v):
    m = re.match(r"(\d+)\.(\d+)\.(\d+)(?:-(alpha|beta|rc)\.(\d+))?", v)
    kinds = {"alpha": 0, "beta": 1, "rc": 2, None: 3}
    return (int(m[1]), int(m[2]), int(m[3]), kinds[m[4]], int(m[5] or 0))


def releases(folder, port, bad):
    by_tag = {}
    for f in sorted(os.listdir(folder)):
        m = NAME.match(f)
        if not m:
            continue
        version = m[1]
        tag = "v" + version
        p = os.path.join(folder, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if f == bad:
            h = "0" * 64
        r = by_tag.setdefault(tag, {"tag_name": tag, "name": f"SuperWonders {tag}", "draft": False,
                                    "prerelease": "-" in version, "html_url": f"http://127.0.0.1:{port}/notes/{tag}",
                                    "assets": [], "_v": version})
        r["assets"].append({"name": f, "size": os.path.getsize(p), "digest": "sha256:" + h,
                            "browser_download_url": f"http://127.0.0.1:{port}/download/{tag}/{f}"})
        if os.path.isfile(p + ".sig"):
            r["assets"].append({"name": f + ".sig", "size": os.path.getsize(p + ".sig"), "digest": None,
                                "browser_download_url": f"http://127.0.0.1:{port}/download/{tag}/{f}.sig"})
    out = sorted(by_tag.values(), key=lambda r: vkey(r["_v"]), reverse=True)
    for r in out:
        del r["_v"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--repo", default="test/superwonders")
    ap.add_argument("--bad-digest", default="")
    ap.add_argument("--log", default="")
    a = ap.parse_args()
    folder = os.path.abspath(a.dir)

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            line = f"{time.strftime('%H:%M:%S')} {self.command} {self.path} -> {args[1] if len(args) > 1 else ''}"
            print(line, flush=True)
            if a.log:
                with open(a.log, "a") as f:
                    f.write(line + "\n")

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == f"/repos/{a.repo}/releases":
                body = json.dumps(releases(folder, a.port, a.bad_digest), indent=1).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            m = re.match(r"^/download/[^/]+/([^/]+)$", path)
            p = os.path.join(folder, m[1]) if m else ""
            if m and os.path.isfile(p) and os.path.dirname(os.path.abspath(p)) == folder:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(os.path.getsize(p)))
                self.end_headers()
                with open(p, "rb") as f:
                    while chunk := f.read(1 << 20):
                        self.wfile.write(chunk)
                return
            self.send_error(404)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), H)
    print(f"mock releases of {a.repo} on http://127.0.0.1:{a.port} from {folder}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
