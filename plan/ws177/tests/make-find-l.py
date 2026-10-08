#!/usr/bin/env python3
"""make-find-l.py: the PDFs of the host test of PDF Viewer's find and selection (ws177-p040 to p043, case L).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    make-find-l.py FOLDER

The text is shown in a Type 3 font (/U) whose glyphs are boxes 500 units wide and whose /ToUnicode CMap tells each
code's character, so that any character can be written whatever font the host has; /N is the same font without the
CMap and with glyph names no list knows (its characters are U+FFFD); /P is a Type 3 font whose glyph "a" shows text
inside its procedure (the glyph's picture, not the page's text).

forms.pdf (ws177-p040), one page:
  the page's own lines "Page line" and the /P string "a"; the form /Fm1 shows "Form one", draws the nested /Fm2
  ("Nested two"), then shows "after nested" and, apart on the same baseline, the /P string "a"; /Fm3 is turned a
  quarter by its /Matrix and shows "Turned text".  The page text expected:
  Page line | a | Form one | Nested two | after nested a | Turned text

find.pdf (ws177-p041, p042), 32 pages of 612 x 792 points:
  1. the lines of FIND_PAGE below (a hyphen at a line's end, spaces, full and half width, kana with voiced marks,
     Greek, Cyrillic, ligatures, a sharp s, Japanese across a line's end)
  2. "Unreadable" in /N, then "page two has a lazy dog"
  3 to 32. "lazy dog on page N" (one "lazy" each)
"""
import sys
from pathlib import Path

FIND_PAGE = [
	"The inter-",
	"national meeting",
	"a well-",
	"known word",
	"Many    spaces   here",
	"ＦＵＬＬ ｗｉｄｔｈ ＡＢＣ",
	"ｶﾞｷﾞ half width",
	"か゛ and が",
	"ΑΘΗΝΑ ΣΟΦΟΣ",
	"МОСКВА",
	"ﬁnd the ﬂow",
	"Straße",
	"日本語の",
	"文章です",
	"The quick brown fox jumps over the lazy dog",
]


class Fonts:
	"""The characters of /U: each distinct character a one-byte code (1 to 255)."""

	def __init__(self) -> None:
		self.codes: dict[str, int] = {}

	def encode(self, text: str) -> bytes:
		"""The string's codes as a PDF hex string."""
		out = []
		for character in text:
			if character not in self.codes:
				self.codes[character] = len(self.codes) + 1
				if len(self.codes) > 255:
					raise SystemExit("too many characters for one font")
			out.append(self.codes[character])
		return b"<" + bytes(out).hex().encode() + b">"

	def tounicode(self) -> bytes:
		"""The /ToUnicode CMap of /U."""
		lines = [b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap",
			b"/CMapName /KeiFindL def 1 begincodespacerange <00> <FF> endcodespacerange"]
		items = sorted(self.codes.items(), key=lambda item: item[1])
		for start in range(0, len(items), 100):
			chunk = items[start:start + 100]
			lines.append(b"%d beginbfchar" % len(chunk))
			for character, code in chunk:
				lines.append(b"<%02X> <%s>" % (code, character.encode("utf-16-be").hex().upper().encode()))
			lines.append(b"endbfchar")
		lines.append(b"endcmap CMapName currentdict /CMap defineresource pop end end")
		return b"\n".join(lines)


def stream(dictionary: bytes, data: bytes) -> bytes:
	"""A stream object's body: its dictionary with its length, and its data."""
	return dictionary[:-2] + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def write(path: Path, objects: list[bytes]) -> None:
	"""Writes a PDF of objects 1..n (object 1 the catalog) with its cross-reference table."""
	out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
	offsets = []
	for number, body in enumerate(objects, start=1):
		offsets.append(len(out))
		out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
	table = len(out)
	out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
	for offset in offsets:
		out += b"%010d 00000 n \n" % offset
	out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, table)
	path.write_bytes(bytes(out))


class Document:
	"""Objects numbered as they are added; 1 is the catalog, 2 the pages."""

	def __init__(self) -> None:
		self.objects: list[bytes] = [b"", b""]

	def add(self, body: bytes) -> int:
		self.objects.append(body)
		return len(self.objects)

	def set(self, number: int, body: bytes) -> None:
		self.objects[number - 1] = body


def type3(document: Document, names: list[str], tounicode: int | None, procedures: dict[str, bytes] | None = None,
	  resources: bytes = b"<< >>") -> int:
	"""A Type 3 font of codes 1..len(names): boxes 500 wide (or the procedures given), named as given."""
	box = b"500 0 0 0 400 700 d1 0 0 400 700 re f"
	procs = []
	for name in names:
		body = box
		if procedures is not None and name in procedures:
			body = procedures[name]
		number = document.add(stream(b"<< >>", body))
		procs.append(b"/%s %d 0 R" % (name.encode(), number))
	differences = b"[1 " + b" ".join(b"/" + name.encode() for name in names) + b"]"
	widths = b"[" + b" ".join(b"500" for _ in names) + b"]"
	entry = b""
	if tounicode is not None:
		entry = b" /ToUnicode %d 0 R" % tounicode
	return document.add(b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 500 700] /FontMatrix [0.001 0 0 0.001 0 0]"
			    b" /CharProcs << " + b" ".join(procs) + b" >> /Encoding << /Type /Encoding /Differences "
			    + differences + b" >> /FirstChar 1 /LastChar %d /Widths " % len(names) + widths
			    + b" /Resources " + resources + entry + b" >>")


def finish(document: Document, pages: list[int], path: Path) -> None:
	"""The catalog and the page tree, then the file."""
	document.set(1, b"<< /Type /Catalog /Pages 2 0 R >>")
	kids = b" ".join(b"%d 0 R" % page for page in pages)
	document.set(2, b"<< /Type /Pages /Kids [" + kids + b"] /Count %d >>" % len(pages))
	write(path, document.objects)


def page(document: Document, content: bytes, resources: bytes) -> int:
	"""A page of 612 x 792 points with its content."""
	contents = document.add(stream(b"<< >>", content))
	return document.add(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents %d 0 R /Resources %s >>"
			    % (contents, resources))


def text(fonts: Fonts, font: bytes, size: int, x: float, y: float, words: str) -> bytes:
	"""A text object showing words at a place."""
	return b"BT /%s %d Tf %g %g Td %s Tj ET\n" % (font, size, x, y, fonts.encode(words))


def make_forms(folder: Path) -> None:
	"""forms.pdf: the text of the page, of forms, of a nested form, of a turned form, and of a Type 3 glyph."""
	fonts = Fonts()
	document = Document()
	page_content = text(fonts, b"U", 12, 72, 700, "Page line") + b"BT /P 12 Tf 72 680 Td <01> Tj ET\n"
	page_content += b"q /Fm1 Do Q /Fm3 Do\n"
	form1 = text(fonts, b"U", 12, 72, 600, "Form one") + b"/Fm2 Do\n" + text(fonts, b"U", 12, 72, 560, "after nested")
	form1 += b"BT /P 12 Tf 300 560 Td <01> Tj ET\n"
	form2 = text(fonts, b"U", 12, 72, 580, "Nested two")
	form3 = text(fonts, b"U", 12, 0, 0, "Turned text")
	inner = text(fonts, b"U", 5, 0, 0, "zz")
	names = ["u%d" % code for code in range(1, len(fonts.codes) + 1)]
	cmap = document.add(stream(b"<< >>", fonts.tounicode()))
	font_u = type3(document, names, cmap)
	glyph_cmap = document.add(stream(b"<< >>", b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
					  b"1 begincodespacerange <00> <FF> endcodespacerange\n1 beginbfchar <01> <0061> endbfchar\n"
					  b"endcmap CMapName currentdict /CMap defineresource pop end end"))
	font_p = type3(document, ["a"], glyph_cmap, {"a": b"500 0 d0 " + inner},
		       b"<< /Font << /U %d 0 R >> >>" % font_u)
	fonts_entry = b"/Font << /U %d 0 R /P %d 0 R >>" % (font_u, font_p)
	form2_number = document.add(stream(b"<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << "
					   + fonts_entry + b" >> >>", form2))
	form1_number = document.add(stream(b"<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << "
					   + fonts_entry + b" /XObject << /Fm2 %d 0 R >> >> >>" % form2_number, form1))
	form3_number = document.add(stream(b"<< /Type /XObject /Subtype /Form /BBox [0 0 200 50] /Matrix [0 1 -1 0 400 100]"
					   b" /Resources << " + fonts_entry + b" >> >>", form3))
	resources = b"<< " + fonts_entry + b" /XObject << /Fm1 %d 0 R /Fm3 %d 0 R >> >>" % (form1_number, form3_number)
	pages = [page(document, page_content, resources)]
	finish(document, pages, folder / "forms.pdf")


def make_find(folder: Path) -> None:
	"""find.pdf: the lines the find rules are checked on, an unreadable font, and pages to count."""
	fonts = Fonts()
	document = Document()
	contents = []
	first = b""
	y = 740
	for line in FIND_PAGE:
		first += text(fonts, b"U", 12, 72, y, line)
		y -= 20
	contents.append(first)
	contents.append(b"BT /N 12 Tf 72 740 Td <010203> Tj ET\n" + text(fonts, b"U", 12, 72, 700, "page two has a lazy dog"))
	for number in range(3, 33):
		contents.append(text(fonts, b"U", 12, 72, 700, "lazy dog on page %d" % number))
	names = ["u%d" % code for code in range(1, len(fonts.codes) + 1)]
	cmap = document.add(stream(b"<< >>", fonts.tounicode()))
	font_u = type3(document, names, cmap)
	font_n = type3(document, ["kei1", "kei2", "kei3"], None)
	resources = b"<< /Font << /U %d 0 R /N %d 0 R >> >>" % (font_u, font_n)
	pages = [page(document, content, resources) for content in contents]
	finish(document, pages, folder / "find.pdf")


def main() -> int:
	if len(sys.argv) != 2:
		print(__doc__, file=sys.stderr)
		return 2
	folder = Path(sys.argv[1])
	folder.mkdir(parents=True, exist_ok=True)
	make_forms(folder)
	make_find(folder)
	return 0


if __name__ == "__main__":
	sys.exit(main())
