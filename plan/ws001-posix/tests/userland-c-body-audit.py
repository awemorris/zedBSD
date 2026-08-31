#!/usr/bin/env python3
"""Audit the mechanically decidable function-body C-style rules."""

import argparse
import collections
import importlib.util
import pathlib
import re
import sys
from types import SimpleNamespace


SCRIPT_DIRECTORY = pathlib.Path(__file__).parent


def load_module(name, filename):
    """Load one neighboring audit helper without requiring a package."""
    specification = importlib.util.spec_from_file_location(
        name, SCRIPT_DIRECTORY / filename)
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


STRUCTURE = load_module("userland_c_structure_body", "userland-c-structure.py")
DECLARATIONS = load_module(
    "userland_c_declarations_body", "refactor-userland-declarations.py")
CONTROL = load_module(
    "userland_c_control_body", "refactor-userland-control-comments.py")
ANSI = load_module("userland_c_ansi_body", "refactor-userland-ansi-c.py")
SEMANTIC = load_module(
    "userland_c_semantic_body", "refactor-userland-semantic-layout.py")
CONTROL_BLOCKS = load_module(
    "userland_c_control_blocks_body", "refactor-userland-control-blocks.py")


def nested_declaration_diagnostics(text, type_names):
    """Find automatic declarations outside function-leading groups."""
    masked = STRUCTURE.mask_c(text)
    diagnostics = []
    for function in STRUCTURE.parse_functions(text):
        pairs = DECLARATIONS.matching_braces(
            masked, function.open_brace, function.close_brace)
        for opening, closing in pairs:
            if opening == function.open_brace:
                continue
            if not ANSI.compound_block(masked, function, opening):
                continue
            block = SimpleNamespace(open_brace=opening, close_brace=closing)
            statements = DECLARATIONS.leading_declarations(
                text, block, type_names)
            for start, _, statement in statements:
                line = text.count("\n", 0, start) + 1
                diagnostics.append(
                    (line, "automatic declaration is outside the function "
                     "declaration group"))
    return diagnostics


def declaration_spacing_diagnostics(text, type_names):
    """Find declaration groups without exactly one following blank line."""
    diagnostics = []
    for function in STRUCTURE.parse_functions(text):
        declarations = DECLARATIONS.leading_declarations(
            text, function, type_names)
        if not declarations:
            continue
        start = declarations[-1][1]
        end = start
        while end < function.close_brace and text[end] in " \t\r\n":
            end += 1
        if text[start:end].count("\n") == 2:
            continue
        line = text.count("\n", 0, start) + 1
        diagnostics.append(
            (line, "function declaration group lacks one following blank line"))
    return diagnostics


def scope_only_block_diagnostics(text):
    """Find standalone compound statements selected by the safe remover."""
    migrated, count = ANSI.refactor_scope_only_blocks(text)
    if migrated == text or count == 0:
        return []
    return [(1, f"file contains {count} standalone scope-only blocks")]


def matching_parenthesis(masked, opening):
    """Return the closing parenthesis for one masked expression."""
    depth = 0
    for position in range(opening, len(masked)):
        if masked[position] == "(":
            depth += 1
        elif masked[position] == ")":
            depth -= 1
            if depth == 0:
                return position
    return -1


def for_declaration_diagnostics(text, type_names):
    """Find declarations in for-loop initializers."""
    masked = STRUCTURE.mask_c(text)
    diagnostics = []
    for match in re.finditer(r"\bfor\s*\(", masked):
        opening = masked.find("(", match.start())
        closing = matching_parenthesis(masked, opening)
        if closing < 0:
            continue
        depth = 0
        semicolon = -1
        for position in range(opening + 1, closing):
            if masked[position] == "(":
                depth += 1
            elif masked[position] == ")" and depth:
                depth -= 1
            elif masked[position] == ";" and depth == 0:
                semicolon = position
                break
        if semicolon < 0:
            continue
        initializer = text[opening + 1:semicolon + 1]
        if DECLARATIONS.is_declaration(initializer, type_names):
            line = text.count("\n", 0, match.start()) + 1
            diagnostics.append((line, "for-loop initializer declares a variable"))
    return diagnostics


def malformed_multiline_comments(text):
    """Find prose on the opening line of a multi-line block comment."""
    diagnostics = []
    for start, _, prose in CONTROL.multiline_comment_openings(text):
        if prose.startswith("-*- "):
            continue
        line = text.count("\n", 0, start) + 1
        diagnostics.append((line, "multi-line comment has prose on opening line"))
    return diagnostics


def semantic_layout_diagnostics(text):
    """Find source that the deterministic semantic-layout pass would change."""
    migrated, counts = SEMANTIC.refactor(text)
    if migrated == text:
        return []
    labels = (
        "loop preparation comments", "direct call returns",
        "compound call returns", "if comments", "return comments",
        "comment paragraph gaps")
    details = ", ".join(
        f"{count} {label}" for count, label in zip(counts, labels) if count)
    if not details:
        details = "semantic paragraph spacing"
    return [(1, "semantic-layout transformer would change: " + details)]


def control_block_diagnostics(text):
    """Find source that the deterministic control-block pass would change."""
    migrated, counts = CONTROL_BLOCKS.refactor(text)
    if migrated == text:
        return []
    labels = (
        "if branches", "loop bodies", "overindented control statements",
        "block-entry blank lines")
    details = ", ".join(
        f"{count} {label}" for count, label in zip(counts, labels) if count)
    if not details:
        details = "control-block layout"
    return [(1, "control-block transformer would change: " + details)]


def audit_source(path, type_names):
    """Return body-style diagnostics for one implementation."""
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    diagnostics = []
    diagnostics.extend(nested_declaration_diagnostics(text, type_names))
    diagnostics.extend(declaration_spacing_diagnostics(text, type_names))
    diagnostics.extend(scope_only_block_diagnostics(text))
    diagnostics.extend(for_declaration_diagnostics(text, type_names))
    diagnostics.extend(malformed_multiline_comments(text))
    diagnostics.extend(control_block_diagnostics(text))
    diagnostics.extend(semantic_layout_diagnostics(text))
    return sorted(diagnostics)


def main():
    """Audit all C implementations and optionally write the review ledger."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--expected-c", type=int, default=214)
    parser.add_argument("--expected-h", type=int, default=44)
    parser.add_argument("--summary", action="store_true")
    parser.add_argument("--ledger")
    arguments = parser.parse_args()

    root = pathlib.Path(arguments.root)
    paths = STRUCTURE.relative_sources(root)
    c_count = sum(path.suffix == ".c" for path in paths)
    h_count = sum(path.suffix == ".h" for path in paths)
    failed = (c_count != arguments.expected_c or
              h_count != arguments.expected_h)
    type_names = DECLARATIONS.typedef_names(root)
    results = {}
    diagnostic_count = 0
    categories = collections.Counter()

    for path in paths:
        diagnostics = (audit_source(path, type_names)
                       if path.suffix == ".c" else [])
        results[path] = diagnostics
        diagnostic_count += len(diagnostics)
        categories.update(message for _, message in diagnostics)
        if diagnostics:
            failed = True
            if not arguments.summary:
                for line, message in diagnostics:
                    print(f"{path}:{line}: {message}")

    if arguments.ledger:
        ledger = pathlib.Path(arguments.ledger)
        rows = ["path\tstructure\tbody-mechanical\tsemantic-review"]
        for path in paths:
            body = ("not-applicable" if path.suffix == ".h" else
                    "pass" if not results[path] else "uncleared")
            semantic = ("not-applicable" if path.suffix == ".h" else
                        "uncleared")
            rows.append(f"{path}\tpass\t{body}\t{semantic}")
        ledger.write_text("\n".join(rows) + "\n", encoding="utf-8")

    print(f"USERLAND-C-BODY-STYLE: {c_count} C, {h_count} headers, "
          f"{diagnostic_count} mechanical residuals")
    for message, count in sorted(categories.items()):
        print(f"USERLAND-C-BODY-STYLE: {count} {message}")
    if failed:
        return 1
    print("USERLAND-C-BODY-STYLE-T001 audit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
