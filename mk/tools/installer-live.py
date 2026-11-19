#!/usr/bin/env python3
"""installer-live.py: makes the root the installer's image runs from.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    installer-live.py --manifest FULL.manifest --tree ROOT --unit UNIT ... \\
        --payload DIRECTORY [--file DEST=SOURCE ...] [--mode DEST=MODE ...] --output LIVE

LIVE, which must not exist, is made of the files of ROOT (the installer's
whole root) that the manifest gives to the named units, with the manifest's
modes; the directories of ROOT above them and the ones ROOT has empty (/tmp,
/home), with ROOT's modes; DIRECTORY at /opt/installer (directories 0755,
files 0644); and each --file, which may replace a file of the units.  A
--file has 0755 when its source is executable and 0644 when not, unless a
--mode names it; a --mode may also name a directory above a --file (a
key's directory).

Then LIVE is read back: it must hold exactly those paths, and every file it
took from ROOT the same bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import sys

PAYLOAD_PATH = "opt/installer"


def read_manifest(path: str) -> list[tuple[str, str, str, int]]:
	"""The manifest's lines: path, unit, kind (f or l) and mode."""
	entries = []
	with open(path, encoding="utf-8") as stream:
		for raw in stream:
			path_, unit, _program, kind, mode = raw.rstrip("\n").split("\t")
			entries.append((path_, unit, kind, int(mode, 8)))
	return entries


def ancestors(path: str) -> list[str]:
	"""The directories above a path, the top first."""
	parts = path.split("/")[:-1]
	return ["/".join(parts[:count]) for count in range(1, len(parts) + 1)]


def digest(path: str) -> str:
	"""A file's SHA-256."""
	value = hashlib.sha256()
	with open(path, "rb") as stream:
		for block in iter(lambda: stream.read(1 << 20), b""):
			value.update(block)
	return value.hexdigest()


def make_directory(output: str, path: str, mode: int) -> None:
	"""Makes one directory of the live root with a mode, whatever the file creation mask."""
	full = os.path.join(output, path)
	if not os.path.isdir(full):
		os.mkdir(full)
	os.chmod(full, mode)


def main() -> int:
	"""Makes the live root and checks it, or fails naming what differs."""
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument("--manifest", required=True)
	parser.add_argument("--tree", required=True)
	parser.add_argument("--unit", action="append", default=[])
	parser.add_argument("--payload", required=True)
	parser.add_argument("--file", action="append", default=[])
	parser.add_argument("--mode", action="append", default=[])
	parser.add_argument("--output", required=True)
	arguments = parser.parse_args()
	problems: list[str] = []

	manifest = read_manifest(arguments.manifest)
	known_units = {unit for _path, unit, _kind, _mode in manifest}
	for unit in arguments.unit:
		if unit not in known_units:
			problems.append(f"no unit {unit} in the manifest")
	taken = [entry for entry in manifest if entry[1] in arguments.unit]

	# The files added, and the modes asked for.
	modes: dict[str, int] = {}
	for specification in arguments.mode:
		destination, _, mode = specification.partition("=")
		modes[destination.lstrip("/")] = int(mode, 8)
	added: dict[str, str] = {}
	for specification in arguments.file:
		destination, _, source = specification.partition("=")
		if not os.path.isfile(source):
			problems.append(f"--file names no file: {source}")
		added[destination.lstrip("/")] = source
	added_directories: set[str] = set()
	for path in added:
		added_directories.update(ancestors(path))
	for destination in modes:
		if destination not in added and destination not in added_directories:
			problems.append(f"--mode names a file no --file adds: /{destination}")

	# The root's directories: which hold anything, and their modes.
	tree_directories: dict[str, int] = {}
	occupied: set[str] = set()
	for top, directories, files in os.walk(arguments.tree):
		relative = os.path.relpath(top, arguments.tree)
		for name in directories:
			full = os.path.join(top, name)
			path = name if relative == "." else os.path.join(relative, name)
			if os.path.islink(full):
				occupied.update(ancestors(path))
				continue
			tree_directories[path] = stat.S_IMODE(os.lstat(full).st_mode)
		for name in files:
			if relative == "." and name == ".stamp":
				continue
			path = name if relative == "." else os.path.join(relative, name)
			occupied.update(ancestors(path))
	if problems:
		for problem in problems:
			print(f"installer-live: {problem}", file=sys.stderr)
		return 1

	# The directories: above what is taken, and the ones the root has empty.
	wanted: set[str] = set()
	for path in tree_directories:
		if path not in occupied:
			wanted.add(path)
			wanted.update(ancestors(path))
	for path, _unit, _kind, _mode in taken:
		wanted.update(ancestors(path))
	os.mkdir(arguments.output)
	os.chmod(arguments.output, 0o755)
	expected: dict[str, str] = {}
	for path in sorted(wanted):
		make_directory(arguments.output, path, tree_directories.get(path, 0o755))
		expected[path] = "d"

	# The units' files, with the manifest's modes.
	for path, _unit, kind, mode in taken:
		if path in added:
			continue
		source = os.path.join(arguments.tree, path)
		destination = os.path.join(arguments.output, path)
		if kind == "l":
			os.symlink(os.readlink(source), destination)
			expected[path] = "l"
			continue
		shutil.copyfile(source, destination)
		os.chmod(destination, mode)
		expected[path] = "f"

	# The payload at /opt/installer.
	for path in ancestors(PAYLOAD_PATH + "/x"):
		make_directory(arguments.output, path, 0o755)
		expected[path] = "d"
	for top, directories, files in os.walk(arguments.payload):
		relative = os.path.relpath(top, arguments.payload)
		base = PAYLOAD_PATH if relative == "." else os.path.join(PAYLOAD_PATH, relative)
		for name in sorted(directories):
			make_directory(arguments.output, os.path.join(base, name), 0o755)
			expected[os.path.join(base, name)] = "d"
		for name in sorted(files):
			destination = os.path.join(arguments.output, base, name)
			shutil.copyfile(os.path.join(top, name), destination)
			os.chmod(destination, 0o644)
			expected[os.path.join(base, name)] = "a"

	# The files added, which may replace one of the units'.
	for path, source in sorted(added.items()):
		for directory in ancestors(path):
			if directory not in expected:
				make_directory(arguments.output, directory, modes.get(directory, 0o755))
				expected[directory] = "d"
			elif directory in modes:
				os.chmod(os.path.join(arguments.output, directory), modes[directory])
		destination = os.path.join(arguments.output, path)
		shutil.copyfile(source, destination)
		executable = os.stat(source).st_mode & 0o111
		os.chmod(destination, modes.get(path, 0o755 if executable else 0o644))
		expected[path] = "a"

	# Read back: exactly the expected paths, the units' files with the root's bytes.
	found: set[str] = set()
	for top, directories, files in os.walk(arguments.output):
		relative = os.path.relpath(top, arguments.output)
		for name in directories + files:
			found.add(name if relative == "." else os.path.join(relative, name))
	for path in sorted(found - set(expected)):
		problems.append(f"not asked for: /{path}")
	for path in sorted(set(expected) - found):
		problems.append(f"missing: /{path}")
	for path, kind in expected.items():
		if kind == "f" and digest(os.path.join(arguments.output, path)) != digest(os.path.join(arguments.tree, path)):
			problems.append(f"differs from the whole root: /{path}")
	if problems:
		for problem in problems:
			print(f"installer-live: {problem}", file=sys.stderr)
		print(f"installer-live: {len(problems)} problems", file=sys.stderr)
		return 1
	print(f"installer-live: {len(found)} entries")
	return 0


if __name__ == "__main__":
	sys.exit(main())
