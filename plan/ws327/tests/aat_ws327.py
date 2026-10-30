#!/usr/bin/env python3
"""The QEMU checks of WS327's moved applications that the AAT's scenarios do not make (guest-apps.sh runs them
after the scenarios, on the same guest and session).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    aat_ws327.py --outdir OUTDIR [--only REGEX] -- --qemu

Items (each with a verdict in OUTDIR/verdicts.tsv and its steps in OUTDIR/records/):
  ws327.music.close-ends        Music plays a song given on its command line; its window's close button ends the
                                program (its player thread joined): no music process is left.
  ws327.videoplayer.close-ends  Video Player plays sample.mp4 (two pictures a second apart, the frames shown at a
                                pause after two seconds); its close button ends the program: no videoplayer is left.
  ws327.settings.storage-scan   Settings' Storage page: the Trash's size (its thread) and Analyze (the scan's
                                thread) to its end, "ZSETTINGS STORAGE done".
  ws327.ime.dictionary-at-exit  After desktop.input-method.japanese-textedit: the input method ended with SIGTERM
                                writes its learned choices (the writer thread joined at the end): the user
                                dictionary has a line with Japanese.
The places clicked come from the logs (the window's lines, Settings' CONTROL lines), never fixed pixels.
"""
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "plan/tools/aat/scenarios"))

import aatlib  # noqa: E402
import common  # noqa: E402

run = aatlib.Run.from_command_line("ws327")

# Settings' Storage page: its Analyze button (page-storage.c).
STORAGE_ANALYZE = 400


def running(name: str) -> list[str]:
	"""The processes whose program (argv[0]'s last part; zedBSD's ps shows argv[0] alone) is name."""
	_, output = run.sh(f"ps -A -o pid,args | awk '{{n = $2; sub(/.*\\//, \"\", n); if (n == \"{name}\") print $1}}'")
	return [line for line in output.split() if line.isdigit()]


def gone_after(name: str, seconds: float) -> list[str]:
	"""Waits up to seconds for every process of a program to end; returns those still running."""
	deadline = time.monotonic() + seconds
	left = running(name)
	while left and time.monotonic() < deadline:
		time.sleep(0.5)
		left = running(name)
	return left


@run.define("ws327.music.close-ends")
def music_close(item):
	folder = f"{aatlib.HOME}/Music/WS327"
	song = run.outdir / "ws327-tone.m4a"
	run.outdir.mkdir(parents=True, exist_ok=True)
	subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=550:duration=20", "-c:a",
		"aac", "-b:a", "96k", "-metadata", "title=WS327 Tone", str(song)], check=True, timeout=60)
	run.sh(f"mkdir -p {folder}")
	run.aat("put", str(song), f"{folder}/tone.m4a")
	run.sh(f"chown -R {aatlib.USER} {aatlib.HOME}/Music; chmod -R a+rX {folder}")
	try:
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/music {folder}/tone.m4a", ready=r"MUSIC READY ")
		ready = run.wait(r"MUSIC READY width=\d+ height=\d+", mark, 5)
		if ready and not window.sized():
			width = aatlib.number(ready, "width")
			height = aatlib.number(ready, "height")
			window = aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)
		played = run.wait(r"MUSIC PLAY song=\d+ error=0", mark, 10)
		time.sleep(2.5)
		positions = run.lines(r"MUSIC POSITION song=\d+ ms=\d+", mark)
		furthest = max((aatlib.number(line, "ms") or 0 for line in positions), default=0)
		item.step("/bin/music tone.m4a", f"{played}; furthest {furthest} ms")
		run.shot(item, "playing")
		item.check(played, "the song did not play")
		item.check(furthest >= 1000, f"the position did not move on ({furthest} ms)")
		run.close(item, window)
		left = gone_after("music", 10)
		item.step("after the close", f"music processes left: {left}")
		item.check(not left, f"Music did not end after its window closed: {left}")
		item.passed(f"played to {furthest} ms; no process left")
	finally:
		run.sh(f"rm -rf {folder}")


@run.define("ws327.videoplayer.close-ends")
def videoplayer_close(item):
	mark = run.mark()
	window = run.open_as_user(item, f"/bin/videoplayer {aatlib.SAMPLES}/sample.mp4", ready=r"VIDEOPLAYER READY ")
	opened = run.wait(r"VIDEOPLAYER OPEN path=\S*sample.mp4 width=\d+ height=\d+", mark, 15)
	item.step("opened sample.mp4", opened or "")
	item.check(opened, "sample.mp4 did not open")
	ready = run.wait(r"VIDEOPLAYER READY width=\d+ height=\d+", mark, 5)
	if ready and not window.sized():
		width = aatlib.number(ready, "width")
		height = aatlib.number(ready, "height")
		window = aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)
	time.sleep(0.5)
	run.shot(item, "playing-1")
	time.sleep(1.0)
	run.shot(item, "playing-2")
	time.sleep(0.5)
	pause = run.mark()
	run.key("space")
	paused = run.wait(r"VIDEOPLAYER PAUSE shown=\d+", pause, 5)
	shown = aatlib.number(paused, "shown")
	item.step("played about 2 s, then Space", paused or "no PAUSE")
	item.check(shown is not None and shown > 1, f"the frames did not advance: {paused}")
	run.key("space")
	run.close(item, window)
	left = gone_after("videoplayer", 10)
	item.step("after the close", f"videoplayer processes left: {left}")
	item.check(not left, f"Video Player did not end after its window closed: {left}")
	item.person(f"{shown} frames shown in about 2 s; the two pictures a second apart show different frames")


@run.define("ws327.settings.storage-scan")
def storage_scan(item):
	window, since = run.settings(item, "storage")
	trash = run.wait(r"ZSETTINGS STORAGE trash bytes=\d+ items=\d+", since, 20)
	item.step("the Storage page", trash or "no STORAGE trash line")
	controls = run.controls(since, "storage")
	mark = run.mark()
	run.click_control(item, window, controls, STORAGE_ANALYZE, "Analyze")
	started = run.wait(r"ZSETTINGS STORAGE analyze root=\S+ error=0", mark, 10)
	done = run.wait(r"ZSETTINGS STORAGE done root=\S+ bytes=\d+ files=\d+", mark, 120)
	item.step("Analyze", f"{started}; {done}")
	run.shot(item, "analyzed")
	item.check(trash, "the Trash's size was not counted")
	item.check(started and done, "the scan did not start and end")
	item.passed(f"{trash}; {done}")


@run.define("ws327.ime.dictionary-at-exit")
def ime_dictionary(item):
	path = f"{aatlib.HOME}/.config/zedbsd/ime/ja-user.dict"
	before = running("keiland-ime")
	item.step("the input method's processes", f"{before}")
	item.check(before, "no keiland-ime runs")
	run.sh(f"ls -l {path} 2>&1; for p in {' '.join(before)}; do kill -TERM $p; done")
	left = gone_after("keiland-ime", 15)
	_, listing = run.sh(f"ls -l {path} 2>&1")
	_, text = run.sh(f"cat {path} 2>&1")
	item.step("SIGTERM, then the user dictionary", f"left {left}; {listing.strip()}")
	japanese = [line for line in text.splitlines() if not line.startswith(";") and any(ord(c) > 0x7f for c in line)]
	item.step("its lines with Japanese", repr(japanese[:3]))
	item.check(not [pid for pid in left if pid in before], f"keiland-ime did not end: {left}")
	item.check(japanese, f"no learned Japanese in {path}: {text[:200]!r}")
	item.passed(f"{len(japanese)} learned lines, the first {japanese[0]!r}")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
