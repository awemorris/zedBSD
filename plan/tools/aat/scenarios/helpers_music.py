#!/usr/bin/env python3
"""The automatic helper of tests/scenarios/apps/music/play.md (WS120 p009).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_music.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

On QEMU the guest needs a sound device for audiod (T1-301: MUSIC AUDIO error=13, ENODEV, without one): start it with
plan/tools/guest/guest.sh start --qemu-extra '-audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0'.

The songs are two m4a files the host's ffmpeg makes (8 s of 440 Hz and 660 Hz, AAC, with their tags; the first with a
PNG cover), put in kei's ~/Music/AAT and taken away after.  The places in Music's window are view.c's layout: the
albums' column 0.28 of the window's width within 220 and 300, the songs' card 8 to its right on glass, its Play
button 216 from the card's left and 135 from the top; the bar the bottom 80, its play button in the middle 28 down,
Next 52 right of it.
"""
import re
import subprocess
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_music")

FOLDER = "/home/kei/Music/AAT"
SONGS = (("01-tone-a.m4a", 440, "Tone A", 1), ("02-tone-b.m4a", 660, "Tone B", 2))


def sized(window, mark, regex):
	"""A window started outside App Home, its size taken from the program's own READY line (the compositor's lines
	give only its place)."""
	if window is None or window.sized():
		return window
	ready = run.wait(regex, mark, 5)
	if not ready:
		return window
	width, height = (int(value) for value in re.search(regex, ready).groups())
	return aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)


def make_songs(item) -> None:
	"""The two songs, made on this host and put in kei's Music folder."""
	folder = run.outdir / "music"
	folder.mkdir(parents=True, exist_ok=True)
	cover = folder / "cover.png"
	subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=orange:s=240x240", "-frames:v",
		"1", str(cover)], check=True, timeout=60)
	for name, frequency, title, number in SONGS:
		words = ["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", f"sine=frequency={frequency}:duration=8"]
		if number == 1:
			words += ["-i", str(cover), "-map", "0", "-map", "1", "-c:v", "png", "-disposition:v", "attached_pic"]
		words += ["-c:a", "aac", "-b:a", "96k", "-metadata", f"title={title}", "-metadata", "artist=AAT", "-metadata",
			"album=AAT Tones", "-metadata", f"track={number}", str(folder / name)]
		subprocess.run(words, check=True, timeout=60)
	run.sh(f"rm -rf {FOLDER}; mkdir -p {FOLDER}")
	for name, _, _, _ in SONGS:
		run.aat("put", str(folder / name), f"{FOLDER}/{name}")
	run.sh(f"chown -R kei /home/kei/Music; chmod -R a+rX {FOLDER}")
	item.step("two songs in ~/Music/AAT", "01-tone-a.m4a, 02-tone-b.m4a")


@run.define("apps.music.play")
def play(item):
	try:
		make_songs(item)
		mark = run.mark()
		window = run.launch(item, "Music")
		library = run.wait(r"MUSIC LIBRARY songs=\d+ error=\d+", mark, 10)
		audio = run.wait(r"MUSIC AUDIO error=\d+", mark, 5)
		codec = run.wait(r"MUSIC CODEC load error=\d+", mark, 5)
		glass = run.wait(r"MUSIC GLASS see_through=\d", mark, 5)
		cover = run.wait(r"MUSIC COVER album=0 error=-?\d+", mark, 5)
		item.step("opened", f"{library}; {audio}; {codec}; {glass}; {cover}")
		run.shot(item, "library")
		item.check(library and library.endswith("songs=2 error=0"), "the two songs are not in the list")
		item.check(audio and audio.endswith("error=0"), "no sound (audiod): on QEMU the guest needs a sound device, "
			"--qemu-extra '-audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0'")
		item.check(codec and "error=0" in codec, "libavcodec did not load")
		item.check(cover and cover.endswith("error=0"), "the album's cover was not read from its file and made (ws177-p020)")
		gap = 8 if glass and glass.endswith("=1") else 0
		share = min(max(int(window.width * 0.28), 220), 300)
		# Play, at the top of the songs: the first song.
		mark = run.mark()
		run.click(window.x + share + gap + 216, window.y + 135)
		played = run.wait(r"MUSIC PLAY song=0 error=0", mark, 10)
		opened = run.wait(r"MUSIC PLAY open codec=\S+", mark, 5)
		time.sleep(3.5)
		positions = run.lines(r"MUSIC POSITION song=0 ms=\d+", mark)
		furthest = max((int(re.search(r"ms=(\d+)", line).group(1)) for line in positions), default=0)
		item.step("Play", f"{played}; {opened}; furthest {furthest} ms")
		run.shot(item, "playing")
		item.check(played and opened and "codec=aac" in opened, "the first song did not play with the AAC decoder")
		item.check(furthest >= 2000, f"the position did not move on ({furthest} ms)")
		# Next, in the bar.
		middle = window.x + window.width // 2
		bar = window.y + window.height - 80 + 28
		mark = run.mark()
		run.click(middle + 52, bar)
		following = run.wait(r"MUSIC PLAY song=1 error=0", mark, 10)
		item.step("Next", following or "")
		item.check(following, "Next did not play the second song")
		# Space pauses, Space plays on.
		mark = run.mark()
		run.key("space")
		paused = run.wait(r"MUSIC PAUSE song=1", mark, 5)
		time.sleep(0.5)
		run.key("space")
		resumed = run.wait(r"MUSIC RESUME song=1", mark, 5)
		item.step("Space, Space", f"{paused}; {resumed}")
		item.check(paused and resumed, "Space did not pause and play on")
		# The last song's end: the player stops.
		ended = run.wait(r"MUSIC ENDED song=1", mark, 15)
		stopped = run.wait(r"MUSIC STOP song=1", mark, 3)
		item.step("waited for the end", f"{ended}; {stopped}")
		run.shot(item, "ended")
		item.check(ended and stopped, "the last song did not end and stop")
		# A folder added while Music is open: looked through again (ws177-p020); the same number in another folder is
		# another album.
		mark = run.mark()
		run.sh(f"mkdir -p {FOLDER}/copy && cp {FOLDER}/01-tone-a.m4a {FOLDER}/copy/ && chmod -R a+rX {FOLDER}/copy")
		rescan = run.wait(r"MUSIC RESCAN songs=\d+ albums=\d+ error=-?\d+", mark, 12)
		item.step("a copy in ~/Music/AAT/copy", rescan or "")
		item.check(rescan and rescan.endswith("songs=3 albums=2 error=0"), "the folder was not looked through again")
		run.close(item, window)
		# A file opened as Files opens it.
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/music {FOLDER}/02-tone-b.m4a", ready=r"MUSIC READY ")
		window = sized(window, mark, r"MUSIC READY width=(\d+) height=(\d+)")
		found = run.wait(r"MUSIC FILE song=\d+ error=\d+", mark, 10)
		started = run.wait(r"MUSIC PLAY song=\d+ error=\d+", mark, 10)
		item.step("/bin/music 02-tone-b.m4a", f"{found}; {started}")
		number = re.search(r"song=(\d+) error=0$", found or "")
		item.check(number, "the file was not found in the list")
		item.check(number and started and f"song={number.group(1)} error=0" in started, "the file did not play")
		run.close(item, window)
		item.person("the list with the album's cover, the bar with Tone A playing, the bar after the end")
	finally:
		run.sh(f"rm -rf {FOLDER}")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
