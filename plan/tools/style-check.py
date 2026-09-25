#!/usr/bin/env python3
"""Checks the rules of plan/coding-style.md that a program can check.

  python3 plan/tools/style-check.py FILE... [--rule NAME]... [--summary]

Each violation is printed as FILE:LINE: RULE: text.  Exits 1 when there is
any.  The rules (with the section of coding-style.md):

  call-in-condition   6   a function called in the condition of if, while
                          or the middle of for (sizeof is not a call)
  blank-after-brace   5   a statement right after the } that ends a block
  paragraph-comment   5   a paragraph inside a function that does not start
                          with a comment after its blank line (a lone break
                          or continue that ends a case or a turn is not a
                          paragraph of its own)
  nested-declaration  4   a declaration inside a nested block, or in a for
  conditional         6   the conditional operator (listed; a short
                          symmetric choice may stay)
  goto               14   any goto
  forward-declaration 3   a static function without a forward declaration
  comment-form       10   a multi-line comment that starts or ends with
                          prose on its /* or */ line
  name                4   function_result, and _local names on variables
                          or with a number (rollback_local1)
  multi-line-body     8   the one statement an if, else, for or while
                          controls spans lines without braces

What it cannot judge (whether a comment says something, names, the shape
of functions) needs reading.
"""

import argparse
import re
import sys

KEYWORDS = {
	"if", "while", "for", "switch", "return", "sizeof", "defined",
	"__attribute__", "__typeof__", "typeof", "_Alignof", "alignof",
}

DECLARATION = re.compile(
	r"^\s+(const\s+|static\s+|volatile\s+|unsigned\s+|signed\s+)*"
	r"(struct\s+\w+|union\s+\w+|enum\s+\w+|int|char|long|short|double|float|"
	r"size_t|ssize_t|off_t|pid_t|FILE|regex_t|regmatch_t|va_list|\w+_t)"
	r"(\s+|\s*\*+\s*)\**\w+(\s*\[[^\]]*\])?(\s*=.*)?;\s*$")


def strip_line(line: str, in_comment: bool) -> tuple[str, bool]:
	"""Blanks out comments, strings and character constants of a line."""
	out = []
	index = 0
	while index < len(line):
		if in_comment:
			end = line.find("*/", index)
			if end < 0:
				return "".join(out), True
			index = end + 2
			in_comment = False
			out.append(" ")
			continue
		character = line[index]
		if line.startswith("/*", index):
			in_comment = True
			index += 2
			continue
		if line.startswith("//", index):
			break
		if character in "\"'":
			quote = character
			index += 1
			while index < len(line) and line[index] != quote:
				if line[index] == "\\":
					index += 1
				index += 1
			index += 1
			out.append(quote + quote)
			continue
		out.append(character)
		index += 1
	return "".join(out), in_comment


def matching_paren(text: str, start: int) -> int:
	"""Returns the index of the ) that closes the ( at start, or -1."""
	depth = 0
	for index in range(start, len(text)):
		if text[index] == "(":
			depth += 1
		elif text[index] == ")":
			depth -= 1
			if depth == 0:
				return index
	return -1


def calls_in(text: str) -> list[str]:
	"""Returns the names called in an expression."""
	names = []
	for match in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", text):
		name = match.group(1)
		if name in KEYWORDS:
			continue
		names.append(name)
	return names


def check_file(path: str) -> list[tuple[int, str, str]]:
	"""Returns the violations of a file as (line, rule, text)."""
	with open(path, encoding="utf-8", errors="replace") as stream:
		raw = stream.read().split("\n")
	stripped = []
	in_comment = False
	for line in raw:
		text, in_comment = strip_line(line, in_comment)
		stripped.append(text)

	problems = []
	depth = 0
	depths = []
	# Whether a line is inside a struct, union or enum body, where a
	# declaration is a member and not a local variable.
	braces = []
	in_type = []
	for text in stripped:
		depths.append(depth)
		depth += text.count("{") - text.count("}")
		in_type.append("type" in braces)
		opens_type = re.search(r"\b(struct|union|enum)\b[^;()]*\{", text) is not None
		for character in text:
			if character == "{":
				braces.append("type" if opens_type else "code")
			elif character == "}" and braces:
				braces.pop()

	# Conditions: gather the whole parenthesized condition, over lines.
	joined = "\n".join(stripped)
	offsets = [0]
	for line in stripped:
		offsets.append(offsets[-1] + len(line) + 1)
	for match in re.finditer(r"(?m)^[ \t}]*(?:else\s+)?\b(if|while|for)\s*\(", joined):
		open_index = match.end() - 1
		close_index = matching_paren(joined, open_index)
		if close_index < 0:
			continue
		condition = joined[open_index + 1:close_index]
		if match.group(1) == "for":
			parts = condition.split(";")
			if len(parts) == 3:
				condition = parts[1]
		names = calls_in(condition)
		if names:
			line_number = joined.count("\n", 0, match.start()) + 1
			problems.append((line_number, "call-in-condition", ", ".join(names)))

	# Line rules.
	for index, line in enumerate(raw):
		number = index + 1
		text = stripped[index]
		following = raw[index + 1] if index + 1 < len(raw) else ""

		if re.match(r"^\t+\}\s*$", line):
			nxt = following.strip()
			if (nxt and not nxt.startswith("}") and
			    not re.match(r"^(else\b|case\b|default\s*:|while\s*\(|#)", nxt) and
			    not re.match(r"^\w+:\s*$", nxt)):
				problems.append((number + 1, "blank-after-brace", nxt))

		if line.strip() == "" and depths[index] >= 1 and index + 1 < len(raw):
			nxt = following.strip()
			if (nxt and not nxt.startswith("/*") and not nxt.startswith("}") and
			    not nxt.startswith("#") and
			    not re.match(r"^(case\b|default\s*:|UNUSED_PARAMETER|assert\s*\(|break;|continue;)", nxt) and
			    not re.match(r"^\w+:\s*$", nxt) and
			    not DECLARATION.match(following)):
				problems.append((number + 1, "paragraph-comment", nxt))

		if depths[index] >= 2 and not in_type[index] and DECLARATION.match(text) and not text.strip().startswith("return"):
			problems.append((number, "nested-declaration", line.strip()))
		if re.search(r"\bfor\s*\(\s*(const\s+)?(int|size_t|unsigned|long|char|struct)\b", text):
			problems.append((number, "nested-declaration", line.strip()))

		if re.search(r"\?[^?]*:", text) and "?" in text:
			problems.append((number, "conditional", line.strip()))

		control = re.match(r"^\s*(\}\s*)?(else\s+)?(if|for|while)\b.*\)\s*$|^\s*(\}\s*)?else\s*$", text)
		if control and index + 1 < len(stripped):
			body = stripped[index + 1].rstrip()
			paren_depth = text.count("(") - text.count(")")
			if paren_depth == 0 and body.strip() and not body.strip().startswith("{") and not body.endswith(";"):
				problems.append((number + 1, "multi-line-body", body.strip()))

		if re.search(r"\bgoto\b", text):
			problems.append((number, "goto", line.strip()))

		# A mechanical _local suffix: numbered (rollback_local1), or on a
		# declared variable.  A function named for the local builtin is not one.
		declared_local = DECLARATION.match(text) and re.search(r"\w_local\b", text)
		if re.search(r"\bfunction_result\b|\w_local\d+\b", text) or declared_local:
			problems.append((number, "name", line.strip()))

		stripped_line = line.strip()
		if "/*" in line and "*/" not in line[line.find("/*"):]:
			if stripped_line != "/*":
				problems.append((number, "comment-form", stripped_line))
		elif ("*/" in line and "/*" not in line and stripped_line != "*/"):
			problems.append((number, "comment-form", stripped_line))

	# Static functions and their forward declarations.
	declared = set()
	for line in stripped:
		match = re.match(r"^static\b.*?\b(\w+)\s*\([^;]*\)\s*(__attribute__.*)?;\s*$", line)
		if match:
			declared.add(match.group(1))
	for index, line in enumerate(stripped):
		match = re.match(r"^(\w+)\($", line)
		if not match or index == 0:
			continue
		if stripped[index - 1].startswith("static") and "inline" not in stripped[index - 1]:
			if match.group(1) not in declared:
				problems.append((index + 1, "forward-declaration", match.group(1)))

	return sorted(problems)


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	parser.add_argument("files", nargs="+")
	parser.add_argument("--rule", action="append", help="report only these rules")
	parser.add_argument("--summary", action="store_true", help="counts per file and rule")
	arguments = parser.parse_args()

	total = 0
	for path in arguments.files:
		problems = check_file(path)
		if arguments.rule:
			problems = [p for p in problems if p[1] in arguments.rule]
		total += len(problems)
		if arguments.summary:
			counts = {}
			for _, rule, _ in problems:
				counts[rule] = counts.get(rule, 0) + 1
			if counts:
				print(path, " ".join(f"{rule}={count}" for rule, count in sorted(counts.items())))
			continue
		for number, rule, text in problems:
			print(f"{path}:{number}: {rule}: {text}")
	if arguments.summary:
		print("total", total)
	return 1 if total else 0


if __name__ == "__main__":
	sys.exit(main())
