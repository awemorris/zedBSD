#!/usr/bin/env python3
"""installer-manifest.py: gives every file of the installer's whole root its unit.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    installer-manifest.py --owners FULL.owners --tree ROOT --desktop-include DIR --output MANIFEST

FULL.owners is what platform/amd64/installer-image.mk writes: a block B of the
root's package entries under the empty selection, a block R of the root's own
list (commands, accounts, data, runtime), then a block a program (P
name menu path class mode requires; C its command; D a data file; W the words
of the entries its selection alone gives).  ROOT is the staged root.  The
entries' words come in pairs, an option and DEST=VALUE: --file DEST=SOURCE,
--subtree DEST=DIRECTORY, --mode DEST=MODE and --link DEST=TARGET.

A file is claimed by the program whose entries or registration name it.  What
the empty selection names, the root's own files, what the root's own list names
and no program claims, and the development files the root copies from the
sysroot are the base's, but a header the desktop's
include directory has is the desktop's.  A unit is the program's menu: base,
desktop and firmware by those names, a package by its registered path with '-'
for '/' (network/curl is network-curl).  When two programs claim a path, the
base's claim wins, then the one the other requires; two others are an error.

MANIFEST has a line a file or link of ROOT: path, unit, program ('-' for none),
f or l, and the mode in octal.  The script fails, naming them, when a file has
no unit, when a program of a menu that is not installed (tests, X11) claims a
file, when a claimed --file is missing from ROOT, and when a set-user-ID or
set-group-ID file is not one the registration asked for.
"""
from __future__ import annotations

import argparse
import os
import stat
import sys

# The menus whose programs are units; another menu's files are not installed.
UNIT_MENUS = ("base", "desktop", "firmware")

# The root's own files, which the root's rule makes whatever is selected.
ROOT_FILES = ("lib/arch.id", "etc/zedbsd-root", "var/run")


class Program:
	"""A program's block: its registration and the entries its selection gives."""

	def __init__(self, fields: list[str]):
		self.name = fields[1]
		self.menu = fields[2] if len(fields) > 2 else ""
		self.path = fields[3] if len(fields) > 3 else ""
		self.mode = fields[5] if len(fields) > 5 else ""
		self.requires = fields[6].split() if len(fields) > 6 else []
		self.commands: list[str] = []
		self.data: list[str] = []
		self.words: list[str] = []

	def unit(self) -> str | None:
		"""The unit the program's files go to, or None for a menu that is not installed."""
		if self.menu in UNIT_MENUS:
			return self.menu
		if self.menu.startswith("packages/") and self.path:
			return self.path.replace("/", "-")
		return None


def read_owners(path: str) -> tuple[list[str], list[str], dict[str, Program]]:
	"""Reads the owners file: the empty selection's words, the root's own list's and the programs in order."""
	baseline: list[str] = []
	root_list: list[str] = []
	programs: dict[str, Program] = {}
	current: Program | None = None
	words = baseline
	with open(path, encoding="utf-8") as stream:
		for raw in stream:
			line = raw.rstrip("\n").lstrip(" ")
			if not line:
				continue
			fields = line.split("\t")
			kind = fields[0]
			if kind == "B":
				current = None
				words = baseline
			elif kind == "R":
				current = None
				words = root_list
			elif kind == "P":
				current = Program(fields)
				programs[current.name] = current
				words = current.words
			elif kind == "C" and current is not None:
				current.commands.append(fields[1])
			elif kind == "D" and current is not None:
				current.data.append(fields[1])
			elif kind == "W":
				words.append(fields[1])
			else:
				raise SystemExit(f"installer-manifest: {path}: a line not understood: {line!r}")
	return baseline, root_list, programs


def pairs(words: list[str]) -> list[tuple[str, str, str]]:
	"""The entries of a list of words: option, destination (no leading '/'), value."""
	if len(words) % 2 != 0:
		raise SystemExit("installer-manifest: an odd number of entry words")
	entries = []
	for at in range(0, len(words), 2):
		option, specification = words[at], words[at + 1]
		destination, _, value = specification.partition("=")
		entries.append((option, destination.lstrip("/"), value))
	return entries


def subtree_paths(destination: str, directory: str) -> list[str]:
	"""The paths a --subtree puts in the root: every file and link under its directory."""
	found = []
	for top, directories, files in os.walk(directory):
		relative = os.path.relpath(top, directory)
		for name in files + [d for d in directories if os.path.islink(os.path.join(top, d))]:
			inner = name if relative == "." else os.path.join(relative, name)
			found.append(os.path.join(destination, inner))
	return found


def requires_closure(programs: dict[str, Program], name: str) -> set[str]:
	"""The programs a program needs, through their registered paths."""
	by_path = {program.path: program.name for program in programs.values() if program.path}
	seen: set[str] = set()
	pending = [name]
	while pending:
		program = programs.get(pending.pop())
		if program is None:
			continue
		for requirement in program.requires:
			needed = by_path.get(requirement)
			if needed is not None and needed not in seen:
				seen.add(needed)
				pending.append(needed)
	return seen


def main() -> int:
	"""Writes the manifest, or fails naming what does not fit."""
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument("--owners", required=True)
	parser.add_argument("--tree", required=True)
	parser.add_argument("--desktop-include", required=True)
	parser.add_argument("--output", required=True)
	arguments = parser.parse_args()

	baseline, root_list, programs = read_owners(arguments.owners)
	baseline_entries = pairs(baseline)
	baseline_set = set(baseline_entries)
	problems: list[str] = []

	# Every claim: path -> set of claimants ('-' for the base's own), with what each asked of the mode.
	claims: dict[str, set[str]] = {}
	files_claimed: dict[str, str] = {}
	special_allowed: set[str] = set()

	def claim(path: str, owner: str) -> None:
		claims.setdefault(path, set()).add(owner)

	for option, destination, value in baseline_entries:
		if option in ("--file", "--link"):
			claim(destination, "-")
			if option == "--file":
				files_claimed[destination] = "-"
		elif option == "--subtree":
			for path in subtree_paths(destination, value):
				claim(path, "-")
		elif option == "--mode" and int(value, 8) & 0o6000:
			special_allowed.add(destination)
	for name in ROOT_FILES:
		claim(name, "-")

	for program in programs.values():
		for command in program.commands:
			claim(command.lstrip("/"), program.name)
			if program.mode and int(program.mode, 8) & 0o6000:
				special_allowed.add(command.lstrip("/"))
		for data in program.data:
			claim(data.lstrip("/"), program.name)
		for entry in pairs(program.words):
			if entry in baseline_set:
				continue
			option, destination, value = entry
			if option in ("--file", "--link"):
				claim(destination, program.name)
				if option == "--file":
					files_claimed[destination] = program.name
			elif option == "--subtree":
				for path in subtree_paths(destination, value):
					claim(path, program.name)
			elif option == "--mode" and int(value, 8) & 0o6000:
				special_allowed.add(destination)

	# What the root's own list names and no program claims is the base's.
	for option, destination, value in pairs(root_list):
		if option == "--file" and destination not in claims:
			claim(destination, "-")
			files_claimed.setdefault(destination, "-")
		elif option == "--mode" and int(value, 8) & 0o6000:
			special_allowed.add(destination)

	# The root's files, a unit each (the root rule's stamp is not one).
	lines = []
	for top, directories, files in os.walk(arguments.tree):
		directories.sort()
		names = sorted(files + [d for d in directories if os.path.islink(os.path.join(top, d))])
		for name in names:
			full = os.path.join(top, name)
			path = os.path.relpath(full, arguments.tree)
			if path == ".stamp":
				continue
			info = os.lstat(full)
			kind = "l" if stat.S_ISLNK(info.st_mode) else "f"
			mode = stat.S_IMODE(info.st_mode)
			owners = claims.get(path)
			owner = None
			if owners is None:
				# A development file the root copied from the sysroot.
				if path.startswith(("usr/include/", "usr/lib/")):
					inner = path[len("usr/include/"):]
					desktop = path.startswith("usr/include/") and os.path.lexists(os.path.join(arguments.desktop_include, inner))
					owner = "+desktop" if desktop else "-"
				else:
					problems.append(f"no unit: /{path}")
					continue
			elif "-" in owners:
				owner = "-"
			elif len(owners) == 1:
				owner = next(iter(owners))
			else:
				# The one the others need.
				needed = [o for o in owners if all(o == other or o in requires_closure(programs, other) for other in owners)]
				if len(needed) == 1:
					owner = needed[0]
				else:
					problems.append(f"claimed by {' '.join(sorted(owners))}: /{path}")
					continue
			if owner == "-":
				unit, program_name = "base", "-"
			elif owner == "+desktop":
				unit, program_name = "desktop", "-"
			else:
				unit, program_name = programs[owner].unit(), owner
				if unit is None:
					problems.append(f"of {owner} (menu {programs[owner].menu}, not installed): /{path}")
					continue
			if kind == "f" and mode & 0o6000 and path not in special_allowed:
				problems.append(f"set-user-ID or set-group-ID not asked for: /{path} {mode:04o}")
			lines.append(f"{path}\t{unit}\t{program_name}\t{kind}\t{mode:04o}\n")

	# A --file that the root does not have.
	for path, owner in sorted(files_claimed.items()):
		if not os.path.lexists(os.path.join(arguments.tree, path)):
			problems.append(f"named by {owner} but not in the root: /{path}")

	if problems:
		for problem in problems:
			print(f"installer-manifest: {problem}", file=sys.stderr)
		print(f"installer-manifest: {len(problems)} problems", file=sys.stderr)
		return 1
	with open(arguments.output + ".tmp", "w", encoding="utf-8") as stream:
		stream.writelines(lines)
	os.replace(arguments.output + ".tmp", arguments.output)
	print(f"installer-manifest: {len(lines)} files")
	return 0


if __name__ == "__main__":
	sys.exit(main())
