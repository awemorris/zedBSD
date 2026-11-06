#!/usr/bin/env python3
"""installer-units.py: cuts the installer's whole root into units and writes their index.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    installer-units.py --manifest FULL.manifest --owners FULL.owners --tree ROOT \\
        --items ITEMS --version VERSION --mtime SECONDS \\
        --boot NAME=DESTINATION=FILE ... --output DIRECTORY

DIRECTORY becomes what the installer's image has at /opt/installer: index,
boot/NAME for each --boot, and units/UNIT.tar.gz for each unit of the
manifest (installer-manifest.py).

A unit's tarball is ustar compressed with deflate (the gzip format).  Every
entry belongs to 0:0 and has the manifest's mode; a directory has the root's.
Each tarball has the directories above its files, and the base's also the
ones the root has empty (/tmp, /home, /var/empty).  A name or a link target
ustar cannot hold (a name over 100 bytes with no '/' that splits it into a
prefix of 155 and a name of 100, a target over 100) is refused, as are a device, a FIFO, a socket and a file the root has
twice under one inode, so that the installer's extraction has only files,
directories and symbolic links to make.

The index is text, a record a line with its fields apart by a TAB:

    format  1
    unit    NAME LABEL GROUP VERSION TAR-BYTES BYTES ENTRIES SHA-256 REQUIRES
    boot    NAME DESTINATION BYTES SHA-256
    setid   UNIT PATH MODE
    license UNIT PACKAGE PATH
    item    ID PARENT DEFAULT LABEL UNITS

GROUP is base, desktop, firmware or package.  BYTES is what the unit's
regular files hold, ENTRIES its files, links and directories (an inode
each).  REQUIRES are the units it needs, apart by a space, '-' for none; a
requirement on a program of the same unit is not one, and a cycle among the
units is refused.  A boot file is NAME at DESTINATION (esp:PATH on the ESP,
or a path of the installed root).  setid lists every set-user-ID and
set-group-ID file.  license lists the files under /usr/share/licenses of the
firmware.  An item is a choice of the installer's page of what to install:
the fixed ones of ITEMS (ID PARENT DEFAULT LABEL UNITS, DEFAULT required,
on or off) and, under the item 'other', one for each package unit no fixed
item names.  An item that names a unit the root has not is refused.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import stat
import sys
import tarfile

# The groups of the units that are not packages, with the name each is shown by.
GROUP_LABELS = {"base": "Base System", "desktop": "Keiland Desktop", "firmware": "Firmware"}

# The item the package units that no fixed item names are put under.
OTHER_ITEM = "other"


class Problems:
	"""What does not fit, reported together at the end."""

	def __init__(self):
		self.lines: list[str] = []

	def add(self, line: str) -> None:
		"""Records one problem."""
		self.lines.append(line)

	def stop_if_any(self) -> None:
		"""Reports the problems and stops when there are any."""
		if not self.lines:
			return
		for line in self.lines:
			print(f"installer-units: {line}", file=sys.stderr)
		print(f"installer-units: {len(self.lines)} problems", file=sys.stderr)
		sys.exit(1)


def read_programs(path: str) -> dict[str, dict]:
	"""The programs of the owners file: name -> menu, path, label, version and requirements."""
	programs: dict[str, dict] = {}
	with open(path, encoding="utf-8") as stream:
		for raw in stream:
			fields = raw.rstrip("\n").split("\t")
			if fields[0] != "P":
				continue
			fields += [""] * (9 - len(fields))
			programs[fields[1]] = {
				"menu": fields[2],
				"path": fields[3],
				"requires": fields[6].split(),
				"label": fields[7],
				"version": fields[8],
			}
	return programs


def read_manifest(path: str) -> list[tuple[str, str, str, str, int]]:
	"""The manifest's lines: path, unit, program, kind (f or l) and mode."""
	entries = []
	with open(path, encoding="utf-8") as stream:
		for raw in stream:
			path_, unit, program, kind, mode = raw.rstrip("\n").split("\t")
			entries.append((path_, unit, program, kind, int(mode, 8)))
	return entries


def unit_of_program(program: dict) -> str | None:
	"""The unit a program's files go to, as installer-manifest.py names it."""
	if program["menu"] in GROUP_LABELS:
		return program["menu"]
	if program["menu"].startswith("packages/") and program["path"]:
		return program["path"].replace("/", "-")
	return None


def unit_requirements(programs: dict[str, dict], units: set[str]) -> dict[str, set[str]]:
	"""The units each unit needs, from its programs' requirements.

	A requirement names a program by its registered path.  A program whose unit
	has no file (a library merged into another's files) passes its own
	requirements on.
	"""
	by_path = {program["path"]: name for name, program in programs.items() if program["path"]}

	def needed_units(name: str, seen: set[str]) -> set[str]:
		"""The units with files that a program's requirements reach."""
		found: set[str] = set()
		for requirement in programs[name]["requires"]:
			other = by_path.get(requirement)
			if other is None or other in seen:
				continue
			seen.add(other)
			unit = unit_of_program(programs[other])
			if unit in units:
				found.add(unit)
			else:
				found |= needed_units(other, seen)
		return found

	requirements = {unit: set() for unit in units}
	for name, program in programs.items():
		unit = unit_of_program(program)
		if unit not in units:
			continue
		requirements[unit] |= needed_units(name, {name})
	for unit in requirements:
		requirements[unit].discard(unit)
	return requirements


def find_cycle(requirements: dict[str, set[str]]) -> list[str] | None:
	"""A cycle among the units' requirements, or None."""
	state: dict[str, int] = {}
	trail: list[str] = []

	def visit(unit: str) -> list[str] | None:
		"""Walks the requirements depth first; 1 is on the trail, 2 is done."""
		state[unit] = 1
		trail.append(unit)
		for other in sorted(requirements[unit]):
			if state.get(other) == 1:
				return trail[trail.index(other):] + [other]
			if other not in state:
				cycle = visit(other)
				if cycle is not None:
					return cycle
		trail.pop()
		state[unit] = 2
		return None

	for unit in sorted(requirements):
		if unit not in state:
			cycle = visit(unit)
			if cycle is not None:
				return cycle
	return None


def tar_entry(name: str, kind: bytes, mode: int, mtime: int) -> tarfile.TarInfo:
	"""An entry of 0:0 with a mode and a time."""
	info = tarfile.TarInfo(name)
	info.type = kind
	info.mode = mode
	info.uid = 0
	info.gid = 0
	info.uname = "root"
	info.gname = "wheel"
	info.mtime = mtime
	return info


def write_unit(output: str, tree: str, directories: list[tuple[str, int]], entries: list[tuple[str, str, int]],
               mtime: int, problems: Problems) -> tuple[int, int]:
	"""Writes one unit's tarball; returns the bytes of its regular files and its entries."""
	payload = 0
	seen_inodes: dict[tuple[int, int], str] = {}
	with open(output, "wb") as raw:
		with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=6) as compressed:
			with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
				for path, mode in directories:
					try:
						archive.addfile(tar_entry(path, tarfile.DIRTYPE, mode, mtime))
					except ValueError as error:
						problems.add(f"/{path}: {error}")
				for path, kind, mode in entries:
					full = os.path.join(tree, path)
					info = os.lstat(full)
					try:
						if kind == "l":
							entry = tar_entry(path, tarfile.SYMTYPE, 0o777, mtime)
							entry.linkname = os.readlink(full)
							archive.addfile(entry)
							continue
						if not stat.S_ISREG(info.st_mode):
							problems.add(f"neither a file nor a link: /{path}")
							continue
						key = (info.st_dev, info.st_ino)
						if info.st_nlink > 1 and key in seen_inodes:
							problems.add(f"a hard link of /{seen_inodes[key]}: /{path}")
							continue
						seen_inodes[key] = path
						entry = tar_entry(path, tarfile.REGTYPE, mode, mtime)
						entry.size = info.st_size
						with open(full, "rb") as source:
							archive.addfile(entry, source)
						payload += info.st_size
					except ValueError as error:
						problems.add(f"/{path}: {error}")
	return payload, len(directories) + len(entries)


def file_digest(path: str) -> tuple[int, str]:
	"""A file's size and SHA-256."""
	digest = hashlib.sha256()
	size = 0
	with open(path, "rb") as stream:
		for block in iter(lambda: stream.read(1 << 20), b""):
			digest.update(block)
			size += len(block)
	return size, digest.hexdigest()


def read_items(path: str) -> list[list[str]]:
	"""The fixed items: ID PARENT DEFAULT LABEL UNITS, a line each, '#' a comment."""
	items = []
	with open(path, encoding="utf-8") as stream:
		for number, raw in enumerate(stream, 1):
			line = raw.rstrip("\n")
			if not line or line.startswith("#"):
				continue
			fields = line.split("\t")
			if len(fields) != 5 or fields[2] not in ("required", "on", "off"):
				raise SystemExit(f"installer-units: {path}:{number}: not ID PARENT DEFAULT LABEL UNITS")
			items.append(fields)
	return items


def ancestors(path: str) -> list[str]:
	"""The directories above a path of the root, the top first."""
	parts = path.split("/")[:-1]
	return ["/".join(parts[:count]) for count in range(1, len(parts) + 1)]


def main() -> int:
	"""Writes the units, the boot files and the index, or fails naming what does not fit."""
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument("--manifest", required=True)
	parser.add_argument("--owners", required=True)
	parser.add_argument("--tree", required=True)
	parser.add_argument("--items", required=True)
	parser.add_argument("--version", required=True)
	parser.add_argument("--mtime", type=int, default=0)
	parser.add_argument("--boot", action="append", default=[])
	parser.add_argument("--output", required=True)
	arguments = parser.parse_args()

	problems = Problems()
	programs = read_programs(arguments.owners)
	manifest = read_manifest(arguments.manifest)

	# The units' entries, and each unit's group, label and version.
	unit_entries: dict[str, list[tuple[str, str, int]]] = {}
	unit_program: dict[str, str] = {}
	for path, unit, program, kind, mode in manifest:
		unit_entries.setdefault(unit, []).append((path, kind, mode))
		if program != "-" and unit not in GROUP_LABELS:
			unit_program.setdefault(unit, program)
	units = set(unit_entries)
	if "base" not in units:
		problems.add("the manifest has no base")

	# The directories of the root, and which of them each unit carries.
	tree_directories: dict[str, int] = {}
	occupied: set[str] = set()
	for top, directories, files in os.walk(arguments.tree):
		for name in directories + files:
			full = os.path.join(top, name)
			path = os.path.relpath(full, arguments.tree)
			if name in directories and not os.path.islink(full):
				tree_directories[path] = stat.S_IMODE(os.lstat(full).st_mode)
			elif path != ".stamp":
				occupied.update(ancestors(path))
	empty_directories: set[str] = set()
	for path in tree_directories:
		if path not in occupied:
			empty_directories.add(path)
			empty_directories.update(ancestors(path))

	# The requirements, which must not go round.
	requirements = unit_requirements(programs, units)
	cycle = find_cycle(requirements)
	if cycle is not None:
		problems.add(f"the units' requirements go round: {' -> '.join(cycle)}")
	problems.stop_if_any()

	os.makedirs(os.path.join(arguments.output, "units"))
	os.makedirs(os.path.join(arguments.output, "boot"))
	records: list[str] = ["format\t1\n"]

	for unit in sorted(units):
		entries = sorted(unit_entries[unit])
		wanted = set(empty_directories) if unit == "base" else set()
		for path, _kind, _mode in entries:
			wanted.update(ancestors(path))
		directories = [(path, tree_directories[path]) for path in sorted(wanted) if path in tree_directories]
		missing = sorted(wanted - set(tree_directories))
		for path in missing:
			problems.add(f"a directory of {unit} is not one of the root: /{path}")
		tarball = os.path.join(arguments.output, "units", unit + ".tar.gz")
		payload, count = write_unit(tarball, arguments.tree, directories, entries, arguments.mtime, problems)
		size, digest = file_digest(tarball)
		if unit in GROUP_LABELS:
			group, label, version = unit, GROUP_LABELS[unit], arguments.version
		else:
			program = programs.get(unit_program.get(unit, ""), {})
			group = "package"
			label = program.get("label") or unit
			version = program.get("version") or arguments.version
		required = " ".join(sorted(requirements[unit])) or "-"
		records.append(f"unit\t{unit}\t{label}\t{group}\t{version}\t{size}\t{payload}\t{count}\t{digest}\t{required}\n")

	# The boot files, copied beside the units.
	for specification in arguments.boot:
		name, destination, source = specification.split("=", 2)
		copy = os.path.join(arguments.output, "boot", name)
		with open(source, "rb") as reader, open(copy, "wb") as writer:
			writer.write(reader.read())
		os.chmod(copy, 0o644)
		size, digest = file_digest(copy)
		records.append(f"boot\t{name}\t{destination}\t{size}\t{digest}\n")

	# The set-user-ID and set-group-ID files, and the firmware's licences.
	for path, unit, _program, kind, mode in sorted(manifest):
		if kind == "f" and mode & 0o6000:
			records.append(f"setid\t{unit}\t/{path}\t{mode:04o}\n")
	for path, unit, _program, kind, _mode in sorted(manifest):
		parts = path.split("/")
		if unit == "firmware" and len(parts) > 4 and parts[:3] == ["usr", "share", "licenses"]:
			records.append(f"license\t{unit}\t{parts[3]}\t/{path}\n")

	# The items: the fixed ones, then one under 'other' for each package unit they leave.
	named: set[str] = set()
	item_ids: set[str] = set()
	for identifier, parent, default, label, item_units in read_items(arguments.items):
		item_ids.add(identifier)
		if item_units == "-":
			item_units = ""
		for unit in item_units.split():
			if unit not in units:
				problems.add(f"the item {identifier} names a unit the root has not: {unit}")
			named.add(unit)
		records.append(f"item\t{identifier}\t{parent}\t{default}\t{label}\t{item_units or '-'}\n")
	if OTHER_ITEM not in item_ids:
		problems.add(f"the items have no {OTHER_ITEM}")
	for unit in sorted(units):
		if unit in GROUP_LABELS or unit in named:
			continue
		label = programs.get(unit_program.get(unit, ""), {}).get("label") or unit
		records.append(f"item\t{OTHER_ITEM}/{unit}\t{OTHER_ITEM}\toff\t{label}\t{unit}\n")
	problems.stop_if_any()

	index = os.path.join(arguments.output, "index")
	with open(index, "w", encoding="utf-8") as stream:
		stream.writelines(records)
	os.chmod(index, 0o644)
	print(f"installer-units: {len(units)} units")
	return 0


if __name__ == "__main__":
	sys.exit(main())
