#!/usr/bin/env python3
"""ws046: counts the make features that configured Makefiles use.

  python3 plan/ws046/tests/make-features.py TREE...

Every file named Makefile (and GNUmakefile) under each tree is read,
continuation lines are joined, and directives, assignment kinds,
functions, automatic variables, special targets and rule kinds are
counted.  Prints one table per tree: feature, count, and the first place
it was seen.
"""

import re
import sys
from pathlib import Path

DIRECTIVES = ["include", "-include", "sinclude", "ifeq", "ifneq", "ifdef", "ifndef",
	      "else", "endif", "define", "endef", "export", "unexport", "vpath", "override",
	      "private", "undefine", "load"]
SPECIAL = [".PHONY", ".SUFFIXES", ".PRECIOUS", ".DEFAULT", ".SILENT", ".NOTPARALLEL",
	   ".INTERMEDIATE", ".SECONDARY", ".DELETE_ON_ERROR", ".EXPORT_ALL_VARIABLES",
	   ".ONESHELL", ".POSIX", ".MAKE", ".NOEXPORT", ".IGNORE", ".LOW_RESOLUTION_TIME",
	   ".SECONDEXPANSION", ".DEFAULT_GOAL", ".RECIPEPREFIX", ".EXTRA_PREREQS", ".WAIT"]
FUNCTIONS = ["subst", "patsubst", "strip", "findstring", "filter", "filter-out", "sort",
	     "word", "wordlist", "words", "firstword", "lastword", "dir", "notdir", "suffix",
	     "basename", "addsuffix", "addprefix", "join", "wildcard", "realpath", "abspath",
	     "error", "warning", "info", "shell", "origin", "flavor", "foreach", "if", "or",
	     "and", "call", "eval", "file", "value", "let", "intcmp"]


def logical_lines(text: str) -> list:
	"""Joins backslash-continued lines; returns (first line number, text)."""
	result = []
	pending = ""
	start = 0
	for number, line in enumerate(text.split("\n"), 1):
		if not pending:
			start = number
		if line.endswith("\\"):
			pending += line[:-1] + " "
			continue
		result.append((start, pending + line))
		pending = ""
	return result


def scan(path: Path, counts: dict, where: dict, label: str) -> None:
	"""Adds the features of one Makefile to the counts."""
	def add(feature: str, number: int) -> None:
		counts[feature] = counts.get(feature, 0) + 1
		where.setdefault(feature, "%s:%d" % (label, number))

	for number, line in logical_lines(path.read_text(errors="replace")):
		if line.startswith("#"):
			continue
		recipe = line.startswith("\t")
		body = line.strip()

		# Directives, at the start of a non-recipe line.
		if not recipe:
			first = body.split(" ", 1)[0].split("(", 1)[0]
			if first in DIRECTIVES:
				add("directive " + first, number)

		# Assignments (not in recipes): the first operator outside $(...).
		if not recipe:
			match = re.match(r"^(?:override\s+|export\s+)?([^:#=\s]+(?:\s+[^:#=\s]+)*)\s*(::=|:::=|:=|\+=|\?=|!=|=)", body)
			if match and not re.match(r"^[^=]*[^:]:[^=]", body.split("=", 1)[0] + "="):
				add("assign " + match.group(2), number)

		# Rules: target lines with a colon that is not part of an assignment.
		if not recipe and ":" in body and not re.search(r"(:=|::=|\+=|\?=|!=)", body.split(":", 1)[0] + body.split(":", 1)[1][:2]):
			head = body.split(";", 1)[0]
			left, _, right = head.partition(":")
			if "=" not in left and not left.startswith("$(") or left.startswith("$(") and ")" in left:
				if right.startswith(":"):
					add("rule double-colon", number)
				if "%" in left:
					add("rule pattern", number)
				if re.match(r"^\.[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)?$", left.strip()) and left.strip() not in SPECIAL:
					add("rule suffix " + left.strip(), number)
				if "|" in right:
					add("rule order-only prerequisite", number)
				if re.match(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*(\+=|:=|\?=|=)", right):
					add("rule target-specific variable", number)
				for special in SPECIAL:
					if left.strip() == special:
						add("special " + special, number)

		# Functions and automatic variables anywhere; $$ is a literal dollar for the shell.
		line = line.replace("$$", "")
		for name in re.findall(r"\$[({]([a-z-]+)[ \t]", line):
			if name in FUNCTIONS:
				add("function " + name, number)
		for automatic in re.findall(r"\$(?:[@<^+?*%|]|[({][@<^+?*%][DF][)}])", line):
			add("automatic " + automatic, number)
		if "$(MAKE)" in line or "${MAKE}" in line:
			add("recursive $(MAKE)", number)
		if "MAKEFLAGS" in line:
			add("variable MAKEFLAGS", number)
		if recipe:
			prefix = line[1:3]
			for character in prefix:
				if character in "@-+":
					add("recipe prefix " + character, number)
				else:
					break


def main() -> int:
	"""Prints the table for each tree."""
	for tree in sys.argv[1:]:
		root = Path(tree)
		counts = {}
		where = {}
		files = sorted(list(root.rglob("Makefile")) + list(root.rglob("GNUmakefile")))
		for path in files:
			scan(path, counts, where, str(path.relative_to(root)))
		print("## %s (%d Makefiles)" % (root.name, len(files)))
		for feature in sorted(counts):
			print("%-44s %6d  %s" % (feature, counts[feature], where[feature]))
		print()
	return 0


if __name__ == "__main__":
	sys.exit(main())
