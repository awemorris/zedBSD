#!/usr/bin/env python3
"""Lays ACPI tables out in a simulated physical memory, as firmware does.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    make-firmware.py [--rsdt] OUTPUT_DIR TABLE.dat...

The tables are the files acpidump -b (or qemu-acpi-dump.py) writes; one
must be dsdt.dat.  facp.dat is used when given (its DSDT and FACS pointers
are patched), otherwise a minimal FADT is made.  An RSDP of revision 2 and
an XSDT that lists every table but the DSDT and the FACS are made; with
--rsdt, an RSDP of revision 0 and an RSDT of 32-bit entries instead.

OUTPUT_DIR/memory.txt describes the memory for aml-host --firmware:
    rsdp ADDRESS
    ADDRESS FILE        (one line per table, the file's bytes at ADDRESS)
"""
import os
import struct
import sys

RSDP_ADDRESS = 0x000E0000
XSDT_ADDRESS = 0x7F000000
TABLES_ADDRESS = 0x7F001000


def checksum(data):
	return (-sum(data)) & 0xFF


def fix_checksum(table):
	table = bytearray(table)
	table[9] = 0
	table[9] = checksum(table)
	return bytes(table)


def header(signature, length, revision):
	return bytearray(struct.pack("<4sIBB6s8sI4sI", signature, length, revision, 0,
		b"ZEDBSD", b"WS049FW ", 1, b"ZED ", 1))


def main():
	arguments = sys.argv[1:]
	use_rsdt = False
	if arguments and arguments[0] == "--rsdt":
		use_rsdt = True
		arguments = arguments[1:]
	if len(arguments) < 2:
		print(__doc__, file=sys.stderr)
		return 2
	output = arguments[0]
	os.makedirs(output, exist_ok=True)
	tables = {}
	order = []
	for path in arguments[1:]:
		name = os.path.basename(path)
		with open(path, "rb") as stream:
			tables[name] = stream.read()
		order.append(name)
	if "dsdt.dat" not in tables:
		print("dsdt.dat is required", file=sys.stderr)
		return 2

	# Places each table on its own pages.
	addresses = {}
	next_address = TABLES_ADDRESS
	for name in order:
		addresses[name] = next_address
		next_address += (len(tables[name]) + 0xFFF) & ~0xFFF

	# The FADT points at the DSDT and the FACS.
	if "facp.dat" in tables:
		fadt = bytearray(tables["facp.dat"])
	else:
		fadt = header(b"FACP", 276, 6) + bytearray(276 - 36)
		addresses["facp.dat"] = next_address
		order.append("facp.dat")
	facs = addresses.get("facs.dat", 0)
	struct.pack_into("<I", fadt, 36, facs & 0xFFFFFFFF)
	struct.pack_into("<I", fadt, 40, addresses["dsdt.dat"])
	if len(fadt) >= 140:
		struct.pack_into("<Q", fadt, 132, facs)
	if len(fadt) >= 148:
		struct.pack_into("<Q", fadt, 140, addresses["dsdt.dat"])
	tables["facp.dat"] = fix_checksum(fadt)

	# The XSDT lists every table but the DSDT, the FACS and old root tables.
	listed = [name for name in order if name not in ("dsdt.dat", "facs.dat", "rsdt.dat", "xsdt.dat")]
	if use_rsdt:
		xsdt = header(b"RSDT", 36 + 4 * len(listed), 1)
		for name in listed:
			xsdt += struct.pack("<I", addresses[name])
	else:
		xsdt = header(b"XSDT", 36 + 8 * len(listed), 1)
		for name in listed:
			xsdt += struct.pack("<Q", addresses[name])
	xsdt = fix_checksum(xsdt)

	# The RSDP: revision 0 with the RSDT only, or revision 2 with the XSDT.
	if use_rsdt:
		rsdp = bytearray(struct.pack("<8sB6sBI", b"RSD PTR ", 0, b"ZEDBSD", 0, XSDT_ADDRESS))
		rsdp[8] = checksum(rsdp)
	else:
		rsdp = bytearray(struct.pack("<8sB6sBIIQB3s", b"RSD PTR ", 0, b"ZEDBSD", 2, 0, 36, XSDT_ADDRESS, 0, b"\0\0\0"))
		rsdp[8] = checksum(rsdp[:20])
		rsdp[32] = checksum(rsdp)

	# Writes the files and the description.
	lines = ["rsdp 0x%X" % RSDP_ADDRESS]
	with open(os.path.join(output, "rsdp.bin"), "wb") as stream:
		stream.write(rsdp)
	lines.append("0x%X %s" % (RSDP_ADDRESS, os.path.join(output, "rsdp.bin")))
	with open(os.path.join(output, "xsdt.bin"), "wb") as stream:
		stream.write(xsdt)
	lines.append("0x%X %s" % (XSDT_ADDRESS, os.path.join(output, "xsdt.bin")))
	for name in order:
		if name in ("rsdt.dat", "xsdt.dat"):
			continue
		path = os.path.join(output, name)
		with open(path, "wb") as stream:
			stream.write(tables[name])
		lines.append("0x%X %s" % (addresses[name], path))
	with open(os.path.join(output, "memory.txt"), "w") as stream:
		stream.write("\n".join(lines) + "\n")
	print(os.path.join(output, "memory.txt"))
	return 0


if __name__ == "__main__":
	sys.exit(main())
