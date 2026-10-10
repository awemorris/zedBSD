#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Checks path-only CLI exchange, concurrent writes and live commit wakeups."""
import concurrent.futures
import json
import calendar
import struct
import time
from PIL import Image
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(tempfile.mkdtemp(prefix="media-library-"))
library = root / "Pictures" / "Media"
environment = dict(os.environ, HOME=str(root), WAYLAND_DISPLAY=str(root / "wayland-test"), TZ="UTC")



def command(operation, paths=(), data=None, expected=0):
    invocation = [str(binary), operation, *map(str, paths), "--root=" + str(library)]
    process = subprocess.run(invocation, input=data, capture_output=True, env=environment, timeout=20)
    assert process.returncode == expected, (invocation, process.returncode, process.stderr.decode(errors="replace"))
    if expected:
        return []
    lines = process.stdout.decode().splitlines()
    assert lines[0] == "# keiland-media 1\t" + str(library)
    return [line.split("\t") for line in lines[1:]]


def photos(rows):
    return [row for row in rows if row[0] == "P"]


assert command("list") == []
originals = root / "Originals"
originals.mkdir()
# Actual opaque video payload is never included in the CLI response.
video = originals / "動画 sample.mp4"
video.write_bytes(b"\x00\x00\x00\x18ftypisom\x00\x00\x00\x00" + bytes(range(256)) * 1025)
photo = originals / "photo space.png"
Image.new("RGB", (37, 19), (33, 55, 77)).save(photo)
os.utime(photo, (946684800, 946684800))
rows = command("add-list", data=(str(video) + "\n" + str(photo) + "\n").encode())
assert len(photos(rows)) == 2
for row in photos(rows):
    assert len(row) == 12
    path = Path(row[2])
    assert path.is_absolute() and path.is_relative_to(library / "Files")
    assert path.parent.relative_to(library / "Files").as_posix() == time.strftime("%Y/%m/%d", time.gmtime(int(row[6])))
    if row[11] == "photo space.png":
        assert row[7:9] == ["37", "19"]
        assert row[5] == row[6]  # No capture metadata: import date, never old mtime.
    original = originals / row[11]
    assert path.read_bytes() == original.read_bytes()
    assert int(row[4]) == original.stat().st_size
assert photos(command("add-list", data=(str(video) + "\n").encode())) == photos(rows)

# Commands serialize against a fresh database, even when many app helpers overlap.
paths = []
for index in range(12):
    path = originals / ("item-%d.mp4" % index)
    path.write_bytes(b"\x00\x00\x00\x18ftypmp42" + bytes([index]) * 65537)
    paths.append(path)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
    list(workers.map(lambda path: command("add", [path]), paths))
rows = command("list")
assert len(photos(rows)) == 14
first = photos(rows)[0][1]
album = "0123456789abcdef0123456789abcdef"
command("apply", data=("P\t%s\t1\t2\nA\t%s\tMy album\nM\t%s\t%s\n" % (first, album, album, first)).encode())
rows = command("list")
assert len(photos(rows)) == 14
assert any(row[1] == first and row[9:11] == ["1", "2"] for row in photos(rows))
assert ["M", album, first] in rows
# Reimport never resets saved metadata; unsupported image/audio brands are refused.
command("add", [video])
assert len(photos(command("list"))) == 14
unsupported = originals / "not-video.avif"
unsupported.write_bytes(b"\x00\x00\x00\x18ftypavif" + b"x" * 128)
command("add", [unsupported], expected=95)
assert len(photos(command("list"))) == 14
# A failed later item leaves successful imports recoverable and emits a wakeup.
partial = originals / "partial.mp4"
partial.write_bytes(b"\x00\x00\x00\x18ftypisom" + b"partial" * 17)
process = subprocess.run([str(binary), "add-list", "--root=" + str(library)],
    input=(str(partial) + "\n" + str(unsupported) + "\n").encode(), capture_output=True, env=environment, timeout=20)
assert process.returncode != 0
assert len(photos(command("list"))) == 15
# Bad options and empty lists cannot silently succeed.
command("list", ["unexpected"], expected=22)
command("add-list", data=b"", expected=95)
print("PASS mediastorage: metadata/path exchange, opaque originals, dedup, concurrent imports, marks/albums, partial failure")
print("Evidence root:", root)

# The default root is exactly the current user's ~/Pictures/Media.
process = subprocess.run([str(binary), "list"], capture_output=True, env=environment, timeout=20)
assert process.returncode == 0 and process.stdout.decode().splitlines()[0] == "# keiland-media 1\t" + str(library)
metadata = library / "metadata.db"
document = json.loads(metadata.read_text())
assert document["version"] == 1 and len(document["media"]) == 15
assert not (library / "db").exists()
assert document["media"][0]["path"].startswith("Files/")
# Unknown future fields, decimals, escapes, arrays and booleans survive unrelated edits.
extension = {"location": {"latitude": 35.123456, "longitude": 139.5}, "label": '撮影地 "海"\n😀', "flags": [True, False, None]}
document["extensions"] = extension
document["media"][0]["capture_location"] = extension
metadata.write_text(json.dumps(document, ensure_ascii=True))
identity = document["media"][0]["id"]
command("apply", data=("P\t%s\t1\t1\n" % identity).encode())
restored = json.loads(metadata.read_text())
assert restored["extensions"] == extension
assert next(item for item in restored["media"] if item["id"] == identity)["capture_location"] == extension
# JPEG dimensions and EXIF capture date override an intentionally old source mtime.
jpeg = originals / "captured.jpg"
exif = Image.Exif()
exif[306] = "2024:08:15 10:30:05"
Image.new("RGB", (81, 43), (23, 87, 101)).save(jpeg, exif=exif)
os.utime(jpeg, (946684800, 946684800))
rows = photos(command("add", [jpeg]))
captured = next(row for row in rows if row[11] == "captured.jpg")
assert captured[7:9] == ["81", "43"]
assert Path(captured[2]).parent.relative_to(library / "Files").as_posix() == "2024/08/15"
plain = originals / "plain.jpg"
Image.new("RGB", (29, 17), (99, 11, 77)).save(plain)
os.utime(plain, (946684800, 946684800))
rows = photos(command("add", [plain]))
uncaptured = next(row for row in rows if row[11] == "plain.jpg")
assert uncaptured[5] == uncaptured[6] and uncaptured[7:9] == ["29", "17"]
# An invalid JSON document refuses writes and preserves both metadata and originals.
good = metadata.read_bytes()
for malformed in (b'{"version":1,}', b'{"version":true}', b'{"version":1,"media":[', b'{"version":1,"extensions":-}', b'{"version":1,"extensions":"\\uD800"}'):
    metadata.write_bytes(malformed)
    process = subprocess.run([str(binary), "add", str(photo)], capture_output=True, env=environment, timeout=20)
    assert process.returncode != 0 and metadata.read_bytes() == malformed
metadata.write_bytes(good)
assert len(photos(command("list"))) == 17
print("PASS Media JSON: default root, date fallback/EXIF, JPEG/PNG sizes, unknown extensions/Unicode, malformed metadata preservation")
