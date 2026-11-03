#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Runs the pages of plan/ws346/tests/pages and checks that the cache of style and layout changes nothing.

  run-pages.py [--program PATH] [--modes off,on,verify] [PAGE...]

Each page is loaded with `browser --run` under every mode (BROWSER_LAYOUT_CACHE); the page prints what getComputedStyle,
offsetWidth and offsetHeight say after each change it makes.  The output under every mode must be the same as under
off, and verify must report no difference on standard error.  Prints one line per page and a total; the exit status
is 0 when every page agrees.
"""

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
FONTS = ["--font=userland/desktop/fonts/Mahora-Regular.ttf", "--mono-font=userland/desktop/fonts/JetBrainsMono-Regular.ttf",
         "--fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"]


def run(program, page, mode):
    """Returns the page's output and standard error under a mode."""
    env = dict(os.environ, BROWSER_LAYOUT_CACHE=mode, BROWSER_LAYOUT_CACHE_STATS="1",
               XDG_DATA_HOME=os.path.join(ROOT, "build/ws074-host-data"))
    result = subprocess.run([program, "--run"] + FONTS + [page], capture_output=True, text=True, cwd=ROOT, env=env, timeout=300)
    return result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", default=os.path.join(ROOT, "build/ws074-host/plain/browser"))
    parser.add_argument("--modes", default="off,on,verify")
    parser.add_argument("pages", nargs="*")
    options = parser.parse_args()
    pages = options.pages or sorted(glob.glob(os.path.join(ROOT, "plan/ws346/tests/pages/restyle-*.html")))
    modes = options.modes.split(",")
    good = 0
    for page in pages:
        name = os.path.basename(page)
        outputs = {}
        problems = []
        stats = ""
        for mode in modes:
            out, err = run(options.program, page, mode)
            outputs[mode] = out
            if "layout mismatch" in err:
                problems.append("%s: verify reported a difference" % mode)
            for line in err.splitlines():
                if line.startswith("layout-cache:") and "mode=" + mode in line:
                    stats += "\n    " + line
        base = outputs[modes[0]]
        if not base.strip():
            problems.append("%s: no output" % modes[0])
        for mode in modes[1:]:
            if outputs[mode] != base:
                a = base.splitlines()
                b = outputs[mode].splitlines()
                at = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                problems.append("%s differs from %s at line %d:\n      %s: %s\n      %s: %s" % (
                    mode, modes[0], at + 1, modes[0], a[at] if at < len(a) else "(end)", mode, b[at] if at < len(b) else "(end)"))
        if problems:
            print("FAIL %s\n    %s" % (name, "\n    ".join(problems)))
        else:
            print("pass %s (%d lines)" % (name, len(base.splitlines())))
            good += 1
        if os.environ.get("PAGES_STATS"):
            print(stats)
    print("pages %d/%d" % (good, len(pages)))
    return 0 if good == len(pages) else 1


if __name__ == "__main__":
    sys.exit(main())
