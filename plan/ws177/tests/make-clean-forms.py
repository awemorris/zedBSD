#!/usr/bin/env python3
"""make-clean-forms.py: the PDF of ws177-p011's host test (Save Clean Copy: resources of forms, Type 3 fonts and
tiling patterns; the attachments' name tree).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    make-clean-forms.py OUT

One page of 300 x 300 points draws the form Fm1 (its own resources: ImIn used, ImOut not), the form Fm2 (no resources
of its own: it draws the page's ImB), the Type 3 font T3 (its own resources: its glyph draws ImG, ImH unused) and the
tiling pattern P1 (no resources of its own: it draws the page's ImP).  The page's ImC and ImQ are used by nothing.
Each image's twelve bytes are its name's letters repeated, which the test looks for in the copy.  The catalog's
EmbeddedFiles name tree has two leaves: (a.txt) and (kei-notes.bin), the latter's file specification written in the
leaf, then (z.txt); the catalog's /AF lists a direct specification of kei-notes.bin and a.txt's.
"""
import sys
from pathlib import Path


def stream(dictionary: bytes, data: bytes) -> bytes:
	"""A stream object's body: its dictionary with its length, and its data."""
	return dictionary[:-2] + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def image(marker: bytes) -> bytes:
	"""A 2 x 2 RGB image whose bytes are a marker."""
	return stream(b"<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 >>",
		(marker * 12)[:12])


def main() -> int:
	if len(sys.argv) != 2:
		print(__doc__, file=sys.stderr)
		return 2
	page = (b"q 1 0 0 1 10 10 cm /Fm1 Do Q q 1 0 0 1 150 10 cm /Fm2 Do Q "
		b"BT /T3 40 Tf 10 200 Td (a) Tj ET /Pattern cs /P1 scn 150 150 100 100 re f\n")
	objects = [
		# 1 the catalog, 2 the page tree, 3 the page, 4 its content
		b"<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 20 0 R >> /AF [ << /Type /Filespec /F (kei-notes.bin) >> 23 0 R ] >>",
		b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Contents 4 0 R "
		b"/Resources << /XObject << /Fm1 5 0 R /Fm2 6 0 R /ImB 11 0 R /ImC 12 0 R /ImP 13 0 R /ImQ 14 0 R >> "
		b"/Font << /T3 7 0 R >> /Pattern << /P1 9 0 R >> >> >>",
		stream(b"<< >>", page),
		# 5 Fm1 with its own resources, 6 Fm2 without
		stream(b"<< /Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /ImIn 15 0 R /ImOut 16 0 R >> >> >>",
			b"q 100 0 0 100 0 0 cm /ImIn Do Q\n"),
		stream(b"<< /Type /XObject /Subtype /Form /BBox [0 0 100 100] >>", b"q 100 0 0 100 0 0 cm /ImB Do Q\n"),
		# 7 T3 with its own resources, 8 its glyph
		b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] "
		b"/CharProcs << /a 8 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 "
		b"/Widths [1000] /Resources << /XObject << /ImG 17 0 R /ImH 18 0 R >> >> >>",
		stream(b"<< >>", b"1000 0 d0 q 1000 0 0 1000 0 0 cm /ImG Do Q\n"),
		# 9 P1 without resources of its own, 10 the information dictionary
		stream(b"<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 50 50] /XStep 50 /YStep 50 >>",
			b"q 50 0 0 50 0 0 cm /ImP Do Q\n"),
		b"<< /Title (Clean forms) >>",
		# 11 to 18 the images
		image(b"IMB"), image(b"IMC"), image(b"IMP"), image(b"IMQ"),
		image(b"ININ"), image(b"OUT"), image(b"GLY"), image(b"HHH"),
		# 19 an embedded file's stream
		stream(b"<< /Type /EmbeddedFile >>", b"hello"),
		# 20 the name tree's root, 21 and 22 its leaves, 23 a.txt's specification, 24 z.txt's
		b"<< /Kids [21 0 R 22 0 R] >>",
		b"<< /Limits [(a.txt) (kei-notes.bin)] /Names [(a.txt) 23 0 R (kei-notes.bin) "
		b"<< /Type /Filespec /F (kei-notes.bin) /UF (kei-notes.bin) /EF << /F 19 0 R >> >>] >>",
		b"<< /Limits [(z.txt) (z.txt)] /Names [(z.txt) 24 0 R] >>",
		b"<< /Type /Filespec /F (a.txt) /UF (a.txt) /EF << /F 19 0 R >> >>",
		b"<< /Type /Filespec /F (z.txt) /UF (z.txt) /EF << /F 19 0 R >> >>",
	]
	out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
	offsets = []
	for number, body in enumerate(objects, start=1):
		offsets.append(len(out))
		out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
	table = len(out)
	out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
	for offset in offsets:
		out += b"%010d 00000 n \n" % offset
	out += b"trailer\n<< /Size %d /Root 1 0 R /Info 10 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, table)
	Path(sys.argv[1]).write_bytes(bytes(out))
	return 0


if __name__ == "__main__":
	sys.exit(main())
