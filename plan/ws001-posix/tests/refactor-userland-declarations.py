#!/usr/bin/env python3
"""Separate safe function-entry declarations from their initial assignments."""

import argparse
import importlib.util
import pathlib
import re
import sys
from types import SimpleNamespace


STRUCTURE_PATH = pathlib.Path(__file__).with_name("userland-c-structure.py")
STRUCTURE_SPEC = importlib.util.spec_from_file_location(
    "userland_c_structure", STRUCTURE_PATH)
STRUCTURE = importlib.util.module_from_spec(STRUCTURE_SPEC)
sys.modules[STRUCTURE_SPEC.name] = STRUCTURE
STRUCTURE_SPEC.loader.exec_module(STRUCTURE)

BUILTIN_TYPES = {
    "_Bool", "bool", "char", "double", "float", "int", "long", "short",
    "signed", "unsigned", "void", "size_t", "ssize_t", "ptrdiff_t",
    "intptr_t", "uintptr_t", "FILE", "DIR", "Display", "Window", "GC",
    "KeySym", "Atom", "XEvent", "XImage", "XFontStruct",
}
QUALIFIERS = {"const", "volatile", "restrict", "register", "auto", "static"}


def typedef_names(root):
    """Collect plainly spelled typedef names used by the local source tree."""
    names = set(BUILTIN_TYPES)
    pattern = re.compile(r"\btypedef\b(?:(?!;).)*\b([A-Za-z_][A-Za-z0-9_]*)\s*;", re.S)
    for path in STRUCTURE.relative_sources(root):
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        masked = STRUCTURE.mask_c(text)
        for match in pattern.finditer(masked):
            names.add(match.group(1))
    return names


def is_declaration(statement, types):
    """Recognize a local declaration conservatively."""
    masked = STRUCTURE.mask_c(statement).strip()
    if masked.startswith("("):
        return False
    left = masked.split("=", 1)[0]
    identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", left)
    if not identifiers:
        return False
    index = 0
    while index < len(identifiers) and identifiers[index] in QUALIFIERS:
        index += 1
    if index >= len(identifiers):
        return False
    first = identifiers[index]
    if first in {"struct", "union", "enum"}:
        return len(identifiers) >= index + 3
    if first in types or first.endswith("_t"):
        type_match = re.search(r"\b" + re.escape(first) + r"\b", left)
        if type_match is not None:
            declarator = left[type_match.end():].lstrip()
            if declarator.startswith(("[", "=", ".", "->")):
                return False
        return len(identifiers) >= index + 2
    return False


def leading_declarations(text, function, types):
    """Return the leading declaration statements in one function body."""
    start = function.open_brace + 1
    end = function.close_brace
    masked = STRUCTURE.mask_c(text)
    position = start
    statements = []

    while position < end:
        probe = position
        while probe < end and masked[probe].isspace():
            probe += 1
        if probe >= end:
            break
        round_depth = 0
        square_depth = 0
        brace_depth = 0
        saw_assignment = False
        cursor = probe
        semicolon = -1
        while cursor < end:
            character = masked[cursor]
            if character == "(" :
                round_depth += 1
            elif character == ")" and round_depth:
                round_depth -= 1
            elif character == "[":
                square_depth += 1
            elif character == "]" and square_depth:
                square_depth -= 1
            elif (character == "=" and round_depth == 0 and
                  square_depth == 0 and brace_depth == 0):
                saw_assignment = True
            elif (character == "{" and round_depth == 0 and
                  square_depth == 0):
                if not saw_assignment and brace_depth == 0:
                    break
                brace_depth += 1
            elif (character == "}" and round_depth == 0 and
                  square_depth == 0 and brace_depth):
                brace_depth -= 1
            elif (character == ";" and round_depth == 0 and
                  square_depth == 0 and brace_depth == 0):
                semicolon = cursor
                break
            cursor += 1
        if semicolon < 0:
            break
        statement = text[probe:semicolon + 1]
        if not is_declaration(statement, types):
            break
        statements.append((probe, semicolon + 1, statement))
        position = semicolon + 1
    return statements


def top_level_positions(masked, targets):
    """Return target character positions outside nested declarators/expressions."""
    positions = []
    round_depth = 0
    square_depth = 0
    brace_depth = 0
    for index, character in enumerate(masked):
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
        elif (character in targets and round_depth == 0 and
              square_depth == 0 and brace_depth == 0):
            positions.append(index)
    return positions


def top_level_assignments(masked):
    """Return ordinary assignment operators outside nested expressions."""
    positions = top_level_positions(masked, {"="})
    assignments = []
    for position in positions:
        previous = masked[position - 1] if position > 0 else ""
        following = masked[position + 1] if position + 1 < len(masked) else ""
        if previous in "!<>=+-*/%&|^" or following == "=":
            continue
        assignments.append(position)
    return assignments


def split_initialized_declaration(statement):
    """Return an uninitialized declaration and ordered assignments when safe."""
    source = statement.strip()
    if not source.endswith(";"):
        return None
    source = source[:-1]
    masked = STRUCTURE.mask_c(source)
    if re.search(r"\b(?:static|extern)\b", masked):
        return source + ";", []
    comma_positions = top_level_positions(masked, {","})
    boundaries = [-1] + comma_positions + [len(source)]
    declarations = []
    assignments = []
    for index in range(len(boundaries) - 1):
        start = boundaries[index] + 1
        end = boundaries[index + 1]
        part = source[start:end].strip()
        part_masked = STRUCTURE.mask_c(part)
        equals = top_level_assignments(part_masked)
        if not equals:
            declarations.append(part)
            continue
        if len(equals) != 1:
            return None
        equal = equals[0]
        left = part[:equal].rstrip()
        value = part[equal + 1:].strip()
        if not value or value.startswith("{") or "[" in left:
            return None
        if re.search(r"\*\s*const\b", left):
            return None
        if re.search(r"\bconst\b", left) and "*" not in left:
            return None
        identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", left)
        if not identifiers:
            return None
        function_pointer = re.search(
            r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", left)
        name = (function_pointer.group(1) if function_pointer is not None
                else identifiers[-1])
        declarations.append(left)
        assignments.append((name, value))
    if not assignments:
        return source + ";", []
    return ", ".join(declarations) + ";", assignments


def refactor_function_group(text, function, types):
    """Build one safe function-entry declaration-group replacement."""
    statements = leading_declarations(text, function, types)
    if not statements:
        return None
    region = text[statements[0][0]:statements[-1][1]]
    for dimension in re.findall(r"\[([^\]]*)\]", STRUCTURE.mask_c(region)):
        identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", dimension)
        if any(identifier != "sizeof" and not identifier.isupper()
               for identifier in identifiers):
            return None
    declarations = []
    assignments = []
    for _, _, statement in statements:
        result = split_initialized_declaration(statement)
        if result is None:
            declarations.append(statement.strip())
            continue
        declaration, statement_assignments = result
        declarations.append(declaration)
        assignments.extend(statement_assignments)
    if not assignments:
        return None

    line_start = text.rfind("\n", 0, statements[0][0]) + 1
    indentation = text[line_start:statements[0][0]]
    replacement = "\n".join(indentation + value for value in declarations)
    replacement += "\n\n"
    replacement += "\n".join(
        indentation + name + " = " + value + ";"
        for name, value in assignments)
    return line_start, statements[-1][1], replacement


def matching_braces(masked, start, end):
    """Return compound-statement brace pairs within one function."""
    stack = []
    pairs = []
    for position in range(start, end + 1):
        if masked[position] == "{":
            stack.append(position)
        elif masked[position] == "}" and stack:
            opening = stack.pop()
            pairs.append((opening, position))
    return pairs


def direct_if_or_else_body(masked, opening):
    """Return true for a compound statement directly owned by if/else."""
    position = opening - 1
    while position >= 0 and masked[position].isspace():
        position -= 1
    if position < 0:
        return False
    if masked[position] == ")":
        depth = 1
        position -= 1
        while position >= 0 and depth:
            if masked[position] == ")":
                depth += 1
            elif masked[position] == "(":
                depth -= 1
            position -= 1
        while position >= 0 and masked[position].isspace():
            position -= 1
        end = position + 1
        while position >= 0 and (masked[position].isalnum() or
                                 masked[position] == "_"):
            position -= 1
        return masked[position + 1:end] == "if"
    end = position + 1
    while position >= 0 and (masked[position].isalnum() or
                             masked[position] == "_"):
        position -= 1
    return masked[position + 1:end] == "else"


def refactor(text, types):
    """Refactor safe non-branch block-entry declaration groups."""
    replacements = []
    masked = STRUCTURE.mask_c(text)
    for function in STRUCTURE.parse_functions(text):
        for opening, closing in matching_braces(
                masked, function.open_brace, function.close_brace):
            if opening != function.open_brace and direct_if_or_else_body(
                    masked, opening):
                continue
            block = SimpleNamespace(
                open_brace=opening, close_brace=closing)
            replacement = refactor_function_group(text, block, types)
            if replacement is not None:
                replacements.append(replacement)
    for start, end, replacement in sorted(
            replacements, key=lambda item: item[0], reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, len(replacements)


def main():
    """Apply or dry-run safe declaration separation."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expected-c", type=int, default=214)
    arguments = parser.parse_args()

    root = pathlib.Path(arguments.root)
    paths = [path for path in STRUCTURE.relative_sources(root)
             if path.suffix == ".c"]
    if len(paths) != arguments.expected_c:
        raise SystemExit(
            f"C inventory is {len(paths)}, expected {arguments.expected_c}")
    types = typedef_names(root)

    changed = 0
    groups = 0
    for path in paths:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        migrated, count = refactor(original, types)
        groups += count
        if migrated != original:
            changed += 1
            if arguments.apply:
                path.write_text(
                    migrated, encoding="utf-8", errors="surrogateescape",
                    newline="\n")

    action = "changed" if arguments.apply else "would change"
    print(f"USERLAND-C-STYLE-DECLARATIONS: {action} {changed} files, "
          f"normalized {groups} entry groups")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
