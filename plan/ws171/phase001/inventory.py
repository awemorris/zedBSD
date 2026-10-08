#!/usr/bin/env python3
"""ws171-p001: the inventory of include/hal/hal.h's functions -- each declaration, its comment and how complete a
contract it states, and which architectures define it (a definition "hal_name(" at a line's start in src/hal/<arch>/
or in src/hal/*.c, which every architecture may share).  Writes a Markdown table on stdout.

  python3 plan/ws171/phase001/inventory.py > plan/ws171/phase001/inventory.md
Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
HAL = ROOT / "include/hal/hal.h"
ARCHES = ("amd64", "i386", "arm64", "sparcv9", "m68k")

lines = HAL.read_text().splitlines()
functions = []
for index, line in enumerate(lines):
	match = re.match(r"^(hal_[a-z0-9_]+)\(", line)
	if not match:
		continue
	# The comment above the return type's line.
	end = index - 2
	while end >= 0 and lines[end].strip() == "":
		end -= 1
	comment = []
	if end >= 0 and lines[end].strip().endswith("*/"):
		start = end
		while start >= 0 and not lines[start].lstrip().startswith("/*"):
			start -= 1
		comment = [text.strip().lstrip("/*").strip() for text in lines[start:end + 1]]
	text = " ".join(part for part in comment if part)
	functions.append((match.group(1), index + 1, text))


def grade(text: str) -> str:
	"""none, placeholder, short (what only), or contract (states returns/errors or context)."""
	if not text:
		return "none"
	if "XXX: Add explanation" in text:
		return "placeholder"
	contract = re.search(r"\bReturns?\b|HAL_OK|HAL_ERR|interrupts?\b|lock|context|caller", text)
	words = len(text.split())
	if contract and words >= 25:
		return "contract"
	return "short"


def defined_in(name: str) -> str:
	"""The architectures whose sources define the function (shared: a file directly under src/hal)."""
	found = []
	pattern = re.compile(rf"^{re.escape(name)}\(", re.M)
	for arch in ARCHES:
		for path in (ROOT / "src/hal" / arch).rglob("*.[cS]"):
			if pattern.search(path.read_text(errors="replace")):
				found.append(arch)
				break
	for path in (ROOT / "src/hal").glob("*.c"):
		if pattern.search(path.read_text(errors="replace")):
			found.append(f"shared:{path.name}")
	x86 = ROOT / "src/hal/x86"
	if x86.exists() and any(pattern.search(p.read_text(errors="replace")) for p in x86.rglob("*.[cS]")):
		found.append("x86(common)")
	return ", ".join(found) if found else "(macro or none)"


counts = {}
print("| # | function | hal.h line | comment | words | defined in |")
print("| --- | --- | --- | --- | --- | --- |")
for number, (name, line, text) in enumerate(functions, 1):
	kind = grade(text)
	counts[kind] = counts.get(kind, 0) + 1
	print(f"| {number} | `{name}` | {line} | {kind} | {len(text.split())} | {defined_in(name)} |")
print()
print("Totals: " + ", ".join(f"{kind} {count}" for kind, count in sorted(counts.items())) + f", all {len(functions)}")
