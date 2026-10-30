#!/usr/bin/env python3
"""Converts acpiexec's namespace dump to the lines aml-host --dump prints.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    acpiexec -l -di -b "namespace" TABLE... | acpiexec-namespace.py > oracle.txt

Each node becomes "PATH TYPE ATTRIBUTES" with the attributes that identify
it (an integer's value, a region's space and place, a field's region and
bits, ...), in the same form as aml-host --dump.  acpiexec's own test node
\\_TI_ is left out.
"""
import re
import sys

LINE = re.compile(r"^\s*(\d+)\s+([A-Z_][A-Z0-9_]{3})\s+(\S+)\s+0x[0-9a-f]+\s+\d+\s?(.*)$")


def hexnum(text):
	return "%X" % int(text, 16)


def attributes(kind, rest):
	if kind == "Integer":
		match = re.match(r"= ([0-9A-F]+)", rest)
		return "Integer " + hexnum(match.group(1))
	if kind == "String":
		match = re.match(r'Len [0-9A-F]+ "(.*)"', rest)
		return 'String "%s"' % match.group(1).replace('\\\\', '\\')
	if kind == "Buffer":
		match = re.match(r"Len ([0-9A-F]+)", rest)
		return "Buffer len=" + hexnum(match.group(1))
	if kind == "Package":
		match = re.match(r"Elements ([0-9A-F]+)", rest)
		return "Package count=" + hexnum(match.group(1))
	if kind == "Method":
		match = re.match(r"Args (\d+)", rest)
		return "Method args=" + match.group(1)
	if kind == "Region":
		match = re.match(r"\[(\S+)\] Addr ([0-9A-F]+) Len ([0-9A-F]+)", rest)
		if not match:
			match = re.match(r"\[(\S+)\]", rest)
			return "Region %s unevaluated" % match.group(1)
		return "Region %s addr=%s len=%s" % (match.group(1), hexnum(match.group(2)), hexnum(match.group(3)))
	if kind == "RegionField":
		match = re.match(r"Rgn \[(\S+)\] Off ([0-9A-F]+) Len ([0-9A-F]+)", rest)
		return "RegionField rgn=%s off=%s len=%s" % (match.group(1), hexnum(match.group(2)), hexnum(match.group(3)))
	if kind == "IndexField":
		match = re.match(r"Idx \[(\S+)\] Dat \[(\S+)\] Off ([0-9A-F]+) Len ([0-9A-F]+)", rest)
		return "IndexField idx=%s dat=%s off=%s len=%s" % (
			match.group(1), match.group(2), hexnum(match.group(3)), hexnum(match.group(4)))
	if kind == "BankField":
		match = re.match(r"Rgn \[(\S+)\] Bnk \[(\S+)\] Off ([0-9A-F]+) Len ([0-9A-F]+)", rest)
		if match:
			return "BankField rgn=%s bnk=%s off=%s len=%s" % (
				match.group(1), match.group(2), hexnum(match.group(3)), hexnum(match.group(4)))
		return "BankField " + rest
	if kind == "BufferField":
		match = re.match(r"Buf \[(\S+)\] Off ([0-9A-F]+) Len ([0-9A-F]+)", rest)
		if match:
			return "BufferField off=%s len=%s" % (hexnum(match.group(2)), hexnum(match.group(3)))
		return "BufferField unevaluated"
	if kind == "Processor":
		match = re.match(r"ID ([0-9A-F]+) Len ([0-9A-F]+) Addr ([0-9A-F]+)", rest)
		return "Processor id=%s len=%s addr=%s" % (match.group(1), match.group(2), hexnum(match.group(3)))
	if kind == "Alias":
		match = re.match(r"Target (\S+)", rest)
		return "Alias target=" + match.group(1)
	return kind


def main():
	stack = []
	started = False
	for line in sys.stdin:
		if line.startswith("ACPI Namespace"):
			started = True
			continue
		if not started:
			continue
		match = LINE.match(line.rstrip("\n"))
		if not match:
			continue
		depth = int(match.group(1))
		name = match.group(2)
		kind = match.group(3)
		rest = match.group(4).strip()
		del stack[depth:]
		stack.append(name)
		path = "\\" + ".".join(stack)
		if path.startswith("\\_TI_"):
			continue
		print("%s %s" % (path, attributes(kind, rest)))
	return 0


if __name__ == "__main__":
	sys.exit(main())
