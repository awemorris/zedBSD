#!/usr/bin/env python3
"""Makes the files of the host test of Music's collection (ws177-p020): MP4 boxes with tags and no real sound.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    host-music-m.py FOLDER     (writes FOLDER/Music, and FOLDER/outside.m4a, spare.m4a and wide.png)

The albums: names that differ only in case and spaces (ASCII, Latin-1, Greek, Cyrillic, full-width), one album of
several artists without an album artist (Various Artists), one album artist named only by a later song, an album in
disc folders, two albums of the same names in two folders with the same numbers, a folder 10 down, a link back up,
an .mp4 with pictures and sound, and a cover that is not square.
"""
import io
import os
import struct
import sys
from pathlib import Path


def box(kind: bytes, payload: bytes) -> bytes:
	return struct.pack(">I", 8 + len(payload)) + kind + payload


def data(kind: int, value: bytes) -> bytes:
	return box(b"data", struct.pack(">I", kind) + bytes(4) + value)


def m4a(path: Path, items: dict, handlers=(b"soun",)) -> None:
	path.parent.mkdir(parents=True, exist_ok=True)
	movie = box(b"mvhd", bytes(4) + struct.pack(">IIII", 0, 0, 1000, 120000) + bytes(80))
	for handler in handlers:
		movie += box(b"trak", box(b"mdia", box(b"hdlr", bytes(8) + handler + bytes(12) + b"\0")))
	listed = b""
	for name, value in items.items():
		code = name.encode("latin-1")
		if name == "trkn":
			listed += box(code, data(0, struct.pack(">HHHH", 0, value, 10, 0)))
		elif name == "covr":
			listed += box(code, data(14, value))
		else:
			listed += box(code, data(1, value.encode("utf-8")))
	if listed:
		meta = bytes(4) + box(b"hdlr", bytes(8) + b"mdirappl" + bytes(9)) + box(b"ilst", listed)
		movie += box(b"udta", box(b"meta", meta))
	ftyp = box(b"ftyp", b"M4A " + bytes(4) + b"M4A mp42isom")
	path.write_bytes(ftyp + box(b"moov", movie) + box(b"mdat", bytes(64)))


def wide_cover() -> bytes:
	"""A PNG of 300 by 100: red, green and blue thirds (the middle square is green)."""
	from PIL import Image
	image = Image.new("RGB", (300, 100))
	for x in range(300):
		colour = (255, 0, 0) if x < 100 else (0, 255, 0) if x < 200 else (0, 0, 255)
		for y in range(100):
			image.putpixel((x, y), colour)
	out = io.BytesIO()
	image.save(out, "PNG")
	return out.getvalue()


def song(path: Path, title: str, artist: str = "", album: str = "", track: int = 0, album_artist: str = "",
		cover: bytes = b"", handlers=(b"soun",)) -> None:
	items = {"\xa9nam": title}
	if artist:
		items["\xa9ART"] = artist
	if album_artist:
		items["aART"] = album_artist
	if album:
		items["\xa9alb"] = album
	if track:
		items["trkn"] = track
	if cover:
		items["covr"] = cover
	m4a(path, items, handlers)


def main() -> None:
	folder = Path(sys.argv[1])
	music = folder / "Music"
	wide = wide_cover()
	(folder / "wide.png").write_bytes(wide)
	# One album, its names in other cases and spaces; the cover not square (on the second song only).
	song(music / "fold/1.m4a", "Fold One", "Ann", "Blue  Sky", 1)
	song(music / "fold/2.m4a", "Fold Two", "ANN", " blue sky ", 2, cover=wide)
	song(music / "greek/1.m4a", "Alpha", "ΑΝΝΑ", "ΑΛΦΑ", 1)
	song(music / "greek/2.m4a", "Beta", "αννα", "αλφα", 2)
	song(music / "cyrillic/1.m4a", "Pervaya", "Анна", "ДОМ", 1)
	song(music / "cyrillic/2.m4a", "Vtoraya", "анна", "дом", 2)
	song(music / "wide/1.m4a", "Wide One", "Ann", "ＡＢＣ", 1)
	song(music / "wide/2.m4a", "Wide Two", "Ann", "ａｂｃ", 2)
	# Several artists in one folder without an album artist: one album of Various Artists.
	song(music / "party/1.m4a", "Party One", "Xavier", "Party", 1)
	song(music / "party/2.m4a", "Party Two", "Yuki", "Party", 2)
	# The album artist named by a later song only.
	song(music / "later/1.m4a", "Later One", "Zed", "Later", 1)
	song(music / "later/2.m4a", "Later Two", "Guest", "Later", 2, album_artist="Zed")
	# Disc folders: one album, the numbers again on the second disc.
	song(music / "Opera/CD 1/1.m4a", "Act One", "Diva", "Opera", 1)
	song(music / "Opera/CD 2/1.m4a", "Act Two", "Diva", "Opera", 1)
	# The same names in two folders with the same numbers: two albums.
	song(music / "hits-a/1.m4a", "Hit A", "Quinn", "Greatest Hits", 1)
	song(music / "hits-b/1.m4a", "Hit B", "Quinn", "Greatest Hits", 1)
	# Ten folders down; an .mp4 with pictures and sound; a link back up.
	song(music / "d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/deep.m4a", "Deep Song", "Ann", "Deep", 1)
	song(music / "film.mp4", "Film Song", "Ann", "Film", 1, handlers=(b"vide", b"soun"))
	os.symlink("..", music / "fold/up")
	# The cache's test: a song whose title the test changes without a change of size.
	song(music / "cache/1.m4a", "Cache Song", "Ann", "Cache", 1)
	# Beside the folder: one opened from Files, and one the test copies in.
	song(folder / "outside.m4a", "Outside", "Ann", "Outside", 1)
	song(folder / "spare.m4a", "Spare Song", "Ann", "Spare", 1)


if __name__ == "__main__":
	main()
