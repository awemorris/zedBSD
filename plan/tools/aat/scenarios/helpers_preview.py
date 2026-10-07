#!/usr/bin/env python3
"""The automatic helper of tests/scenarios/apps/files/thumbnails.md (WS168 p004).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_preview.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

Files and Settings make their pictures with keiland-preview in a sandbox (sandbox_spawn): the AAT's samples (a PNG, a
JPEG, a PDF) and a damaged PNG are put in kei's ~/AATThumbs, Files opens the folder, and each thumbnail is made by a
child (its THUMB line names the child's pid and status), the damaged one refused; opened again, they come from the
cache.  Settings' Wallpaper page makes its tiles the same way.  The kernel's log has no "SANDBOX deny" line (the
child keeps to its set of calls).
"""
import re
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_preview")

FOLDER = "/home/kei/AATThumbs"
CACHE = "/home/kei/.cache/keiland/thumbnails"


def sized(window, mark):
	"""A window started outside App Home, its size taken from the compositor's import of its client's buffer.  Not
	from the program's READY line: the desktop is a Files too, and its READY (the whole screen below the bar) can come
	after the mark first (T1-319: the close button was looked for at 1334,78, off the screen)."""
	if window is None or window.sized():
		return window
	imported = run.wait(rf"KWL IMPORT client={window.client} buffer=\d+ width=\d+ height=\d+", mark, 5)
	if not imported:
		return window
	width, height = (int(value) for value in re.search(r"width=(\d+) height=(\d+)", imported).groups())
	return aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)


@run.define("apps.files.thumbnails")
def thumbnails(item):
	try:
		run.sh(f"rm -rf {FOLDER} {CACHE}; mkdir -p {FOLDER}; cp {aatlib.SAMPLES}/sample.png {aatlib.SAMPLES}/sample.jpg "
			f"{aatlib.SAMPLES}/sample.pdf {FOLDER}/; printf '\\211PNG\\r\\n\\032\\nbroken' > {FOLDER}/broken.png; "
			f"chown -R kei {FOLDER}; chmod -R a+rX {FOLDER}")
		_, denied_before = run.sh("dmesg 2>/dev/null | grep -c 'SANDBOX deny'")
		item.step("four files in ~/AATThumbs", "sample.png, sample.jpg, sample.pdf, broken.png")
		# Files on the folder: each thumbnail by a child.
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/files {FOLDER}", ready=r"ZFILES READY ")
		window = sized(window, mark)
		made = {}
		for name in ("sample.png", "sample.jpg", "sample.pdf", "broken.png"):
			made[name] = run.wait(rf"ZFILES THUMB path={re.escape(FOLDER)}/{re.escape(name)} error=\d+ .*", mark, 30)
		item.step("Files on the folder", "; ".join(str(line) for line in made.values()))
		run.shot(item, "files")
		for name in ("sample.png", "sample.jpg", "sample.pdf"):
			line = made[name] or ""
			item.check(" error=0 " in line and " status=0 " in line and not line.endswith(" pid=0"),
				f"{name}: no thumbnail from a child ({line})")
		item.check(made["broken.png"] and " error=0 " not in made["broken.png"], "the damaged PNG was not refused")
		run.close(item, window)
		# Again: from the cache.
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/files {FOLDER}", ready=r"ZFILES READY ")
		window = sized(window, mark)
		cached = run.wait(rf"ZFILES THUMB path={re.escape(FOLDER)}/sample.png error=0 .* cached=1", mark, 15)
		item.step("opened again", cached or "")
		item.check(cached, "the thumbnail was not read from the cache")
		run.close(item, window)
		# Settings' Wallpaper page: its tiles by children.
		window, since = run.settings(item, "wallpaper")
		time.sleep(3.0)
		tiles = run.lines(r"ZSETTINGS LOOK picture path=\S+ error=\d+ ms=\d+", since)
		good = [line for line in tiles if " error=0 " in line]
		item.step("Settings' Wallpaper page", f"{len(good)} of {len(tiles)} tiles made")
		run.shot(item, "wallpaper")
		item.check(tiles and good, "no wallpaper tile was made")
		run.close(item, window)
		# The children kept to their set of calls.
		_, denied_after = run.sh("dmesg 2>/dev/null | grep -c 'SANDBOX deny'")
		item.step("the kernel's log", f"SANDBOX deny lines {denied_before.strip()} before, {denied_after.strip()} after")
		item.check(denied_before.strip() == denied_after.strip(), "a child made a call its sandbox refuses")
		item.person("the folder's icons are the pictures (the PDF its first page), the damaged PNG its kind's icon; the wallpaper tiles")
	finally:
		run.sh(f"rm -rf {FOLDER}")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
