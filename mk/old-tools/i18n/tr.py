#!/usr/bin/env python3
"""tr.py: the catalogs of Keiland's translations (WS158, libkeiland's kl_tr_*).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The English text in the C source is the key: kl_tr("Open"),
kl_trc("verb", "Open"), kl_trn("{1} item", "{1} items", n).  A catalog is
LANGUAGE/DOMAIN.tr (in the tree userland/desktop/locale/, installed under
KEILAND_DATADIR/keiland/locale/), UTF-8, a line an entry, its fields
separated by a TAB:

    msg     ENGLISH     TEXT
    ctx     CONTEXT     ENGLISH     TEXT
    plural  SINGULAR    PLURAL      FORM [FORM ...]

with \\t, \\n and \\\\ for a TAB, a line's end and a backslash, and '#' for a
comment.  An entry whose text is empty is not translated yet (the library
leaves it out, so the English shows).

    tr.py extract SOURCE...                    the texts the source asks for, as an empty catalog
    tr.py update CATALOG SOURCE...             CATALOG with the source's texts: the translations kept, the new
                                               texts added empty, the ones the source lost kept as comments
    tr.py check CATALOG [SOURCE...]            the catalog's lines are well formed and keep the English's places
                                               ({1}...); with SOURCE, the texts not translated and those not used
                                               (--strict: an untranslated text fails too)

A SOURCE is a C file, a directory (its .c and .h files, recursively), or a
.keys file: the texts asked for through a variable (a data table's names,
a module that does not link the library), in a catalog's form without the
translation.  A call whose text is not a string literal is reported and
left out.
"""
from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The calls whose string literals are the keys, and how many of their first arguments are texts.
CALLS = {"kl_tr": 1, "kl_trc": 2, "kl_trn": 2}

# A place of kl_tr_format.
PLACE = re.compile(r"\{([1-9])\}")

# The comment above an entry that says where the source asks for it.
LOCATION = re.compile(r"^# \S+:\d+")


@dataclass
class Key:
	"""One text the source asks for, or a catalog has: its kind and its English (and context or plural)."""
	kind: str
	context: str | None
	english: str
	plural: str | None

	def ident(self) -> tuple:
		"""The key's identity."""
		return (self.kind, self.context, self.english, self.plural)


@dataclass
class Entry:
	"""A catalog's line: its key and its translation's forms (empty: not translated)."""
	key: Key
	forms: list[str]
	line: int
	where: list[str] = field(default_factory=list)


def escape(text: str) -> str:
	"""A field as a catalog writes it."""
	return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def unescape(text: str) -> str:
	"""A field as the library reads it (\\t, \\n, \\\\; another backslash stays)."""
	out = []
	index = 0
	while index < len(text):
		pair = text[index:index + 2]
		if pair == "\\t":
			out.append("\t")
			index += 2
		elif pair == "\\n":
			out.append("\n")
			index += 2
		elif pair == "\\\\":
			out.append("\\")
			index += 2
		else:
			out.append(text[index])
			index += 1
	return "".join(out)


# The C source.

def without_comments(text: str) -> str:
	"""The source with its comments blanked (strings and characters kept, line numbers kept)."""
	out = []
	index = 0
	length = len(text)
	while index < length:
		two = text[index:index + 2]
		character = text[index]
		if two == "/*":
			end = text.find("*/", index + 2)
			end = length if end < 0 else end + 2
			out.append(re.sub(r"[^\n]", " ", text[index:end]))
			index = end
		elif two == "//":
			end = text.find("\n", index)
			end = length if end < 0 else end
			out.append(" " * (end - index))
			index = end
		elif character in "\"'":
			end = index + 1
			while end < length and text[end] != character:
				end += 2 if text[end] == "\\" else 1
			out.append(text[index:end + 1])
			index = end + 1
		else:
			out.append(character)
			index += 1
	return "".join(out)


def c_string(body: str) -> str:
	"""A C string literal's bytes (its body between the quotes) as text, UTF-8."""
	data = bytearray()
	index = 0
	simple = {"n": 10, "t": 9, "r": 13, "\\": 92, "\"": 34, "'": 39, "a": 7, "b": 8, "f": 12, "v": 11, "?": 63}
	while index < len(body):
		character = body[index]
		if character != "\\":
			data += character.encode("utf-8")
			index += 1
			continue
		following = body[index + 1:index + 2]
		if following in simple:
			data.append(simple[following])
			index += 2
		elif following == "x":
			match = re.match(r"[0-9a-fA-F]+", body[index + 2:])
			data.append(int(match.group(0), 16) & 0xff)
			index += 2 + len(match.group(0))
		else:
			match = re.match(r"[0-7]{1,3}", body[index + 1:])
			data.append(int(match.group(0), 8) & 0xff)
			index += 1 + len(match.group(0))
	return data.decode("utf-8", "replace")


def literal_at(text: str, index: int) -> tuple[str | None, int]:
	"""The string literals that start at a place (adjacent ones joined), and where they end; None for another
	expression."""
	pieces = []
	while True:
		while index < len(text) and text[index].isspace():
			index += 1
		if index >= len(text) or text[index] != '"':
			break
		end = index + 1
		while end < len(text) and text[end] != '"':
			end += 2 if text[end] == "\\" else 1
		pieces.append(c_string(text[index + 1:end]))
		index = end + 1
	if not pieces:
		return None, index
	return "".join(pieces), index


def source_files(paths: list[str]) -> list[Path]:
	"""The C files of the paths given."""
	files = []
	for name in paths:
		path = Path(name)
		if path.is_dir():
			files += sorted(item for item in path.rglob("*") if item.suffix in (".c", ".h"))
		elif path.is_file():
			files.append(path)
		else:
			sys.exit(f"tr: no such source {name}")
	return files


def extract(paths: list[str], problems: list[str]) -> list[Entry]:
	"""The texts the source asks for, in the order met, each once with every place it is asked.  A .keys file
	among the paths lists texts the source asks for through a variable (a data table's names, a pure module's
	labels): lines in a catalog's form without the translation."""
	found: dict[tuple, Entry] = {}
	call = re.compile(r"\b(kl_trc|kl_trn|kl_tr)\s*\(")
	for name in paths:
		if not name.endswith(".keys"):
			continue
		for entry in read_catalog(Path(name), problems):
			entry.where = [f"{name}:{entry.line}"]
			entry.forms = []
			found.setdefault(entry.key.ident(), entry)
	for path in source_files([name for name in paths if not name.endswith(".keys")]):
		text = without_comments(path.read_text(encoding="utf-8", errors="replace"))
		for match in call.finditer(text):
			name = match.group(1)
			line = text.count("\n", 0, match.start()) + 1
			where = f"{path}:{line}"
			texts = []
			index = match.end()
			for number in range(CALLS[name]):
				literal, index = literal_at(text, index)
				if literal is None:
					break
				texts.append(literal)
				while index < len(text) and text[index].isspace():
					index += 1
				if number + 1 < CALLS[name]:
					if index >= len(text) or text[index] != ",":
						literal = None
						break
					index += 1
			if len(texts) != CALLS[name]:
				# The declaration and the definition of the calls themselves are not uses.
				if not re.match(r"\s*(const\s+)?char\b|\s*void\b", text[match.end():match.end() + 20]):
					problems.append(f"{where}: {name} without string literals, left out")
				continue
			if name == "kl_tr":
				key = Key("msg", None, texts[0], None)
			elif name == "kl_trc":
				key = Key("ctx", texts[0], texts[1], None)
			else:
				key = Key("plural", None, texts[0], texts[1])
			entry = found.setdefault(key.ident(), Entry(key, [], 0))
			entry.where.append(where)
	return list(found.values())


# The catalogs.

def read_catalog(path: Path, problems: list[str]) -> list[Entry]:
	"""A catalog's entries in its order (an untranslated one has no forms); a malformed line is a problem."""
	entries = []
	for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
		line = raw.rstrip("\r")
		if not line or line.startswith("#"):
			continue
		fields = [unescape(item) for item in line.split("\t")]
		kind = fields[0]
		where = f"{path}:{number}"
		if kind == "msg" and len(fields) in (2, 3):
			key, forms = Key("msg", None, fields[1], None), fields[2:]
		elif kind == "ctx" and len(fields) in (3, 4):
			key, forms = Key("ctx", fields[1], fields[2], None), fields[3:]
		elif kind == "plural" and len(fields) >= 3:
			key, forms = Key("plural", None, fields[1], fields[2]), fields[3:]
		else:
			problems.append(f"{where}: not an entry: {line[:60]!r}")
			continue
		if any(not text for text in [key.english, key.context or "-", key.plural or "-"]):
			problems.append(f"{where}: an empty key")
			continue
		forms = [text for text in forms if text]
		entries.append(Entry(key, forms, number))
	return entries


def places(text: str) -> set[str]:
	"""The places a text has."""
	return set(PLACE.findall(text))


def write_line(entry: Entry) -> str:
	"""A catalog's line for an entry (its translation empty when it has none)."""
	key = entry.key
	fields = [key.kind]
	if key.context is not None:
		fields.append(key.context)
	fields.append(key.english)
	if key.plural is not None:
		fields.append(key.plural)
	fields += entry.forms or [""]
	return "\t".join(escape(item) for item in fields)


def command_extract(sources: list[str]) -> int:
	"""Prints the source's texts as an empty catalog."""
	problems: list[str] = []
	for entry in extract(sources, problems):
		print(f"# {', '.join(entry.where[:3])}")
		print(write_line(entry))
	for problem in problems:
		print(f"tr: {problem}", file=sys.stderr)
	return 0


def command_update(catalog: str, sources: list[str]) -> int:
	"""Writes the catalog again with the source's texts, keeping its translations."""
	problems: list[str] = []
	path = Path(catalog)
	old = read_catalog(path, problems) if path.exists() else []
	# The catalog's own comment at its top stays; the places of the texts ("# FILE:LINE") are written anew.
	header = []
	if path.exists():
		for line in path.read_text(encoding="utf-8").splitlines():
			if not line.startswith("#") or LOCATION.match(line):
				break
			header.append(line)
	known = {entry.key.ident(): entry for entry in old}
	used = extract(sources, problems)
	lines = header or [f"# The {path.stem} domain's texts in {path.parent.name} (tools/i18n/tr.py update)."]
	added = 0
	for entry in used:
		previous = known.pop(entry.key.ident(), None)
		if previous is None:
			added += 1
		else:
			entry.forms = previous.forms
		lines.append(f"# {', '.join(entry.where[:3])}")
		lines.append(write_line(entry))
	if known:
		lines.append("")
		lines.append("# Not asked for by the source any more (kept for a text that comes back):")
		for entry in known.values():
			lines.append("# " + write_line(entry))
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text("\n".join(lines) + "\n", encoding="utf-8")
	for problem in problems:
		print(f"tr: {problem}", file=sys.stderr)
	print(f"tr: {path}: {len(used)} texts, {added} new, {len(known)} no longer used")
	return 1 if any("not an entry" in problem or "empty key" in problem for problem in problems) else 0


def command_check(catalog: str, sources: list[str], strict: bool) -> int:
	"""Checks a catalog (and against the source); 1 when something is wrong."""
	problems: list[str] = []
	path = Path(catalog)
	entries = read_catalog(path, problems)
	seen: dict[tuple, int] = {}
	for entry in entries:
		where = f"{path}:{entry.line}"
		if entry.key.ident() in seen:
			problems.append(f"{where}: also on line {seen[entry.key.ident()]} (the later one wins)")
		seen[entry.key.ident()] = entry.line
		english = places(entry.key.english) | places(entry.key.plural or "")
		for form in entry.forms:
			if places(form) != english:
				problems.append(f"{where}: the places {sorted(places(form))} are not the English's {sorted(english)}")
	untranslated = [entry for entry in entries if not entry.forms]
	notes = []
	if sources:
		used = {entry.key.ident(): entry for entry in extract(sources, notes)}
		for ident, entry in used.items():
			if ident not in seen:
				notes.append(f"not in the catalog: {write_line(entry).split(chr(9), 1)[1]!r} ({entry.where[0]})")
				if strict:
					problems.append(f"not in the catalog: {entry.key.english!r}")
		for entry in entries:
			if entry.key.ident() not in used:
				notes.append(f"{path}:{entry.line}: not asked for by the source")
	if strict:
		problems += [f"{path}:{entry.line}: not translated: {entry.key.english!r}" for entry in untranslated]
	for note in notes:
		print(f"tr: {note}")
	for problem in problems:
		print(f"tr: {path}: {problem}" if not problem.startswith(str(path)) else f"tr: {problem}")
	print(f"tr: {path}: {len(entries)} entries, {len(entries) - len(untranslated)} translated, {len(problems)} problems")
	return 1 if problems else 0


def main() -> int:
	"""Runs one command."""
	arguments = sys.argv[1:]
	strict = "--strict" in arguments
	arguments = [word for word in arguments if word != "--strict"]
	if len(arguments) >= 2 and arguments[0] == "extract":
		return command_extract(arguments[1:])
	if len(arguments) >= 3 and arguments[0] == "update":
		return command_update(arguments[1], arguments[2:])
	if len(arguments) >= 2 and arguments[0] == "check":
		return command_check(arguments[1], arguments[2:], strict)
	print(__doc__.split("\n\n")[3], file=sys.stderr)
	return 2


if __name__ == "__main__":
	sys.exit(main())
