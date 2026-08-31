#!/usr/bin/env python3
"""Add required one-line intent comments before userland loops and switches."""

import argparse
import importlib.util
import pathlib
import re
import sys


STRUCTURE_PATH = pathlib.Path(__file__).with_name("userland-c-structure.py")
STRUCTURE_SPEC = importlib.util.spec_from_file_location(
    "userland_c_structure", STRUCTURE_PATH)
STRUCTURE = importlib.util.module_from_spec(STRUCTURE_SPEC)
sys.modules[STRUCTURE_SPEC.name] = STRUCTURE
STRUCTURE_SPEC.loader.exec_module(STRUCTURE)


def enclosing_parentheses(masked, keyword_end):
    """Return the control expression following one keyword."""
    opening = keyword_end
    while opening < len(masked) and masked[opening].isspace():
        opening += 1
    if opening >= len(masked) or masked[opening] != "(":
        return ""
    closing = STRUCTURE.matching(masked, opening, "(", ")")
    if closing < 0:
        return ""
    return " ".join(masked[opening + 1:closing].split())


def purpose(kind, expression):
    """Generate a bounded purpose sentence from a control expression."""
    lower = expression.lower()
    if kind == "switch":
        if "option" in lower or "opt" in lower:
            return "Dispatch the selected command-line option."
        if "token" in lower or "type" in lower or "kind" in lower:
            return "Dispatch the selected syntax or record type."
        if "signal" in lower:
            return "Dispatch the selected signal case."
        if "state" in lower or "status" in lower:
            return "Dispatch the current operation state."
        return "Dispatch the selected operation case."

    if "getopt" in lower:
        return "Parse each command-line option."
    if "argc" in lower or "optind" in lower or "argv" in lower:
        return "Process each remaining command-line operand."
    if any(value in lower for value in ("fgetc", "getc", "read(", "fgets")):
        return "Process input until it is exhausted."
    if "readdir" in lower:
        return "Process each directory entry."
    if any(value in lower for value in ("->next", ".next", " next")):
        return "Process each linked entry."
    if any(value in lower for value in ("length", "count", "index", "size")):
        return "Process each remaining element."
    if not expression or expression in {";;", "1", "true"}:
        return "Continue until the operation reaches a terminal state."
    if kind == "for":
        return "Process each element required by the operation."
    return "Continue while the operation condition remains true."


def preceding_one_line_comment(text, line_start):
    """Return true when the immediately preceding line is a block comment."""
    if line_start == 0:
        return False
    previous_end = line_start - 1
    previous_start = text.rfind("\n", 0, previous_end) + 1
    previous = text[previous_start:previous_end].strip()
    return bool(re.fullmatch(r"/\*[^\n]*\*/", previous))


def do_while_tail(masked, keyword_start):
    """Recognize the while clause terminating a do statement."""
    prefix = masked[:keyword_start].rstrip()
    return prefix.endswith("}")


def multiline_comment_openings(text):
    """Return malformed block-comment openings outside literals."""
    openings = []
    position = 0
    while position < len(text):
        if text.startswith("//", position):
            newline = text.find("\n", position + 2)
            position = len(text) if newline < 0 else newline + 1
            continue
        if text.startswith("/*", position):
            closing = text.find("*/", position + 2)
            if closing < 0:
                closing = len(text)
            newline = text.find("\n", position + 2, closing)
            if newline >= 0:
                prose = text[position + 2:newline].strip()
                if prose:
                    openings.append((position, newline + 1, prose))
            position = closing + 2
            continue
        if text[position] in {'"', "'"}:
            quote = text[position]
            position += 1
            while position < len(text):
                if text[position] == "\\":
                    position += 2
                    continue
                if text[position] == quote:
                    position += 1
                    break
                position += 1
            continue
        position += 1
    return openings


def refactor(text):
    """Insert missing intent comments and return the new text and count."""
    masked = STRUCTURE.mask_c(text)
    additions = []
    replacements = []
    comment_openings = []

    for start, end, prose in multiline_comment_openings(text):
        comment_openings.append(
            (start, end, "/*\n * " + prose + "\n"))

    offset = 0
    for masked_line, source_line in zip(
            masked.splitlines(keepends=True), text.splitlines(keepends=True)):
        label = re.match(r"^(\s*)(?:case\b|default\b)", masked_line)
        if label is not None:
            colon = masked_line.find(":", label.end())
            if colon >= 0 and masked_line[colon + 1:].strip():
                ending = "\n" if source_line.endswith("\n") else ""
                statement = source_line[colon + 1:].strip()
                replacement = (
                    source_line[:colon + 1].rstrip() + "\n" +
                    label.group(1) + "\t" + statement + ending)
                replacements.append((offset, offset + len(source_line), replacement))
        offset += len(source_line)

    pattern = re.compile(r"\b(for|while|switch)\b")
    for match in pattern.finditer(masked):
        kind = match.group(1)
        if kind == "while" and do_while_tail(masked, match.start()):
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        before_keyword = text[line_start:match.start()]
        if before_keyword.strip():
            continue
        if preceding_one_line_comment(text, line_start):
            continue
        expression = enclosing_parentheses(masked, match.end())
        indentation = before_keyword
        comment = indentation + "/* " + purpose(kind, expression) + " */\n"
        additions.append((line_start, comment))

    operations = replacements + comment_openings + [
        (position, position, addition) for position, addition in additions]
    for start, end, replacement in sorted(operations, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, len(additions), len(replacements), len(comment_openings)


def main():
    """Apply or dry-run the deterministic comment insertion."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expected-c", type=int, default=214)
    arguments = parser.parse_args()

    paths = [path for path in STRUCTURE.relative_sources(pathlib.Path(arguments.root))
             if path.suffix == ".c"]
    if len(paths) != arguments.expected_c:
        raise SystemExit(
            f"C inventory is {len(paths)}, expected {arguments.expected_c}")

    changed = 0
    additions = 0
    labels = 0
    comments = 0
    for path in paths:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        migrated, count, label_count, comment_count = refactor(original)
        additions += count
        labels += label_count
        comments += comment_count
        if migrated != original:
            changed += 1
            if arguments.apply:
                path.write_text(
                    migrated, encoding="utf-8", errors="surrogateescape",
                    newline="\n")

    action = "changed" if arguments.apply else "would change"
    print(f"USERLAND-C-STYLE-CONTROL: {action} {changed} files, "
          f"added {additions} comments, split {labels} case labels, "
          f"normalized {comments} multi-line comment openings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
