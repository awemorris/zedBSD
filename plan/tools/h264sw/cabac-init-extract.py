#!/usr/bin/env python3
# Cross-check of the CABAC initialization table (h264sw-cabac-init.c) against the specification.
#
# The (m, n) pair of every ctxIdx and set is read from the pdftotext -layout text of ITU-T H.264
# (08/2021) Tables 9-12 to 9-24 (PDF pages 250 to 258) and compared with the dump the C test prints.
#   python3 -I plan/tools/h264sw/cabac-init-extract.py <spec.pdf> <dump.txt>
# <dump.txt> is the output of "test-cabac_init --dump": one line "ctxIdx set m n" per pair
# (460 x 4 = 1840 lines).  Last line: "cabac-init-extract: PASS (1840 pairs)" or "... FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import subprocess
import sys

SETS = 4
CONTEXTS = 460

# Table 9-14 (ctxIdx 24..39) writes the m of some cells over two lines ("-4" above "6" is -46), which
# pdftotext cannot join, so its m values are entered from the page image (PDF page 251) and only its
# n rows are read from the text.  The index is cabac_init_idc 0, 1, 2.
TABLE_9_14_M = [
	[18, 9, 29, 26, 16, 9, -46, -20, 1, -13, -11, 1, -6, -17, -6, 9],
	[26, 19, 40, 57, 41, 26, -45, -15, -4, -6, -13, 5, 6, -13, 0, 8],
	[20, 20, 29, 54, 37, 12, -32, -22, -2, -4, -24, 5, -6, -14, -6, 4],
]


def number(token):
	"""An integer written with the Unicode minus sign, or None for anything else ("na" is 0 later)."""
	text = token.replace("−", "-")
	if re.fullmatch(r"-?\d+", text):
		return int(text)
	return None


def split_tables(lines):
	"""The lines of each table, keyed by its number ("9-12"), from its title up to the next title."""
	title = re.compile(r"Table (9-\d+) – Values of variables m and n")
	found = {}
	current = None
	for line in lines:
		match = title.search(line)
		if match:
			current = match.group(1)
			found[current] = []
			continue
		if current is not None:
			found[current].append(line)
	return found


def read_row_table(lines, pairs):
	"""Tables 9-18 to 9-24: each line holds ctxIdx and four (m, n) pairs, once or twice."""
	for line in lines:
		tokens = [number(token) for token in line.split()]
		if None in tokens or len(tokens) not in (9, 18):
			continue
		for start in range(0, len(tokens), 9):
			row = tokens[start:start + 9]
			ctx = row[0]
			if ctx in pairs:
				raise SystemExit("ctxIdx %d read twice" % ctx)
			pairs[ctx] = [(row[1 + 2 * s], row[2 + 2 * s]) for s in range(SETS)]


def read_column_table(table, lines, pairs):
	"""Tables 9-12, 9-13, 9-15, 9-16, 9-17: a ctxIdx header line, then m and n lines per set."""
	header = None
	current = None
	m_values = {}
	n_values = {}
	for line in lines:
		words = line.split()
		if not words:
			continue
		numbers = [number(word) for word in words]
		if header is None:
			if None not in numbers and len(numbers) >= 6:
				header = numbers
			continue
		# A row is "[set label] m values" or "n values"; "I slices" labels the I set of Table 9-16.
		if words[:2] == ["I", "slices"]:
			current = 0
			words = words[2:]
		elif words[0] in ("0", "1", "2") and len(words) > 1 and words[1] == "m":
			current = 1 + int(words[0])
			words = words[1:]
		if not words or words[0] not in ("m", "n"):
			continue
		values = [0 if word == "na" else number(word) for word in words[1:]]
		if None in values:
			raise SystemExit("Table %s: unreadable row: %s" % (table, line))
		if len(values) != len(header):
			raise SystemExit("Table %s: %d values for %d ctxIdx: %s" % (table, len(values), len(header), line))
		if words[0] == "m":
			m_values[current] = values
		else:
			n_values[current] = values
	if header is None:
		raise SystemExit("Table %s: no ctxIdx header" % table)
	if table in ("9-12", "9-17"):
		# One column for all slice types: the single row belongs to every set.
		for column, ctx in enumerate(header):
			pairs[ctx] = [(m_values[None][column], n_values[None][column])] * SETS
		return
	for column, ctx in enumerate(header):
		entry = [(0, 0)] * SETS
		for current in range(SETS):
			if current in m_values:
				entry[current] = (m_values[current][column], n_values[current][column])
		pairs[ctx] = entry


def read_table_9_14(lines, pairs):
	"""Reads the three n rows of Table 9-14 in order (idc 0, 1, 2) and joins them with the m values."""
	header = None
	rows = []
	for line in lines:
		words = line.split()
		if not words:
			continue
		numbers = [number(word) for word in words]
		if header is None:
			if None not in numbers and len(numbers) >= 6:
				header = numbers
			continue
		if words[0] == "n":
			rows.append([number(word) for word in words[1:]])
	if header is None or len(rows) != 3:
		raise SystemExit("Table 9-14: %d n rows" % len(rows))
	for column, ctx in enumerate(header):
		entry = [(0, 0)]
		for idc in range(3):
			entry.append((TABLE_9_14_M[idc][column], rows[idc][column]))
		pairs[ctx] = entry


def read_spec(pdf):
	"""Every (m, n) of the specification, keyed by ctxIdx, as four pairs (I, idc 0, 1, 2)."""
	result = subprocess.run(["pdftotext", "-layout", "-f", "250", "-l", "258", pdf, "-"],
	    check=True, capture_output=True, text=True)
	tables = split_tables(result.stdout.splitlines())
	pairs = {}
	for table in ("9-12", "9-13", "9-15", "9-16", "9-17"):
		read_column_table(table, tables[table], pairs)
	read_table_9_14(tables["9-14"], pairs)
	for table in ("9-18", "9-19", "9-20", "9-21", "9-22", "9-23", "9-24"):
		read_row_table(tables[table], pairs)
	# ctxIdx 276 has no row in any table (end_of_slice_flag).
	pairs[276] = [(0, 0)] * SETS
	return pairs


def main():
	if len(sys.argv) != 3:
		print("usage: cabac-init-extract.py <spec.pdf> <dump.txt>")
		return 2
	spec = read_spec(sys.argv[1])
	dump = {}
	for line in open(sys.argv[2], encoding="ascii"):
		fields = line.split()
		if len(fields) != 4:
			continue
		ctx, set_index, m, n = (int(field) for field in fields)
		dump[(ctx, set_index)] = (m, n)
	failures = 0
	count = 0
	for ctx in range(CONTEXTS):
		if ctx not in spec:
			print("cabac-init-extract: ctxIdx %d not read from the specification" % ctx)
			failures += 1
			continue
		for set_index in range(SETS):
			count += 1
			if dump.get((ctx, set_index)) != spec[ctx][set_index]:
				print("mismatch ctxIdx %d set %d: C %s, specification %s" %
				    (ctx, set_index, dump.get((ctx, set_index)), spec[ctx][set_index]))
				failures += 1
	extra = [ctx for ctx in spec if not 0 <= ctx < CONTEXTS]
	if extra or count != CONTEXTS * SETS or len(dump) != CONTEXTS * SETS:
		print("cabac-init-extract: sizes: spec extra %s, compared %d, dump %d" % (extra, count, len(dump)))
		failures += 1
	if failures:
		print("cabac-init-extract: FAIL (%d)" % failures)
		return 1
	print("cabac-init-extract: PASS (%d pairs)" % count)
	return 0


sys.exit(main())
