#!/usr/bin/env python3
"""Audit whole-userland structural rules from plan/coding-style.md."""

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
parse_functions = STRUCTURE.parse_functions
parse_static_prototypes = STRUCTURE.parse_static_prototypes
relative_sources = STRUCTURE.relative_sources

SEMANTIC_PATH = pathlib.Path(__file__).with_name(
    "refactor-userland-semantic-layout.py")
SEMANTIC_SPEC = importlib.util.spec_from_file_location(
    "userland_c_semantic_structure", SEMANTIC_PATH)
SEMANTIC = importlib.util.module_from_spec(SEMANTIC_SPEC)
sys.modules[SEMANTIC_SPEC.name] = SEMANTIC
SEMANTIC_SPEC.loader.exec_module(SEMANTIC)


MODELINE = "-*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*-"


def preceding_one_line_comment(text, line_start):
    """Return true when the immediately preceding line is a block comment."""
    if line_start == 0:
        return False
    previous_end = line_start - 1
    previous_start = text.rfind("\n", 0, previous_end) + 1
    previous = text[previous_start:previous_end].strip()
    return bool(re.fullmatch(r"/\*[^\n]*\*/", previous))


def preceding_loop_paragraph_comment(text, line_start):
    """Accept a comment before contiguous loop-preparation assignments."""
    cursor = line_start
    while cursor > 0:
        end = cursor - 1
        start = text.rfind("\n", 0, end) + 1
        if not SEMANTIC.line_is_assignment(text[start:end]):
            break
        cursor = start
    return preceding_one_line_comment(text, cursor)


def audit_control_comments(text):
    """Require an immediately preceding one-line intent comment."""
    masked = STRUCTURE.mask_c(text)
    diagnostics = []
    for match in re.finditer(r"\b(for|while|switch)\b", masked):
        kind = match.group(1)
        if kind == "while" and masked[:match.start()].rstrip().endswith("}"):
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        if text[line_start:match.start()].strip():
            diagnostics.append(
                f"line {text.count(chr(10), 0, match.start()) + 1}: "
                f"{kind} is not on its own indented line")
            continue
        commented = preceding_one_line_comment(text, line_start)
        if kind in {"for", "while"} and not commented:
            commented = preceding_loop_paragraph_comment(text, line_start)
        if not commented:
            diagnostics.append(
                f"line {text.count(chr(10), 0, match.start()) + 1}: "
                f"{kind} lacks an immediately preceding one-line intent comment")
    return diagnostics


def definition_header_is_split(function):
    """Check the required return/name/argument physical-line layout."""
    lines = function.header.splitlines()
    if len(lines) < 3:
        return False
    if lines[1].strip() != function.name + "(":
        return False
    if not lines[0].strip():
        return False
    return all(line.startswith("\t") or line.strip() == ")"
               for line in lines[2:])


def audit_source(path):
    """Return structural diagnostics for one C implementation."""
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    functions = parse_functions(text)
    diagnostics = []

    if MODELINE not in "\n".join(text.splitlines()[:20]):
        diagnostics.append("missing required modeline in first 20 lines")

    first_function = functions[0].header_start if functions else None
    prototypes = parse_static_prototypes(text, first_function)
    prototype_names = [prototype.name for prototype in prototypes]
    normal_static = [function for function in functions
                     if function.is_static and not function.is_inline]

    seen_static = False
    for function in functions:
        if function.is_static:
            seen_static = True
        elif seen_static:
            diagnostics.append(
                f"{function.name}: public definition follows a static definition")

    for function in normal_static:
        count = prototype_names.count(function.name)
        if count == 0:
            diagnostics.append(
                f"{function.name}: normal static function lacks a forward declaration")
        elif count > 1:
            diagnostics.append(
                f"{function.name}: static function has {count} forward declarations")

    defined_names = {function.name for function in normal_static}
    for prototype in prototypes:
        if "\n" in prototype.text:
            diagnostics.append(
                f"{prototype.name}: static forward declaration is not one physical line")
        if prototype.name not in defined_names:
            diagnostics.append(
                f"{prototype.name}: static forward declaration has no normal static definition")

    for function in functions:
        if not definition_header_is_split(function):
            diagnostics.append(
                f"{function.name}: definition header is not split by return/name/argument")
        prefix = text[function.start:function.header_start]
        comments = re.findall(r"/\*.*?\*/", prefix, re.S)
        if not comments:
            kind = "static" if function.is_static else "public"
            diagnostics.append(f"{function.name}: {kind} function header comment is missing")
            continue
        comment = comments[-1]
        if function.is_static and "\n" in comment:
            diagnostics.append(
                f"{function.name}: static function header comment is not one line")
        if not function.is_static:
            if not comment.startswith("/*\n * ") or not comment.endswith("\n */"):
                diagnostics.append(
                    f"{function.name}: public function header comment is not multi-line")

    for number, line in enumerate(text.splitlines(), 1):
        if line.rstrip(" \t") != line:
            diagnostics.append(f"line {number}: trailing whitespace")
        masked_line = STRUCTURE.mask_c(line)
        label = re.match(r"^\s*(?:case\b|default\b)", masked_line)
        if label is not None:
            colon = masked_line.find(":", label.end())
            if colon >= 0 and masked_line[colon + 1:].strip():
                diagnostics.append(
                    f"line {number}: statement appears on a case label")

    diagnostics.extend(audit_control_comments(text))

    return diagnostics, functions


def audit_header(path):
    """Return applicable structural diagnostics for one C header."""
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    diagnostics = []
    if MODELINE not in "\n".join(text.splitlines()[:20]):
        diagnostics.append("missing required modeline in first 20 lines")
    for number, line in enumerate(text.splitlines(), 1):
        if line.rstrip(" \t") != line:
            diagnostics.append(f"line {number}: trailing whitespace")
    return diagnostics, []


def main():
    """Audit the deterministic full userland inventory."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--expected-c", type=int, default=214)
    parser.add_argument("--expected-h", type=int, default=44)
    parser.add_argument("--summary", action="store_true")
    arguments = parser.parse_args()

    paths = relative_sources(pathlib.Path(arguments.root))
    c_count = sum(path.suffix == ".c" for path in paths)
    h_count = sum(path.suffix == ".h" for path in paths)
    failed = c_count != arguments.expected_c or h_count != arguments.expected_h
    diagnostics_count = 0
    function_count = 0

    if failed:
        print(f"USERLAND-C-STYLE: inventory is {c_count} C/{h_count} headers; "
              f"expected {arguments.expected_c} C/{arguments.expected_h} headers")

    for path in paths:
        if path.suffix == ".c":
            diagnostics, functions = audit_source(path)
        else:
            diagnostics, functions = audit_header(path)
        function_count += len(functions)
        diagnostics_count += len(diagnostics)
        if not arguments.summary:
            for diagnostic in diagnostics:
                print(f"{path}: {diagnostic}")
        if diagnostics:
            failed = True

    print(f"USERLAND-C-STYLE: {c_count} C, {h_count} headers, "
          f"{function_count} functions, {diagnostics_count} diagnostics")
    if failed:
        return 1
    print("USERLAND-C-STYLE-T001 structural audit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
