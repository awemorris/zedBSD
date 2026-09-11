#!/usr/bin/env python3
"""Normalize userland control-flow braces and block-entry layout."""

import argparse
from dataclasses import dataclass
import importlib.util
import pathlib
import re
import sys


STRUCTURE_PATH = pathlib.Path(__file__).with_name("userland-c-structure.py")
STRUCTURE_SPEC = importlib.util.spec_from_file_location(
    "userland_c_structure_blocks", STRUCTURE_PATH)
STRUCTURE = importlib.util.module_from_spec(STRUCTURE_SPEC)
sys.modules[STRUCTURE_SPEC.name] = STRUCTURE
STRUCTURE_SPEC.loader.exec_module(STRUCTURE)


@dataclass(frozen=True)
class Wrapper:
    """One unbraced controlled statement that must become a block."""

    opening: int
    body_start: int
    body_end: int
    indentation: str


def skip_space(masked, position, limit=None):
    """Skip masked whitespace without crossing the supplied limit."""
    end = len(masked) if limit is None else limit
    while position < end and masked[position].isspace():
        position += 1
    return position


def word_at(masked, position, word):
    """Return true when one complete identifier is present at a position."""
    end = position + len(word)
    if masked[position:end] != word:
        return False
    before = masked[position - 1] if position > 0 else ""
    after = masked[end] if end < len(masked) else ""
    return not (before.isalnum() or before == "_") and not (
        after.isalnum() or after == "_")


def control_close(masked, position, word):
    """Return the closing parenthesis for a control statement."""
    opening = skip_space(masked, position + len(word))
    if opening >= len(masked) or masked[opening] != "(":
        return -1
    return STRUCTURE.matching(masked, opening, "(", ")")


def statement_end(masked, position, limit):
    """Return the end of one C statement using masked lexical structure."""
    position = skip_space(masked, position, limit)
    if position >= limit:
        return -1
    if masked[position] == "{":
        closing = STRUCTURE.matching(masked, position, "{", "}")
        return closing + 1 if 0 <= closing < limit else -1

    for word in ("if", "for", "while", "switch"):
        if not word_at(masked, position, word):
            continue
        closing = control_close(masked, position, word)
        if closing < 0:
            return -1
        body_end = statement_end(masked, closing + 1, limit)
        if body_end < 0:
            return -1
        if word != "if":
            return body_end
        probe = skip_space(masked, body_end, limit)
        if not word_at(masked, probe, "else"):
            return body_end
        return statement_end(masked, probe + len("else"), limit)

    if word_at(masked, position, "do"):
        body_end = statement_end(masked, position + len("do"), limit)
        probe = skip_space(masked, body_end, limit)
        if not word_at(masked, probe, "while"):
            return -1
        closing = control_close(masked, probe, "while")
        if closing < 0:
            return -1
        semicolon = skip_space(masked, closing + 1, limit)
        return semicolon + 1 if semicolon < limit and masked[semicolon] == ";" else -1

    round_depth = 0
    square_depth = 0
    brace_depth = 0
    cursor = position
    while cursor < limit:
        character = masked[cursor]
        if character == "(":
            round_depth += 1
        elif character == ")" and round_depth:
            round_depth -= 1
        elif character == "[":
            square_depth += 1
        elif character == "]" and square_depth:
            square_depth -= 1
        elif character == "{":
            brace_depth += 1
        elif character == "}" and brace_depth:
            brace_depth -= 1
        elif (character == ";" and round_depth == 0 and
              square_depth == 0 and brace_depth == 0):
            return cursor + 1
        cursor += 1
    return -1


def line_indentation(text, position):
    """Return the leading whitespace on the line containing a position."""
    start = text.rfind("\n", 0, position) + 1
    return re.match(r"[ \t]*", text[start:position]).group(0)


def body_information(masked, closing, limit):
    """Return the start, end, and braced state of one controlled body."""
    start = skip_space(masked, closing + 1, limit)
    if start >= limit or masked[start] == ";":
        return start, -1, False
    end = statement_end(masked, start, limit)
    return start, end, masked[start] == "{"


def symmetric_if_wrappers(text):
    """Find terminal if/else pairs with braces on only one body."""
    masked = STRUCTURE.mask_c(text)
    wrappers = set()
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\bif\b", region):
            position = base + match.start()
            closing = control_close(masked, position, "if")
            if closing < 0:
                continue
            then_start, then_end, then_braced = body_information(
                masked, closing, function.close_brace)
            if then_end < 0:
                continue
            else_position = skip_space(masked, then_end, function.close_brace)
            if not word_at(masked, else_position, "else"):
                continue
            else_start = skip_space(
                masked, else_position + len("else"), function.close_brace)
            if word_at(masked, else_start, "if"):
                continue
            else_end = statement_end(masked, else_start, function.close_brace)
            if else_end < 0:
                continue
            else_braced = masked[else_start] == "{"
            if then_braced == else_braced:
                continue
            indentation = line_indentation(text, position)
            if not then_braced:
                wrappers.add(Wrapper(
                    closing + 1, then_start, then_end, indentation))
            else:
                wrappers.add(Wrapper(
                    else_position + len("else"), else_start, else_end,
                    indentation))
    return sorted(wrappers, key=lambda item: (item.opening, item.body_end))


def multiline_if_wrappers(text):
    """Find unbraced if bodies whose statement spans physical lines."""
    masked = STRUCTURE.mask_c(text)
    wrappers = set()
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\bif\b", region):
            position = base + match.start()
            closing = control_close(masked, position, "if")
            if closing < 0:
                continue
            body_start, body_end, braced = body_information(
                masked, closing, function.close_brace)
            if body_end < 0 or braced:
                continue
            first_line = text.count("\n", 0, body_start)
            last_line = text.count("\n", 0, max(body_start, body_end - 1))
            if first_line == last_line:
                continue
            wrappers.add(Wrapper(
                closing + 1, body_start, body_end,
                line_indentation(text, position)))
    return sorted(wrappers, key=lambda item: (item.opening, item.body_end))


def loop_wrappers(text):
    """Find unbraced loop bodies that require explicit blocks."""
    masked = STRUCTURE.mask_c(text)
    wrappers = set()
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\b(for|while)\b", region):
            word = match.group(1)
            position = base + match.start()
            closing = control_close(masked, position, word)
            if closing < 0:
                continue
            body_start, body_end, braced = body_information(
                masked, closing, function.close_brace)
            if body_end < 0 or braced:
                continue
            first_word = re.match(
                r"[A-Za-z_][A-Za-z0-9_]*", masked[body_start:])
            contains_if = (word == "for" and first_word is not None and
                           first_word.group(0) == "if")
            first_line = text.count("\n", 0, body_start)
            last_line = text.count("\n", 0, max(body_start, body_end - 1))
            if not contains_if and first_line == last_line:
                continue
            wrappers.add(Wrapper(
                closing + 1, body_start, body_end,
                line_indentation(text, position)))
    return sorted(wrappers, key=lambda item: (item.opening, item.body_end))


def apply_wrappers(text, wrappers):
    """Apply non-overlapping opening replacements and grouped closing braces."""
    if not wrappers:
        return text
    replacements = []
    closings = {}
    for wrapper in wrappers:
        between = text[wrapper.opening:wrapper.body_start]
        if "\n" in between:
            replacements.append((wrapper.opening, wrapper.opening, " {"))
        else:
            replacements.append((
                wrapper.opening, wrapper.body_start,
                " {\n" + wrapper.indentation + "\t"))
        closings.setdefault(wrapper.body_end, set()).add(wrapper.indentation)
    for position, indentations in closings.items():
        value = "".join(
            "\n" + indentation + "}"
            for indentation in sorted(indentations, key=len, reverse=True))
        replacements.append((position, position, value))
    for start, end, replacement in sorted(replacements, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def braced_control_blocks(text):
    """Return braced decision and loop bodies with controller indentation."""
    masked = STRUCTURE.mask_c(text)
    blocks = []
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\bif\b", region):
            position = base + match.start()
            closing = control_close(masked, position, "if")
            if closing < 0:
                continue
            start = skip_space(masked, closing + 1, function.close_brace)
            if start >= function.close_brace or masked[start] != "{":
                continue
            end = STRUCTURE.matching(masked, start, "{", "}")
            if end >= 0:
                blocks.append((start, end, line_indentation(text, position)))
            then_end = statement_end(masked, start, function.close_brace)
            if then_end < 0:
                continue
            else_position = skip_space(masked, then_end, function.close_brace)
            if not word_at(masked, else_position, "else"):
                continue
            else_start = skip_space(
                masked, else_position + len("else"), function.close_brace)
            if word_at(masked, else_start, "if") or masked[else_start] != "{":
                continue
            else_end = STRUCTURE.matching(masked, else_start, "{", "}")
            if else_end >= 0:
                blocks.append((
                    else_start, else_end, line_indentation(text, position)))
        for match in re.finditer(r"\b(for|while)\b", region):
            word = match.group(1)
            position = base + match.start()
            closing = control_close(masked, position, word)
            if closing < 0:
                continue
            start = skip_space(masked, closing + 1, function.close_brace)
            if start >= function.close_brace or masked[start] != "{":
                continue
            end = STRUCTURE.matching(masked, start, "{", "}")
            if end >= 0:
                blocks.append((start, end, line_indentation(text, position)))
    return blocks


def deindent_overindented_control_blocks(text):
    """Remove excess indentation from immediate control-block statements."""
    masked = STRUCTURE.mask_c(text)
    candidates = []
    for opening, closing, indentation in braced_control_blocks(text):
        statement_start = skip_space(masked, opening + 1, closing)
        while statement_start < closing:
            statement_stop = statement_end(masked, statement_start, closing)
            if statement_stop < 0:
                break
            line_start = text.rfind("\n", 0, statement_start) + 1
            actual = text[line_start:statement_start]
            expected = indentation + "\t"
            if actual.startswith(expected + "\t"):
                excess = 0
                remainder = actual[len(expected):]
                while remainder.startswith("\t"):
                    excess += 1
                    remainder = remainder[1:]
                if excess:
                    candidates.append((
                        statement_start, statement_stop, indentation, excess))
            statement_start = skip_space(masked, statement_stop, closing)

    selected = []
    for candidate in sorted(candidates):
        if any(start <= candidate[0] < end for start, end, _, _ in selected):
            continue
        selected.append(candidate)
    if not selected:
        return text, 0

    lines = text.splitlines(keepends=True)
    for start, end, indentation, excess in reversed(selected):
        first_line = text.count("\n", 0, start)
        last_line = text.count("\n", 0, max(start, end - 1))
        for index in range(first_line, last_line + 1):
            line = lines[index]
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            for _ in range(excess):
                prefix = indentation + "\t"
                if not line.startswith(prefix):
                    break
                line = indentation + line[len(indentation) + 1:]
            lines[index] = line
    return "".join(lines), len(selected)


def remove_block_entry_blank_lines(text):
    """Remove empty physical lines immediately following block openings."""
    masked = STRUCTURE.mask_c(text)
    replacements = []
    count = 0
    openings = set()
    for function in STRUCTURE.parse_functions(text):
        openings.add(function.open_brace)
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\b(if|for|while|switch)\b", region):
            word = match.group(1)
            position = base + match.start()
            closing = control_close(masked, position, word)
            if closing < 0:
                continue
            opening = skip_space(masked, closing + 1, function.close_brace)
            if opening < function.close_brace and masked[opening] == "{":
                openings.add(opening)
        for match in re.finditer(r"\b(?:else|do)\b", region):
            position = base + match.end()
            opening = skip_space(masked, position, function.close_brace)
            if opening < function.close_brace and masked[opening] == "{":
                openings.add(opening)

    for opening in sorted(openings):
        function_end = len(text)
        newline = text.find("\n", opening, function_end)
        if newline < 0:
            continue
        cursor = newline + 1
        blank_end = cursor
        while blank_end < function_end:
            next_newline = text.find("\n", blank_end, function_end)
            if next_newline < 0 or text[blank_end:next_newline].strip():
                break
            blank_end = next_newline + 1
        if blank_end > cursor:
            replacements.append((cursor, blank_end, ""))
            count += text[cursor:blank_end].count("\n")
    for start, end, replacement in sorted(set(replacements), reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, count


def refactor(text):
    """Apply all deterministic control-block layout transformations."""
    counts = [0, 0, 0, 0]
    for _ in range(8):
        wrappers = sorted(set(
            symmetric_if_wrappers(text) + multiline_if_wrappers(text)),
            key=lambda item: (item.opening, item.body_end))
        if not wrappers:
            break
        text = apply_wrappers(text, wrappers)
        counts[0] += len(wrappers)
    for _ in range(8):
        wrappers = loop_wrappers(text)
        if not wrappers:
            break
        text = apply_wrappers(text, wrappers)
        counts[1] += len(wrappers)
    text, count = deindent_overindented_control_blocks(text)
    counts[2] += count
    text, count = remove_block_entry_blank_lines(text)
    counts[3] += count
    text = re.sub(r"}\n([ \t]*)else\b", r"} else", text)
    return text, counts


def main():
    """Apply or dry-run the whole-userland control-block transformer."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expected-c", type=int, default=214)
    arguments = parser.parse_args()
    paths = sorted(pathlib.Path(arguments.root).rglob("*.c"))
    if len(paths) != arguments.expected_c:
        raise SystemExit(
            f"C inventory is {len(paths)}, expected {arguments.expected_c}")

    totals = [0, 0, 0, 0]
    changed = 0
    for path in paths:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        migrated, counts = refactor(original)
        totals = [left + right for left, right in zip(totals, counts)]
        if migrated != original:
            changed += 1
            if arguments.apply:
                path.write_text(
                    migrated, encoding="utf-8", errors="surrogateescape",
                    newline="\n")
    action = "changed" if arguments.apply else "would change"
    print(
        f"USERLAND-CONTROL-BLOCKS: {action} {changed} files; "
        f"braced {totals[0]} if branches, "
        f"braced {totals[1]} loop bodies, "
        f"deindented {totals[2]} control-block statements, "
        f"removed {totals[3]} block-entry blank lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
