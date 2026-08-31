#!/usr/bin/env python3
"""Parse the file-scope structure needed by the userland C-style audit."""

from dataclasses import dataclass
import pathlib
import re


IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CONTROL_WORDS = {"if", "for", "while", "switch", "sizeof", "_Alignof"}


@dataclass
class Function:
    """One file-scope function definition."""

    name: str
    start: int
    header_start: int
    open_brace: int
    close_brace: int
    is_static: bool
    is_inline: bool
    header: str
    body: str


@dataclass
class Prototype:
    """One file-scope static function declaration."""

    name: str
    start: int
    end: int
    text: str


def mask_c(text):
    """Mask comments, literals, and directives while preserving positions."""
    result = list(text)
    length = len(text)
    index = 0
    line_start = True

    def blank(start, end):
        for position in range(start, end):
            if result[position] != "\n":
                result[position] = " "

    while index < length:
        character = text[index]
        if line_start:
            probe = index
            while probe < length and text[probe] in " \t":
                probe += 1
            if probe < length and text[probe] == "#":
                end = probe
                while end < length:
                    newline = text.find("\n", end)
                    if newline < 0:
                        end = length
                        break
                    slash = newline - 1
                    while slash >= probe and text[slash] == "\r":
                        slash -= 1
                    end = newline + 1
                    if slash < probe or text[slash] != "\\":
                        break
                blank(index, end)
                index = end
                line_start = True
                continue

        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            if end < 0:
                end = length
            blank(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                end = length
            else:
                end += 2
            blank(index, end)
            index = end
            continue
        if character in {'"', "'"}:
            quote = character
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == quote:
                    end += 1
                    break
                end += 1
            blank(index, min(end, length))
            index = end
            continue

        line_start = character == "\n"
        if character not in " \t\r\n":
            line_start = False
        index += 1

    return "".join(result)


def matching(text, opening, left, right):
    """Return the matching delimiter position in already masked C text."""
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == left:
            depth += 1
        elif text[index] == right:
            depth -= 1
            if depth == 0:
                return index
    return -1


def function_name(header):
    """Return a defined function name from a file-scope header, if any."""
    candidates = []
    depth = 0
    for index, character in enumerate(header):
        if character == "(":
            if depth == 0:
                before = header[:index]
                matches = list(IDENTIFIER.finditer(before))
                if matches:
                    identifier = matches[-1]
                    name = identifier.group(0)
                    if header[identifier.end():index].strip():
                        depth += 1
                        continue
                    if name not in CONTROL_WORDS and name != "__attribute__":
                        close = matching(header, index, "(", ")")
                        if close >= 0:
                            candidates.append((name, close))
            depth += 1
        elif character == ")" and depth:
            depth -= 1

    for name, close in reversed(candidates):
        suffix = header[close + 1:].strip()
        if not suffix:
            return name
        if suffix.startswith(("__attribute__", "__asm__")):
            return name
        if re.fullmatch(r"[A-Z_][A-Z0-9_]*(?:\([^;{}]*\))?", suffix, re.S):
            return name
    return None


def attached_comment_start(text, header_start):
    """Include one immediately attached function comment in its block."""
    prefix = text[:header_start]
    whitespace_start = len(prefix.rstrip())
    trimmed = prefix[:whitespace_start]
    if not trimmed.endswith("*/"):
        return header_start
    opening = trimmed.rfind("/*")
    if opening < 0:
        return header_start
    leading = re.match(r"\A(?:/\*.*?\*/\n\n){3}", text, re.S)
    if leading is not None and opening < leading.end():
        return header_start
    between = prefix[whitespace_start:header_start]
    if between.count("\n") > 2:
        return header_start
    comment = trimmed[opening:]
    if "-*-" in comment or "SPDX-License-Identifier" in comment:
        return header_start
    return opening


def parse_functions(text):
    """Return all file-scope function definitions."""
    masked = mask_c(text)
    functions = []
    depth = 0
    segment_start = 0
    index = 0

    while index < len(masked):
        character = masked[index]
        if character == "{" and depth == 0:
            raw_header = masked[segment_start:index]
            leading = len(raw_header) - len(raw_header.lstrip())
            header_start = segment_start + leading
            header = raw_header[leading:]
            name = function_name(header)
            close = matching(masked, index, "{", "}")
            if close < 0:
                break
            if name is not None:
                tokens = IDENTIFIER.findall(header[:header.find(name)])
                start = attached_comment_start(text, header_start)
                functions.append(Function(
                    name=name,
                    start=start,
                    header_start=header_start,
                    open_brace=index,
                    close_brace=close,
                    is_static="static" in tokens,
                    is_inline="inline" in tokens or "__inline" in tokens,
                    header=text[header_start:index].strip(),
                    body=text[index:close + 1]))
            index = close + 1
            segment_start = index
            continue
        if character == ";" and depth == 0:
            segment_start = index + 1
        elif character == "{" :
            depth += 1
        elif character == "}" and depth:
            depth -= 1
        index += 1

    return functions


def parse_static_prototypes(text, first_function=None):
    """Return file-scope static function declarations."""
    masked = mask_c(text)
    limit = len(masked)
    prototypes = []
    depth = 0
    segment_start = 0

    for index, character in enumerate(masked[:limit]):
        if character in "{[":
            depth += 1
        elif character in "}]" and depth:
            depth -= 1
            if depth == 0 and character == "}":
                segment_start = index + 1
        elif character == ";" and depth == 0:
            raw = masked[segment_start:index]
            leading = len(raw) - len(raw.lstrip())
            start = segment_start + leading
            declaration = raw[leading:].strip()
            name = function_name(declaration)
            tokens = IDENTIFIER.findall(
                declaration[:declaration.find(name)] if name else "")
            if name is not None and "static" in tokens:
                prototypes.append(Prototype(
                    name=name,
                    start=start,
                    end=index + 1,
                    text=text[start:index + 1]))
            segment_start = index + 1

    return prototypes


def relative_sources(root):
    """Return the deterministic in-scope C/header inventory."""
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in {".c", ".h"})
