#!/usr/bin/env python3
"""The automatic helpers of Notes' PDF editing (tests/scenarios/apps/notes/pdf-*.md, ws175-p010).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    helpers_notes_edit.py --outdir OUTDIR [--only REGEX] -- TARGET-OPTIONS     (--list: the ids)

Each scenario gets a folder of its own on the target, /tmp/aat-work/notes-ID-PID/, with the document
(edit-basic.pdf, which plan/ws175/tests/make-edit-samples.py writes on the host) as notes-edit.pdf and the
pictures (picture.png with alpha, landscape.jpg; PIL on the host) beside it, so that Notes' journal of
another run is never recovered and the file chooser finds the pictures by their first letter.  Notes is
started as the session's user with the document; its places come from its own lines: NOTES LAYOUT (the
page in the window), NOTES BUTTONS (the toolbar's buttons by action), NOTES TEXT box open (the text box),
and the window's place from the compositor's.  The saved document is taken to the host and read with
qpdf, pdftotext, pdffonts and pdfimages.  What only a person can judge (how the page looks) is left to
the screenshots (needs-person).
"""
import io
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

import aatlib
import common

run = aatlib.Run.from_command_line("helpers_notes_edit")

SAMPLES = aatlib.ROOT / "plan/ws175/tests/make-edit-samples.py"

# Notes' toolbar actions (userland/desktop/notes/app.h).
SELECT = 16
INSERT_IMAGE = 17
REPLACE_IMAGE = 18
DELETE_OBJECT = 19
RESET_OBJECT = 40
TEXT = 41
EDIT_TEXT = 42
FONT = 43
PEN = 1

# edit-basic.pdf (make-edit-samples.py's BASIC_*): US Letter, the paragraph's baselines from 700 down by 20 at x 72,
# 14 points; the image's box (x, y, width, height) in PDF points.
PAGE_HEIGHT = 792
LEFT = 72
TOP = 700
STEP = 20
IMAGE = (72, 420, 240, 150)


class Notes:
	"""Notes on a document of the scenario's own folder: its window and its lines since it started."""

	def __init__(self, item, folder: str, name: str = "notes-edit.pdf"):
		self.item = item
		self.folder = folder
		self.path = f"{folder}/{name}"
		self.start = run.mark()
		self.window = run.open_as_user(item, f"/bin/notes {shlex.quote(self.path)}", ready=r"NOTES FRAME first")
		self.opened = run.wait(r"NOTES OPEN pages=\d+", self.start, 10)
		item.step(f"Notes opened {self.path}", self.opened)
		item.check(self.opened, "Notes did not open the document")

	def layout(self) -> tuple[float, float, float]:
		"""The page's top left in the window and its pixels per point, from the last NOTES LAYOUT line."""
		lines = run.lines(r"NOTES LAYOUT window=", self.start)
		self.item.check(lines, "no NOTES LAYOUT line")
		match = re.search(r"page=(-?\d+),(-?\d+),\d+,\d+ scale=([0-9.]+)", lines[-1])
		return float(match.group(1)), float(match.group(2)), float(match.group(3))

	def at(self, x: float, y: float) -> tuple[int, int]:
		"""The screen's point of a point of the page shown (points, the top left the origin)."""
		left, top, scale = self.layout()
		return int(self.window.x + left + x * scale), int(self.window.y + top + y * scale)

	def at_pdf(self, x: float, y: float) -> tuple[int, int]:
		"""The screen's point of a point of an unrotated page's PDF space (points, y upward)."""
		return self.at(x, PAGE_HEIGHT - y)

	def scale(self) -> float:
		"""The page's pixels per point."""
		return self.layout()[2]

	def button(self, action: int) -> tuple[int, int]:
		"""The middle of a toolbar button, from the last NOTES BUTTONS line that has it."""
		for line in reversed(run.lines(r"NOTES BUTTONS", self.start)):
			match = re.search(rf" {action}:(-?\d+),(-?\d+),(\d+),(\d+)", line)
			if match:
				x, y, width, height = (int(value) for value in match.groups())
				return self.window.x + x + width // 2, self.window.y + y + height // 2
		self.item.failed(f"the toolbar has no button of action {action}")

	def press(self, action: int, wait: str | None = None, what: str = "") -> str | None:
		"""Presses a toolbar button and waits for a line after it."""
		mark = run.mark()
		run.click(*self.button(action))
		line = run.wait(wait, mark, 10) if wait else None
		time.sleep(0.5)
		self.item.step(f"pressed the toolbar's {what or action}", line or "")
		if wait:
			self.item.check(line, f"no line {wait!r} after the toolbar's {what or action}")
		return line

	def act(self, action, wait: str, what: str, timeout: float = 10) -> str:
		"""Does something (a function of no arguments) and waits for a line after it."""
		mark = run.mark()
		action()
		line = run.wait(wait, mark, timeout)
		time.sleep(0.4)
		self.item.step(what, line or "")
		self.item.check(line, f"{what}: no line {wait!r}")
		return line

	def close(self) -> None:
		"""Closes Notes with Ctrl+W (it saves first) and waits for it to end."""
		mark = run.mark()
		run.key("ctrl+w")
		ended = run.wait(r"NOTES EXIT ", mark, 20)
		self.item.step("Ctrl+W", ended or "")
		self.item.check(ended, "Notes did not end at Ctrl+W")
		time.sleep(0.5)


def prepare(item, pictures: bool = False) -> str:
	"""The scenario's folder on the target with the document (and the pictures); returns the folder."""
	local = run.outdir / "notes-edit" / item.ident
	local.mkdir(parents=True, exist_ok=True)
	made = subprocess.run([sys.executable, str(SAMPLES), str(local)], capture_output=True, text=True, timeout=120)
	item.check(made.returncode == 0, f"make-edit-samples.py: {made.stderr.strip()[-200:]}")
	files = {"notes-edit.pdf": local / "edit-basic.pdf"}
	if pictures:
		files.update(make_pictures(local))
	folder = f"{aatlib.WORK}/notes-{item.ident.split('.')[-1]}-{int(time.time())}"
	run.sh(f"mkdir -p {folder} && chmod 755 {folder}")
	for name, path in files.items():
		run.aat("put", str(path), f"{folder}/{name}")
	run.sh(f"chown -R {aatlib.USER} {folder} && chmod 644 {folder}/*")
	item.step(f"put {', '.join(files)} in {folder}")
	return folder


def make_pictures(local: Path) -> dict[str, Path]:
	"""picture.png (RGBA, its right half see-through by half) and landscape.jpg, written on the host."""
	from PIL import Image, ImageDraw

	png = Image.new("RGBA", (160, 100), (40, 120, 220, 255))
	draw = ImageDraw.Draw(png)
	draw.rectangle([80, 0, 159, 99], fill=(220, 60, 40, 128))
	draw.ellipse([20, 20, 60, 60], fill=(255, 255, 255, 255))
	png.save(local / "picture.png")
	jpeg = Image.new("RGB", (200, 120), (30, 140, 60))
	ImageDraw.Draw(jpeg).rectangle([60, 30, 140, 90], fill=(250, 220, 40))
	jpeg.save(local / "landscape.jpg", quality=90)
	return {"picture.png": local / "picture.png", "landscape.jpg": local / "landscape.jpg"}


def choose(item, notes: Notes, action: int, letter: str, mode: str) -> None:
	"""Presses Image or Replace and picks the file whose name starts with letter in the chooser."""
	notes.press(action, rf"NOTES CHOOSER open mode={mode}", "Image" if action == INSERT_IMAGE else "Replace")
	time.sleep(1.5)
	run.shot(item, f"chooser-{mode}")
	run.type(letter)
	time.sleep(0.4)
	run.key("enter")


def fetch(item, notes: Notes, name: str) -> Path:
	"""Takes the document from the target to the host."""
	local = run.outdir / "notes-edit" / item.ident / name
	run.aat("get", notes.path, str(local))
	item.step(f"took {notes.path} to the host", str(local))
	return local


def host(item, *command: str) -> str:
	"""Runs a command of the host on a file taken back; returns its output (with its errors)."""
	done = subprocess.run(list(command), capture_output=True, text=True, timeout=120)
	output = (done.stdout + done.stderr).strip()
	item.step(" ".join(Path(word).name if "/" in word else word for word in command), output[-300:])
	return output


def line_y(index: int) -> float:
	"""The PDF y of a point on the paragraph's line (0 the first), a little above its baseline."""
	return TOP - STEP * index + 4


@run.define("apps.notes.pdf-edit-image")
def pdf_edit_image(item):
	folder = prepare(item, pictures=True)
	notes = Notes(item, folder)
	notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")
	run.shot(item, "select-tool")

	# The JPEG chosen at its middle.
	left, bottom, width, height = IMAGE
	middle = notes.at_pdf(left + width / 2, bottom + height / 2)
	chosen = notes.act(lambda: run.click(*middle), r"NOTES EDIT select page=0 object=\d+ kind=image", "clicked the image's middle")
	run.shot(item, "selected")
	item.check("inserted=0" in chosen, "the page's image was taken for an inserted one")

	# Moved 100 pixels right and down.
	moved = notes.act(lambda: run.drag(middle[0], middle[1], middle[0] + 100, middle[1] + 100), r"NOTES EDIT move page=0 object=\d+",
		"dragged the image 100 px right and down")
	run.shot(item, "moved")

	# Sized by its bottom right handle, 80 pixels out (the handle where the move left it).
	dx = float(aatlib.field(moved, "dx") or 0)
	dy = float(aatlib.field(moved, "dy") or 0)
	handle = notes.at(left + width + dx, PAGE_HEIGHT - bottom + dy)
	sized = notes.act(lambda: run.drag(handle[0], handle[1], handle[0] + 80, handle[1] + 80), r"NOTES EDIT resize page=0 object=\d+",
		"dragged the bottom right handle 80 px out")
	run.shot(item, "resized")
	sx = float(aatlib.field(sized, "sx") or 0)
	sy = float(aatlib.field(sized, "sy") or 0)
	item.check(sx > 1.0 and abs(sx - sy) < 1e-3, f"the resize was not proportional and larger: {sized}")

	# Replaced by picture.png.
	mark = run.mark()
	choose(item, notes, REPLACE_IMAGE, "p", "replace")
	replaced = run.wait(r"NOTES EDIT replace page=0 object=\d+ image=160x100", mark, 15)
	item.step("chose picture.png", replaced or "")
	run.shot(item, "replaced")
	item.check(replaced, "no NOTES EDIT replace with picture.png")

	# Undo, redo, then the image chosen again and deleted.
	notes.act(lambda: run.key("ctrl+z"), r"NOTES EDIT undo page=0", "Ctrl+Z")
	run.shot(item, "undo")
	notes.act(lambda: run.key("ctrl+shift+z"), r"NOTES EDIT redo page=0", "Ctrl+Shift+Z")
	middle = notes.at(left + (width * sx) / 2 + dx, PAGE_HEIGHT - bottom - height + (height * sy) / 2 + dy)
	# The redo chose the image again (app_reselect), so a click on it logs no new select; the click only makes sure.
	run.click(*middle)
	time.sleep(0.4)
	item.step("clicked the image (chosen again by the redo)")
	notes.act(lambda: run.key("delete"), r"NOTES EDIT delete page=0 object=\d+", "Delete")
	run.shot(item, "deleted")

	# Saved, and read on the host.
	saved = notes.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request .*edits=\d+ edited_pages=1 ", "Ctrl+S")
	notes.close()
	path = fetch(item, notes, "saved.pdf")
	check = host(item, "qpdf", "--check", str(path))
	item.check("No syntax or stream encoding errors" in check, "qpdf --check found errors")
	original = (run.outdir / "notes-edit" / item.ident / "edit-basic.pdf").read_bytes()
	item.check(path.read_bytes()[:len(original)] == original, "the saved file does not start with the original's bytes")
	images = host(item, "pdfimages", "-list", "-f", "1", "-l", "1", str(path))
	item.step("page 1's images after the delete", images)
	item.check(len([row for row in images.splitlines()[2:] if row.strip()]) == 0, "page 1 still draws an image")
	item.person(f"{saved}; the screenshots: the frame and handles, the moved, sized, replaced image, the undo and the delete")


@run.define("apps.notes.pdf-insert-image")
def pdf_insert_image(item):
	folder = prepare(item, pictures=True)
	notes = Notes(item, folder)
	notes.act(lambda: run.key("pagedown"), r"NOTES PAGE current=1 count=3", "Page Down")
	notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")

	# picture.png inserted, then moved up and left.
	mark = run.mark()
	choose(item, notes, INSERT_IMAGE, "p", "insert")
	first = run.wait(r"NOTES EDIT insert page=1 kind=image object=\d+ image=160x100", mark, 15)
	item.step("chose picture.png", first or "")
	run.shot(item, "inserted-png")
	item.check(first, "no NOTES EDIT insert of picture.png")
	middle = notes.at(612 / 2, 792 / 2)
	notes.act(lambda: run.drag(middle[0], middle[1], middle[0] - 120, middle[1] - 120), r"NOTES EDIT move page=1 object=\d+",
		"dragged it up and left")

	# landscape.jpg inserted.
	mark = run.mark()
	choose(item, notes, INSERT_IMAGE, "l", "insert")
	second = run.wait(r"NOTES EDIT insert page=1 kind=image object=\d+ image=200x120", mark, 15)
	item.step("chose landscape.jpg", second or "")
	run.shot(item, "inserted-jpeg")
	item.check(second and aatlib.number(second, "object") != aatlib.number(first, "object"), "the JPEG was not a second object")

	# Saved, closed, opened again on page 2, the JPEG chosen.
	notes.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request .*edits=2 edited_pages=1 ", "Ctrl+S")
	notes.close()
	again = Notes(item, folder)
	edits = run.wait(r"NOTES EDITS opened edits=2 edited_pages=1 rebased=0", again.start, 10)
	item.step("opened again", f"{again.opened}; {edits}")
	item.check(edits and "kind=annotated" in again.opened, "the edits were not opened again")
	again.act(lambda: run.key("pagedown"), r"NOTES PAGE current=1 count=3", "Page Down")
	again.press(SELECT, None, "Select")
	again.act(lambda: run.click(*again.at(612 / 2, 792 / 2)), r"NOTES EDIT select page=1 object=\d+ kind=image inserted=1",
		"clicked the JPEG")
	run.shot(item, "reopened")
	again.close()

	# On the host: two images on page 2, the PNG's with an SMask.
	path = fetch(item, again, "saved.pdf")
	check = host(item, "qpdf", "--check", str(path))
	item.check("No syntax or stream encoding errors" in check, "qpdf --check found errors")
	images = host(item, "pdfimages", "-list", "-f", "2", "-l", "2", str(path))
	rows = [row.split() for row in images.splitlines()[2:] if row.strip()]
	item.check(len([row for row in rows if row[2] == "image"]) == 2, f"page 2 does not draw two images: {images}")
	item.check(any(row[2] == "smask" for row in rows), "the PNG has no SMask")
	item.person("the screenshots: the PNG in the middle, see-through on its right half, then moved; the JPEG; both after opening again")


@run.define("apps.notes.pdf-edit-text")
def pdf_edit_text(item):
	folder = prepare(item)
	notes = Notes(item, folder)
	notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")

	# The first line double-clicked: chosen, then its words in the box.
	point = notes.at_pdf(LEFT + 60, line_y(0))
	mark = run.mark()
	run.click(*point, "--count", "2")
	chosen = run.wait(r'NOTES EDIT select page=0 object=\d+ kind=text inserted=0 clipped=0 fixed=0 text="The quick brown fox', mark, 10)
	opened = run.wait(r"NOTES TEXT box open kind=line page=0 object=\d+ font=original", mark, 10)
	item.step("double-clicked the first line", f"{chosen}; {opened}")
	run.shot(item, "box")
	item.check(chosen and opened, "the first line's box did not open")
	first = aatlib.number(chosen, "object")

	# "dog" made "fox": shown on the page as typed, then kept in the line's own font.
	run.key("end", "backspace", "backspace", "backspace")
	run.type("fox")
	time.sleep(0.6)
	run.shot(item, "typed")
	kept = notes.act(lambda: run.key("enter"), rf"NOTES EDIT text page=0 object={first} kind=line chars=43 font=original fallback=0",
		"Enter")
	run.shot(item, "fox")

	# The second line given capitals its font lacks: a replacement font, and the status says so.
	point = notes.at_pdf(LEFT + 60, line_y(1))
	mark = run.mark()
	run.click(*point, "--count", "2")
	opened = run.wait(r"NOTES TEXT box open kind=line page=0", mark, 10)
	item.step("double-clicked the second line", opened or "")
	item.check(opened, "the second line's box did not open")
	run.key("end")
	mark = run.mark()
	run.type(" ZEBRA")
	# The box has all six characters (the 39 bytes of the line and " ZEBRA") before the screenshot.
	typed = run.wait(r"NOTES TEXT box reported=\d+ bytes=45\b", mark, 10)
	if not typed:
		typed = run.wait(r"NOTES TEXT box (input|reported)", mark, 1)
	item.step("typed ZEBRA", typed or "no NOTES TEXT box line (the keys did not reach the box)")
	run.shot(item, "zebra-typed")
	mark = run.mark()
	run.key("esc")
	pattern = r"NOTES EDIT text page=0 object=\d+ kind=line .*font=original fallback=1"
	replaced = run.wait(pattern, mark, 5)
	if not replaced:
		# An input method's composition takes the first Esc; the second closes the box.
		run.key("esc")
		replaced = run.wait(pattern, mark, 5)
	item.step("Esc", replaced or "")
	item.check(replaced, f"Esc: no line {pattern!r}")
	run.shot(item, "zebra")

	# The third line deleted.
	notes.act(lambda: run.click(*notes.at_pdf(LEFT + 60, line_y(2))), r"NOTES EDIT select page=0 object=\d+ kind=text", "clicked the third line")
	notes.act(lambda: run.key("delete"), r"NOTES EDIT delete page=0 object=\d+", "Delete")
	run.shot(item, "deleted")

	# The fourth line in Sans.
	notes.act(lambda: run.click(*notes.at_pdf(LEFT + 60, line_y(3))), r"NOTES EDIT select page=0 object=\d+ kind=text", "clicked the fourth line")
	time.sleep(0.5)
	notes.press(FONT, r"NOTES EDIT font page=0 object=\d+ font=sans fallback=1", "Font")
	run.shot(item, "sans")

	# Saved, and read on the host.
	notes.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request ", "Ctrl+S")
	notes.close()
	path = fetch(item, notes, "saved.pdf")
	check = host(item, "qpdf", "--check", str(path))
	item.check("No syntax or stream encoding errors" in check, "qpdf --check found errors")
	text = host(item, "pdftotext", "-f", "1", "-l", "1", str(path), "-")
	item.check("the lazy fox" in text and "ZEBRA" in text, "pdftotext does not read the new words")
	item.check("a third line that the test deletes" not in text, "the deleted line is still read")
	fonts = host(item, "pdffonts", str(path))
	item.check(re.search(r"\+\S+\s+CID TrueType\s+Identity-H\s+yes\s+yes\s+yes", fonts), "no embedded subset replacement font")
	item.person(f"{kept}; {replaced}; the screenshots: the box under the line, the words on the page as typed, the fonts")


@run.define("apps.notes.text-box-follow")
def text_box_follow(item):
	folder = prepare(item)
	notes = Notes(item, folder)
	notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")

	# The first line's box.
	point = notes.at_pdf(LEFT + 60, line_y(0))
	mark = run.mark()
	run.click(*point, "--count", "2")
	opened = run.wait(r"NOTES TEXT box open kind=line page=0 object=\d+ font=original", mark, 10)
	item.step("double-clicked the first line", opened or "")
	run.shot(item, "box")
	item.check(opened, "the first line's box did not open")
	first = aatlib.number(opened, "object")

	# Full screen: the box moves with the line (ws177-p012).
	mark = run.mark()
	run.key("f11")
	moved = run.wait(r"NOTES TEXT box moved rect=", mark, 10)
	closed = run.lines(r"NOTES TEXT box close", mark)
	item.step("F11", f"{moved}; {len(closed)} closes")
	time.sleep(0.8)
	run.shot(item, "fullscreen")
	item.check(moved and not closed, "the box did not move with the line, or it closed")

	# Typed; Ctrl+Z takes the last key back and Ctrl+Shift+Z does it again (ws177-p013: the box's own history).
	run.key("end")
	run.type(" again")
	time.sleep(0.4)
	mark = run.mark()
	run.key("ctrl+z")
	undone = run.wait(r"NOTES TEXT box reported=\d+ bytes=48\b", mark, 10)
	run.key("ctrl+shift+z")
	redone = run.wait(r"NOTES TEXT box reported=\d+ bytes=49\b", mark, 10)
	item.step("Ctrl+Z, Ctrl+Shift+Z in the box", f"{undone}; {redone}")
	item.check(undone and redone, "Ctrl+Z and Ctrl+Shift+Z did not take one key back and do it again")

	# Ctrl+S with the box open: the words kept and saved.
	mark = run.mark()
	run.key("ctrl+s")
	kept = run.wait(rf"NOTES EDIT text page=0 object={first} kind=line .*font=original", mark, 10)
	saved = run.wait(r"NOTES SAVE reason=request ", mark, 10)
	item.step("typed again, Ctrl+S", f"{kept}; {saved}")
	item.check(kept and saved, "Ctrl+S with the box open did not keep and save the words")

	# Back, closed, and read on the host.
	run.key("f11")
	time.sleep(0.8)
	notes.close()
	path = fetch(item, notes, "saved.pdf")
	text = host(item, "pdftotext", "-f", "1", "-l", "1", str(path), "-")
	item.check("again" in text, "pdftotext does not read the words typed before Ctrl+S")
	item.person("the screenshots: the box under the first line, before and after full screen")


@run.define("apps.notes.pdf-insert-text-font")
def pdf_insert_text_font(item):
	folder = prepare(item)
	old = common.current_method(run)
	common.set_method(run, item, 1)
	try:
		notes = Notes(item, folder)
		notes.press(TEXT, r"NOTES TOOL 41 name=text", "Text")
		run.shot(item, "text-tool")

		# "Hello Notes" at the page's lower margin, Sans.
		place = notes.at_pdf(LEFT, 200)
		notes.act(lambda: run.click(*place), r"NOTES TEXT box open kind=new page=0 object=-1 font=sans", "clicked the lower margin")
		run.type("Hello Notes")
		time.sleep(0.4)
		run.shot(item, "typing")
		inserted = notes.act(lambda: run.key("esc"), r"NOTES EDIT text page=0 object=\d+ kind=new chars=11 font=sans size=12 ",
			"Esc")
		run.shot(item, "inserted")
		index = aatlib.number(inserted, "object")

		# The text chosen with Select, and its font made Mono.
		notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")
		notes.act(lambda: run.click(place[0] + 20, place[1] + 6), rf"NOTES EDIT select page=0 object={index} kind=text inserted=1",
			"clicked the words")
		time.sleep(0.5)
		notes.press(FONT, rf"NOTES EDIT font page=0 object={index} font=mono$", "Font")
		run.shot(item, "mono")

		# Japanese through the input method, in the Japanese font.
		notes.press(TEXT, r"NOTES TOOL 41 name=text", "Text")
		notes.press(FONT, None, "Font (Mono)")
		notes.press(FONT, None, "Font (Japanese)")
		notes.act(lambda: run.click(*notes.at_pdf(LEFT, 140)), r"NOTES TEXT box open kind=new page=0 object=-1 font=cjk",
			"clicked another margin")
		common.to_language(run, item, "ja")
		run.type("nihongo")
		run.key("space")
		time.sleep(0.6)
		run.shot(item, "composing")
		run.key("enter")
		time.sleep(0.5)
		run.key("alt+space")
		time.sleep(0.4)
		japanese = notes.act(lambda: run.key("esc"), r"NOTES EDIT text page=0 object=\d+ kind=new chars=3 font=cjk ", "Esc")
		run.shot(item, "japanese")

		# Saved, and read on the host.
		notes.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request ", "Ctrl+S")
		notes.close()
	finally:
		if common.current_method(run) != old:
			common.set_method(run, item, old)
	path = fetch(item, notes, "saved.pdf")
	check = host(item, "qpdf", "--check", str(path))
	item.check("No syntax or stream encoding errors" in check, "qpdf --check found errors")
	text = host(item, "pdftotext", "-f", "1", "-l", "1", str(path), "-")
	item.check("Hello Notes" in text and "日本語" in text, "pdftotext does not read Hello Notes and 日本語")
	fonts = host(item, "pdffonts", str(path))
	item.check(len(re.findall(r"\+\S+\s+CID TrueType\s+Identity-H\s+yes\s+yes\s+yes", fonts)) >= 2, "not two embedded subsets")
	item.person(f"{japanese}; the screenshots: the box with the words, the preedit at the caret, Mono, the Japanese")


@run.define("apps.notes.pdf-edit-multipage")
def pdf_edit_multipage(item):
	folder = prepare(item)
	notes = Notes(item, folder)
	notes.press(SELECT, r"NOTES TOOL 16 name=select", "Select")

	# Page 1's image moved right.
	left, bottom, width, height = IMAGE
	middle = notes.at_pdf(left + width / 2, bottom + height / 2)
	notes.act(lambda: run.drag(middle[0], middle[1], middle[0] + 120, middle[1]), r"NOTES EDIT move page=0 object=\d+", "dragged the image right")

	# Page 3 (/Rotate 90): its first line deleted (shown at x = its baseline's y, y = its x).
	notes.act(lambda: run.key("pagedown", "pagedown"), r"NOTES PAGE current=2 count=3", "Page Down twice")
	time.sleep(0.8)
	point = notes.at(TOP + 4, LEFT + 60)
	notes.act(lambda: run.click(*point), r"NOTES EDIT select page=2 object=\d+ kind=text", "clicked page 3's first line")
	notes.act(lambda: run.key("delete"), r"NOTES EDIT delete page=2 object=\d+", "Delete")
	run.shot(item, "page-3-deleted")

	# A pen line on page 3.
	notes.press(PEN, r"NOTES TOOL 1 name=pen", "Pen")
	start = notes.at(300, 300)
	stroke = notes.act(lambda: run.press_path([(start[0] + step * 20, start[1] + step * 8) for step in range(8)]),
		r"NOTES STROKE page=2 ", "drew a line on page 3")
	run.shot(item, "page-3-stroke")

	# Saved, closed, opened again: the edits and the line kept.
	notes.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request pages=3 strokes=1 edits=2 edited_pages=2 ", "Ctrl+S")
	notes.close()
	again = Notes(item, folder)
	edits = run.wait(r"NOTES EDITS opened edits=2 edited_pages=2 rebased=0", again.start, 10)
	item.step("opened again", f"{again.opened}; {edits}")
	item.check(edits and "strokes=1" in again.opened, "the edits or the line were not opened again")
	run.shot(item, "reopened-page-1")

	# Page 1's image put back with Reset.
	again.press(SELECT, r"NOTES TOOL 16 name=select", "Select")
	moved = again.at_pdf(left + width / 2 + 120 / again.scale(), bottom + height / 2)
	again.act(lambda: run.click(*moved), r"NOTES EDIT select page=0 object=\d+ kind=image", "clicked the moved image")
	time.sleep(0.5)
	again.press(RESET_OBJECT, r"NOTES EDIT reset page=0 object=\d+", "Reset")
	run.shot(item, "reset")
	again.act(lambda: run.key("ctrl+s"), r"NOTES SAVE reason=request ", "Ctrl+S")
	again.close()

	# On the host: page 2 as the original's.
	path = fetch(item, again, "saved.pdf")
	check = host(item, "qpdf", "--check", str(path))
	item.check("No syntax or stream encoding errors" in check, "qpdf --check found errors")
	original = run.outdir / "notes-edit" / item.ident / "edit-basic.pdf"
	pages = []
	for source, name in ((original, "original"), (path, "saved")):
		prefix = run.outdir / "notes-edit" / item.ident / f"page2-{name}"
		host(item, "pdftoppm", "-r", "50", "-f", "2", "-l", "2", "-png", str(source), str(prefix))
		pages.append(sorted(prefix.parent.glob(f"page2-{name}*.png")))
	item.check(pages[0] and pages[1], "pdftoppm made no picture of page 2")
	from PIL import Image, ImageChops
	same = ImageChops.difference(Image.open(pages[0][0]).convert("RGB"), Image.open(pages[1][0]).convert("RGB")).getbbox() is None
	item.check(same, "page 2 is not drawn as the original's")
	item.person(f"{stroke}; the screenshots: page 3's line gone and the pen line, both after opening again, the image put back")


sys.exit(run.go(before=common.before(run), after=common.after(run)))
