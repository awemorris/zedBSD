#!/usr/bin/env python3
"""The automatic helper of tests/scenarios/apps/photos/browse.md.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_photos.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

The photos are the ones plan/tools/aat/make-photos.py --view makes on this host (PIL): nine pictures, six JPEGs dated
by their EXIF over three months of 2026 (hill.jpg turned by its EXIF), a PNG, a GIF and one damaged JPEG.  They are
put in the session user's ~/AATPhotos and imported with /bin/photos --import, which asks the compositor's media
library (the mediastorage program) to copy them: the copies go to ~/Pictures/Media/Files/YYYY/MM/dd/<name>, their
records (id, path, sha256, taken, favorite, turns) and the albums (id, name, members) to ~/Pictures/Media/metadata.db
(JSON).  A file without an EXIF date is dated the day it is imported (its file time is not used), so the PNG, the
GIF and the damaged JPEG fall on the day of the run.  The same content is never copied twice (SHA-256).

Photos logs `PHOTOS MEDIA done imported=I error=E snapshot=S` when its worker's job ends (I is 1 for an import, 0 for
a reread); every change of the library (its own import or save too) wakes it to read the library again, which puts
the view back on the Timeline (ph_view_reset), so the helper waits for those rereads to stop before it clicks.

The photos' indices are the library's order (library.c library_compare_photos: the newest taken first, then by
path), which the helper computes from metadata.db.  The places in Photos' window are view.c's layout: the lists'
column 0.24 of the window's width within 200 and 260 (view_layout), the grid's card 8 to its right on glass; the
grid 72 down (VIEW_HEADER), its first month's heading 4 below that and 40 high (VIEW_MONTH), the cells 20 in from
the card's left (VIEW_MARGIN), as many columns of about 150 (VIEW_CELL) 6 apart (VIEW_CELL_GAP) as fit
(view_grid_columns), a new row under its heading at each month (view_grid_place); the lists' rows from 64 down
(VIEW_TITLE), Timeline and Favorites 44 each, the label Albums 36, the first album's row 48 high; the card of Add to
Album 360 wide in the middle, 160 high without albums, its name field 56 down and 16 in, 30 high.  The thumbnails
are cached as ~/.cache/keiland/photos/<id>.ppm (main.c ph_cache_folder).  The library, the cache and ~/AATPhotos
are taken away before and after.
"""
import json
import pathlib
import re
import subprocess
import sys
import time

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_photos")

SOURCE = f"{aatlib.HOME}/AATPhotos"
MEDIA = f"{aatlib.HOME}/Pictures/Media"
CACHE = f"{aatlib.HOME}/.cache/keiland/photos"
MAKER = pathlib.Path(__file__).resolve().parents[4] / "plan/tools/aat/make-photos.py"

# The files make-photos.py --view writes and the days their EXIF gives (None: no EXIF date, the day of the import).
DAYS = {
	"lake.jpg": "2026/09/20", "hill.jpg": "2026/09/12", "sea.jpg": "2026/08/30", "cake.jpg": "2026/08/03",
	"park.jpg": "2026/08/01", "garden.jpg": "2026/07/05", "sticker.png": None, "wave.gif": None, "broken.jpg": None,
}

# view.c's layout.
SIDEBAR_SHARE, SIDEBAR_MIN, SIDEBAR_MAX = 0.24, 200, 260
HEADER, MONTH, CELL, CELL_GAP, MARGIN, GLASS_GAP = 72, 40, 150, 6, 20, 8
FIRST_ALBUM_ROW = 64 + 44 + 44 + 36 + 48 // 2
CARD_WIDTH, CARD_HEIGHT, CARD_FIELD_Y, CARD_FIELD_X, CARD_FIELD_HEIGHT = 360, 160, 56, 16, 30

DONE = r"PHOTOS MEDIA done imported=\d+ error=-?\d+ snapshot=-?\d+"


def sized(window, mark):
	"""A window started outside App Home, its size taken from Photos' READY line (the compositor's lines give only
	its place)."""
	regex = r"PHOTOS READY width=(\d+) height=(\d+)"
	if window is None or window.sized():
		return window
	ready = run.wait(regex, mark, 5)
	if not ready:
		return window
	width, height = (int(value) for value in re.search(regex, ready).groups())
	return aatlib.Window(window.client, window.surface, window.x, window.y, width, height, window.docked)


def put_photos(item) -> None:
	"""The photos, made on this host and put in the session user's ~/AATPhotos; no library and no cache yet.  The
	file times of the files without an EXIF date are set to July, which the import must not take for their dates."""
	folder = run.outdir / "photos"
	subprocess.run([sys.executable, str(MAKER), "--view", str(folder)], check=True, timeout=120)
	run.sh(f"rm -rf {SOURCE} {MEDIA} {CACHE}; mkdir -p {SOURCE}/Family {SOURCE}/Trips")
	names = []
	for path in sorted(folder.rglob("*")):
		if path.is_file():
			name = path.relative_to(folder).as_posix()
			run.aat("put", str(path), f"{SOURCE}/{name}")
			names.append(name)
	run.sh(f"cd {SOURCE} && touch -t 202607141200 sticker.png && touch -t 202607101200 wave.gif && "
		f"touch -t 202607011200 broken.jpg; chown -R {aatlib.USER} {SOURCE}; chmod -R a+rX {SOURCE}")
	item.step(f"{len(names)} files in ~/AATPhotos", ", ".join(names))


def today() -> str:
	"""The target's local day as YYYY/MM/DD (the folder of a photo without an EXIF date imported now)."""
	_, text = run.sh("date +%Y/%m/%d")
	return text.strip()


def library() -> dict | None:
	"""metadata.db as JSON (None when it is not there or cannot be read)."""
	status, text = run.sh(f"cat {MEDIA}/metadata.db")
	start = text.find("{")
	if status != 0 or start < 0:
		return None
	try:
		return json.loads(text[start:])
	except ValueError:
		return None


def order(database: dict | None) -> list[dict]:
	"""The media records in Photos' order, which is their indices in its log lines: the newest taken first, then by
	path (the paths share the library's folder, so the relative paths order as the full ones)."""
	media = (database or {}).get("media", [])
	return sorted(media, key=lambda record: (-int(record.get("taken", 0)), record.get("path", "")))


def name_of(record: dict) -> str:
	"""A record's file name in the library (its path's last part, the name Photos logs)."""
	return record.get("path", "").rsplit("/", 1)[-1]


def index_of(records: list[dict], name: str) -> int:
	"""The index of the photo of a file name, or -1."""
	for index, record in enumerate(records):
		if name_of(record) == name:
			return index
	return -1


def month_of(record: dict) -> int:
	"""view.c's view_month: the year times 12 plus the month less 1, of the taken time read as UTC."""
	when = time.gmtime(int(record.get("taken", 0)))
	return when.tm_year * 12 + when.tm_mon - 1


def wait_library(condition, timeout: float = 10.0) -> dict | None:
	"""metadata.db once a condition holds of it (its last reading when it does not within the time)."""
	deadline = time.monotonic() + timeout
	database = library()
	while not condition(database) and time.monotonic() < deadline:
		time.sleep(1.0)
		database = library()
	return database


def settled(mark, timeout: float = 20.0) -> list[str]:
	"""Waits until Photos stops reading the library again (no new MEDIA done line for 3 s); returns the lines."""
	seen = run.lines(DONE, mark)
	deadline = time.monotonic() + timeout
	while time.monotonic() < deadline:
		time.sleep(3.0)
		now = run.lines(DONE, mark)
		if len(now) == len(seen):
			break
		seen = now
	return seen


def record_of(database: dict | None, name: str) -> dict:
	"""The media record of a file name in metadata.db ({} for none)."""
	for record in (database or {}).get("media", []):
		if name_of(record) == name:
			return record
	return {}


def cell(window, gap: int, records: list[dict], index: int) -> tuple[int, int, int]:
	"""The middle of a photo's cell in the Timeline (unscrolled) and its bottom in the window, as view_grid places
	it."""
	share = min(max(int(window.width * SIDEBAR_SHARE), SIDEBAR_MIN), SIDEBAR_MAX)
	inner = window.width - share - gap - 2 * MARGIN
	columns = max(1, (inner + CELL_GAP) // (CELL + CELL_GAP))
	side = max(16, (inner - (columns - 1) * CELL_GAP) // columns)
	at, column, last, place = 4, 0, None, None
	for position, record in enumerate(records):
		month = month_of(record)
		if month != last:
			if column != 0:
				at += side + CELL_GAP
			column, last = 0, month
			at += MONTH
		if position == index:
			place = (column, at)
		column += 1
		if column == columns:
			column, at = 0, at + side + CELL_GAP
	column, top = place
	x = window.x + share + gap + MARGIN + column * (side + CELL_GAP) + side // 2
	y = window.y + HEADER + top
	return x, y + side // 2, HEADER + top + side


@run.define("apps.photos.browse")
def browse(item):
	try:
		put_photos(item)
		# The import before the window: nine photos copied under Files/, their records in metadata.db.
		before = today()
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/photos --import={SOURCE}", ready=r"PHOTOS READY ", timeout=40)
		window = sized(window, mark)
		imported = run.wait(r"PHOTOS MEDIA done imported=1 error=-?\d+ snapshot=-?\d+", mark, 60)
		rereads = settled(mark)
		after = today()
		glass = run.wait(r"PHOTOS GLASS see_through=\d", mark, 5)
		database = library()
		records = order(database)
		names = [name_of(record) for record in records]
		paths = {name_of(record): record.get("path", "") for record in records}
		_, files = run.sh(f"cd {MEDIA} && find Files -type f | sort; find {SOURCE} -type f | grep -c .")
		item.step("imported and opened", f"{imported}; {len(rereads)} MEDIA done; {glass}; order {' '.join(names)}; "
			f"{' '.join(files.split())}")
		item.check(imported and imported.endswith("error=0 snapshot=0"), "the import did not end without an error")
		item.check(sorted(names) == sorted(DAYS), f"metadata.db does not hold the nine photos once each: {names}")
		wrong = [name for name, day in DAYS.items()
			if paths[name] not in (f"Files/{day or before}/{name}", f"Files/{day or after}/{name}")]
		item.check(not wrong, "copied to the wrong day's folder (EXIF date, else the day of the import): "
			+ ", ".join(f"{name}={paths[name]}" for name in wrong))
		item.check(len(files.split()) == 10 and files.split()[-1] == "9",
			"the Files folder does not hold nine copies, or the originals in ~/AATPhotos did not stay")
		dated = [name for name in names if DAYS[name]]
		item.check(dated == ["lake.jpg", "hill.jpg", "sea.jpg", "cake.jpg", "park.jpg", "garden.jpg"],
			f"the photos with an EXIF date are not newest first: {dated}")
		# The damaged file's thumbnail fails, and only its own.
		broken = index_of(records, "broken.jpg")
		failed = run.wait(rf"PHOTOS THUMB photo={broken} error=[1-9]\d*", mark, 30)
		others = [line for line in run.lines(r"PHOTOS THUMB photo=\d+ error=", mark) if f"photo={broken} " not in line]
		item.step(f"the damaged file is photo {broken}", f"{failed}; {'; '.join(others)}")
		run.shot(item, "timeline")
		item.check(failed, "the damaged file's thumbnail did not fail (or the thumbnails were not made)")
		item.check(not others, f"another photo's thumbnail failed: {others[:1]}")
		gap = GLASS_GAP if glass and glass.endswith("=1") else 0
		# A double click on hill.jpg (turned by its EXIF) shows it whole and upright.
		hill = index_of(records, "hill.jpg")
		x, y, bottom = cell(window, gap, records, hill)
		item.check(bottom <= window.height, f"hill.jpg's cell ends at {bottom}, below the window ({window.height})")
		mark = run.mark()
		run.click(x, y, "--count", "2")
		opened = run.wait(rf"PHOTOS OPEN photo={hill} name=hill\.jpg ", mark, 10)
		picture = run.wait(rf"PHOTOS PICTURE photo={hill} error=-?\d+ width=\d+ height=\d+", mark, 15)
		item.step(f"double-clicked {x},{y}", f"{opened}; {picture}")
		run.shot(item, "whole")
		item.check(opened, "the photo was not shown whole")
		item.check(picture and picture.endswith("error=0 width=640 height=480"), "its picture was not made upright (640 by 480)")
		# Right: the next photo (sea.jpg); R turns it, F marks it a favourite, each saved in its record.
		sea = hill + 1
		item.check(sea < len(names) and names[sea] == "sea.jpg", f"the photo after hill.jpg is not sea.jpg: {names}")
		mark = run.mark()
		run.key("right")
		following = run.wait(rf"PHOTOS OPEN photo={sea} name=sea\.jpg ", mark, 5)
		run.wait(rf"PHOTOS PICTURE photo={sea} error=0", mark, 15)
		run.key("r")
		turned = run.wait(rf"PHOTOS TURN photo={sea} turns=1", mark, 5)
		database = wait_library(lambda data: record_of(data, "sea.jpg").get("turns") == 1)
		# The save wakes Photos to read the library again; the photo shown whole is to stay shown.
		reread = run.wait(DONE, mark, 5)
		settled(mark, 10)
		item.step("Right, R", f"{following}; {turned}; {reread}; sea.jpg {record_of(database, 'sea.jpg')}")
		item.check(following and turned, "Right or R did not do its part")
		item.check(record_of(database, "sea.jpg").get("turns") == 1, "metadata.db does not hold the turn")
		mark = run.mark()
		run.key("f")
		marked = run.wait(rf"PHOTOS FAVORITE photo={sea} on=1", mark, 5)
		database = wait_library(lambda data: record_of(data, "sea.jpg").get("favorite") == 1)
		settled(mark, 10)
		saved = record_of(database, "sea.jpg")
		item.step("F", f"{marked}; sea.jpg {saved}")
		run.shot(item, "turned")
		item.check(marked, "F did not mark the photo shown whole (was it closed when the turn was saved?)")
		item.check(saved.get("favorite") == 1 and saved.get("turns") == 1, "metadata.db does not hold the favourite and the turn")
		# A: the card; a new album's name typed and Enter.
		mark = run.mark()
		run.key("a")
		card = run.wait(rf"PHOTOS CARD album photo={sea}", mark, 5)
		top = window.y + (window.height - CARD_HEIGHT) // 2
		run.click(window.x + (window.width - CARD_WIDTH) // 2 + CARD_FIELD_X + 100, top + CARD_FIELD_Y + CARD_FIELD_HEIGHT // 2)
		run.type("Sea")
		run.key("enter")
		created = run.wait(r"PHOTOS ALBUM create name=Sea error=0", mark, 5)
		added = run.wait(rf"PHOTOS ALBUM add album=Sea photo={sea} error=0", mark, 5)
		database = wait_library(lambda data: any(album.get("name") == "Sea" for album in (data or {}).get("albums", [])))
		settled(mark, 10)
		albums = (database or {}).get("albums", [])
		item.step("A, Sea, Enter", f"{card}; {created}; {added}; albums {albums}")
		item.check(card and created and added, "the album was not made with the photo")
		item.check(len(albums) == 1 and albums[0].get("name") == "Sea"
			and albums[0].get("members") == [record_of(database, "sea.jpg").get("id")],
			"metadata.db does not hold the album Sea with sea.jpg alone")
		# Escape: the grid, sea.jpg chosen.  The slideshow from it: Space, two photos on, Space.
		mark = run.mark()
		run.key("esc")
		back = run.wait(rf"PHOTOS BACK photo={sea}", mark, 5)
		run.key("space")
		started = run.wait(rf"PHOTOS SLIDESHOW on=1 photo={sea}", mark, 5)
		first = run.wait(rf"PHOTOS SLIDE photo={(sea + 1) % len(names)}\b", mark, 10)
		second = run.wait(rf"PHOTOS SLIDE photo={(sea + 2) % len(names)}\b", mark, 10)
		run.shot(item, "slideshow")
		run.key("space")
		stopped = run.wait(r"PHOTOS SLIDESHOW on=0", mark, 5)
		item.step("Escape, Space, waited, Space", f"{back}; {started}; {first}; {second}; {stopped}")
		item.check(back and started and first and second and stopped, "the slideshow did not start from sea.jpg, go on and stop")
		run.key("esc")
		run.close(item, window)
		_, kept = run.sh(f"sha256sum {MEDIA}/metadata.db")
		# Open again with the same import: nothing copied again, the album in the list, the thumbnails from the cache.
		mark = run.mark()
		window = run.open_as_user(item, f"/bin/photos --import={SOURCE}", ready=r"PHOTOS READY ", timeout=40)
		window = sized(window, mark)
		again = run.wait(r"PHOTOS MEDIA done imported=1 error=-?\d+ snapshot=-?\d+", mark, 60)
		settled(mark)
		_, now = run.sh(f"sha256sum {MEDIA}/metadata.db; find {MEDIA}/Files -type f | grep -c .")
		database = library()
		run.click(window.x + min(max(int(window.width * SIDEBAR_SHARE), SIDEBAR_MIN), SIDEBAR_MAX) // 2, window.y + FIRST_ALBUM_ROW)
		listed = run.wait(r"PHOTOS LIST list=2 album=0", mark, 5)
		_, cached = run.sh(f"ls {CACHE} | grep -c '\\.ppm$'")
		item.step("opened again, the album", f"{again}; {' '.join(now.split())}; {listed}; cache {cached.strip()}")
		run.shot(item, "album")
		item.check(again and again.endswith("error=0 snapshot=0"), "the second import did not end without an error")
		item.check(kept.split()[:1] == now.split()[:1] and now.split()[-1:] == ["9"] and len((database or {}).get("media", [])) == 9,
			"the same photos were imported again (metadata.db changed or Files grew)")
		item.check(listed, "the album was not read back into the list")
		item.check(cached.strip() == "8", "the thumbnails were not kept in the cache (eight, all but the damaged file's)")
		run.close(item, window)
		item.person("the Timeline by month (the day of the run first, with a grey square for the damaged file), hill.jpg "
			"upright, sea.jpg turned with Unfavorite, the slideshow, the album Sea with sea.jpg and its heart")
	finally:
		run.sh(f"rm -rf {SOURCE} {MEDIA} {CACHE}")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
