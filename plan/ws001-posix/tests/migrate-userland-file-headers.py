#!/usr/bin/env python3
"""Migrate only the leading comments of userland C-family source files."""

import argparse
import hashlib
import pathlib
import re
import textwrap


CANONICAL = """/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

"""
MODELINE = "/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */"
SUFFIXES = {".c", ".h", ".S"}


def source_paths(root):
    """Return every in-scope source in deterministic pathname order."""
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in SUFFIXES)


def split_leading_comments(text):
    """Separate initial C comments from the implementation body."""
    position = 0
    comments = []
    had_bom = text.startswith("\ufeff")
    if had_bom:
        text = text[1:]

    while True:
        match = re.match(r"[ \t\r\n]*/\*(.*?)\*/", text[position:], re.S)
        if match is None:
            break
        comments.append(match.group(1))
        position += match.end()

    body = text[position:]
    body = body.lstrip("\r\n")
    return comments, body, had_bom


def explanation_from_comments(comments):
    """Recover useful prose while dropping legacy header metadata."""
    paragraphs = []
    current = []

    for comment in comments:
        if "-*-" in comment:
            continue
        for source_line in comment.splitlines():
            line = re.sub(r"^\s*\*?\s?", "", source_line).strip()
            line = re.split(
                r"SPDX-License-Identifier:", line, maxsplit=1)[0].strip()
            line = re.split(
                r"Copyright \(C\)", line, maxsplit=1)[0].strip()
            if line in {"", "zedBSD", "Zlib"}:
                if current:
                    paragraphs.append(" ".join(current))
                    current = []
                continue
            current.append(line)
        if current:
            paragraphs.append(" ".join(current))
            current = []

    return paragraphs


def humanize(value):
    """Turn a pathname component into readable prose."""
    return value.replace("_", " ").replace("-", " ")


def generated_explanation(relative):
    """Create a role-specific explanation for a file without leading prose."""
    parts = relative.parts
    stem = humanize(relative.stem)

    if relative.suffix == ".S":
        if parts[-2] == "libc" and relative.stem.startswith("syscall-"):
            architecture = humanize(relative.stem.removeprefix("syscall-"))
            return [f"Implements the {architecture} user-space system call entry point."]
        if parts[-2] == "rtld" and relative.stem.startswith("entry-"):
            architecture = humanize(relative.stem.removeprefix("entry-"))
            return [f"Implements the {architecture} runtime-linker entry point."]
        if parts[-2] == "rtld" and relative.stem.startswith("tlsdesc-"):
            architecture = humanize(relative.stem.removeprefix("tlsdesc-"))
            return [f"Implements the {architecture} runtime-linker TLSDESC resolver."]
        return [f"Implements the zedBSD {stem} assembly component."]

    if "tests" in parts:
        if relative.suffix == ".h":
            return [f"Declares support for the {stem} userland test."]
        return [f"Exercises the zedBSD {stem} userland behavior."]

    if relative.name == "main.c" and len(parts) >= 3:
        command = humanize(parts[-2])
        if parts[1] == "X11":
            return [f"Implements the zedBSD {command} X11 program."]
        return [f"Implements the zedBSD {command} userland command."]

    if relative.suffix == ".h":
        if "common" in parts:
            return [f"Declares shared userland {stem} support."]
        if "libc" in parts:
            return [f"Declares the zedBSD C library {stem} interface."]
        if parts[1] == "X11":
            return [f"Declares the zedBSD X11 {stem} interface."]
        return [f"Declares the zedBSD userland {stem} interface."]

    if "common" in parts:
        return [f"Implements shared userland {stem} support."]
    if "libc" in parts:
        return [f"Implements the zedBSD C library {stem} support."]
    if "service" in parts:
        return [f"Implements shared userland service {stem} support."]
    if parts[1] == "X11":
        return [f"Implements the zedBSD X11 {stem} component."]
    if parts[1] == "packages":
        return [f"Implements the zedBSD package {stem} component."]
    if parts[1] == "comp":
        return [f"Implements the zedBSD compiler {stem} component."]
    return [f"Implements the zedBSD userland {stem} component."]


def format_explanation(paragraphs):
    """Render explanation prose as the required separate comment block."""
    lines = ["/*"]
    for index, paragraph in enumerate(paragraphs):
        if index != 0:
            lines.append(" *")
        wrapped = textwrap.wrap(paragraph, width=75,
                                break_long_words=False,
                                break_on_hyphens=False)
        if not wrapped:
            wrapped = [paragraph]
        lines.extend(" * " + line for line in wrapped)
    lines.append(" */")
    return "\n".join(lines) + "\n\n"


def digest(text):
    """Return a stable digest for an implementation body."""
    return hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest()


def migrate(path, userland_root, apply):
    """Build and optionally install one canonical leading header."""
    original = path.read_bytes().decode("utf-8", "surrogateescape")
    comments, body, had_bom = split_leading_comments(original)
    paragraphs = explanation_from_comments(comments)
    source = "retained"
    relative = path.relative_to(userland_root.parent)
    if not paragraphs:
        paragraphs = generated_explanation(relative)
        source = "generated"

    had_modeline = any("-*-" in comment for comment in comments)
    header = MODELINE + "\n\n" + CANONICAL + format_explanation(paragraphs)
    migrated = header + body

    _, migrated_body, _ = split_leading_comments(migrated)
    if migrated_body != body:
        raise RuntimeError(f"body changed while migrating {relative}")
    if apply and migrated != original:
        path.write_bytes(migrated.encode("utf-8", "surrogateescape"))

    description = " ".join(paragraphs).replace("\t", " ")
    return relative, source, had_bom, digest(body), description


def main():
    """Migrate the selected tree and emit its review inventory."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--inventory")
    parser.add_argument("--expected-count", type=int, default=269)
    arguments = parser.parse_args()

    root = pathlib.Path(arguments.root)
    paths = source_paths(root)
    if len(paths) != arguments.expected_count:
        raise SystemExit(
            f"source count is {len(paths)}, expected {arguments.expected_count}")

    rows = [migrate(path, root, arguments.apply) for path in paths]
    if arguments.inventory:
        inventory = pathlib.Path(arguments.inventory)
        with inventory.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write("path\texplanation-source\thad-bom\tbody-sha256\texplanation\n")
            for relative, source, had_bom, body_hash, description in rows:
                stream.write(
                    f"{relative}\t{source}\t{'yes' if had_bom else 'no'}\t"
                    f"{body_hash}\t{description}\n")

    action = "migrated" if arguments.apply else "audited"
    retained = sum(row[1] == "retained" for row in rows)
    generated = len(rows) - retained
    print(f"USERLAND-HEADER-MIGRATE: {action} {len(rows)} files "
          f"({retained} retained explanations, {generated} generated)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
