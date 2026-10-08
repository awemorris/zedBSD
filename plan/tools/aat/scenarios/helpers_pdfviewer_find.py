#!/usr/bin/env python3
"""The automatic helper of apps.pdfviewer.find-select (tests/scenarios/apps/pdfviewer/find-select.md, ws128-p004).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_pdfviewer_find.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

The document is WS175's edit-basic.pdf (plan/ws175/tests/make-edit-samples.py, written on the host and put in
/tmp/aat-work/pdf-find.pdf).  The viewer's lines (PDFVIEWER ...) say what Find and the selection did; the place of
the first line's words is the viewer's layout of the scroll mode fitting the width (its margin of 16 pixels; the scale
fits the widest page, the sample's turned third page of 792 points, and a narrower page is centred: the first, US
Letter, is 612 points wide), from the window's place and the size the viewer's READY line gives.
"""
import re
import subprocess
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_pdfviewer_find")

SAMPLES = aatlib.ROOT / "plan/ws175/tests/make-edit-samples.py"
REMOTE = f"{aatlib.WORK}/pdf-find.pdf"

# The viewer's layout (pdfviewer/viewer.h PV_MARGIN) and the sample's page and first line (BASIC_* in the sample).
MARGIN = 16
PAGE_WIDTH = 612.0
WIDEST = 792.0
LINE_X = 72.0
LINE_Y = 792.0 - 700.0 - 5.0
K_X = 72.0 + 66.0


@run.define("apps.pdfviewer.find-select")
def find_select(item):
	# The document on the target.
	local = run.outdir / "pdfviewer-find"
	local.mkdir(parents=True, exist_ok=True)
	made = subprocess.run([sys.executable, str(SAMPLES), str(local)], capture_output=True, text=True, timeout=120)
	item.check(made.returncode == 0, f"make-edit-samples.py: {made.stderr.strip()[-200:]}")
	run.aat("put", str(local / "edit-basic.pdf"), REMOTE)
	run.sh(f"chmod 644 {REMOTE}")

	# Opened.  A window started outside App Home has only its place in the compositor's lines (KWL MAP), so its
	# size is the viewer's own READY line's (q826-i03: with 0 by 0 the drag below landed on the window's corner).
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/pdfviewer {REMOTE}")
	ready = run.wait(r"PDFVIEWER READY width=\d+ height=\d+ pages=3", mark, 10)
	item.step("opened edit-basic.pdf", ready or "")
	item.check(ready, "the PDF did not open with 3 pages")
	if ready and not window.sized():
		width, height = (int(value) for value in re.search(r"width=(\d+) height=(\d+)", ready).groups())
		window = aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)
	run.click(*window.middle())

	# "lazy" found on page 1.
	mark = run.mark()
	run.key("ctrl+f")
	time.sleep(0.6)
	run.type("lazy")
	found = run.wait(r'PDFVIEWER FIND found query="lazy" page=0 from=35 length=4', mark, 10)
	item.step("Ctrl+F, lazy", found or "")
	run.shot(item, "lazy")
	item.check(found, "lazy was not found on page 1 at 35")

	# "line": Enter twice reaches page 3.
	run.key("backspace", "backspace", "backspace", "backspace")
	run.type("line")
	time.sleep(0.4)
	mark = run.mark()
	run.key("enter")
	time.sleep(0.4)
	run.key("enter")
	third = run.wait(r'PDFVIEWER FIND found query="line" page=2', mark, 10)
	item.step("line, Enter twice", third or "")
	run.shot(item, "line-page-3")
	item.check(third, "Enter did not reach page 3's line")

	# After Enter the field keeps its words with the caret at their end: what is typed is added (ws177-p043).
	mark = run.mark()
	run.type("s")
	appended = run.wait(r'PDFVIEWER FIND \w+ query="lines"', mark, 10)
	item.step("typed s after Enter", appended or "")
	item.check(appended, "the s typed after Enter did not add to the words")
	run.key("backspace")
	time.sleep(0.4)

	# The first line's "The quick" selected and copied.
	run.key("esc")
	run.click(*window.middle())
	run.key("esc", "home")
	time.sleep(0.8)
	scale = (window.width - 2 * MARGIN) / WIDEST
	left = window.x + (window.width - PAGE_WIDTH * scale) / 2
	top = window.y + MARGIN
	mark = run.mark()
	run.drag(left + (LINE_X + 2.0) * scale, top + LINE_Y * scale, left + K_X * scale, top + LINE_Y * scale, 10)
	selected = run.wait(r"PDFVIEWER SELECT page=0 from=0 to=\d+", mark, 10)
	run.key("ctrl+c")
	copied = run.wait(r'PDFVIEWER COPY bytes=\d+ text="The qu', mark, 10)
	item.step("dragged over The quick, Ctrl+C", f"{selected}; {copied}")
	run.shot(item, "selected")
	item.check(selected and copied, "the first line's words were not selected and copied")
	item.person("the marks: lazy in orange, line on the turned page 3, The quick in blue")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
