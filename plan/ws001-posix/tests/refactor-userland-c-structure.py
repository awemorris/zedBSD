#!/usr/bin/env python3
"""Apply behavior-neutral file-scope organization from the C style contract."""

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

MODELINE = "/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */"


def add_modeline(text):
    """Put the required modeline at byte zero before the source header."""
    prefix = MODELINE + "\n\n"
    if text.startswith(prefix):
        return text
    position = text.find(prefix, 0, min(len(text), 2048))
    if position >= 0:
        text = text[:position] + text[position + len(prefix):]
    if not text.startswith("/*\n * zedBSD\n"):
        raise ValueError("canonical header and explanation prefix not found")
    return prefix + text


def comment_prose(comment):
    """Extract prose from one C block comment."""
    if comment is None:
        return ""
    content = comment[2:-2]
    words = []
    for line in content.splitlines():
        line = re.sub(r"^\s*\*?\s?", "", line).strip()
        if line:
            words.append(line)
    return " ".join(words)


def human_name(name):
    """Render a C identifier for generated comments."""
    return name.strip("_").replace("_", " ") or name


def function_comment(text, function, path):
    """Return the required public or static function header comment."""
    existing = text[function.start:function.header_start].strip()
    prose = comment_prose(existing if existing.startswith("/*") else None)
    if function.is_static:
        if not prose:
            prose = f"Supports the {human_name(function.name)} operation."
        return f"/* {prose} */"

    if not prose:
        if function.name == "main":
            command = path.parent.name.replace("-", " ")
            prose = f"Runs the {command} command."
        else:
            prose = f"Implements the {human_name(function.name)} operation."
    return f"/*\n * {prose}\n */"


def parameter_parts(parameters):
    """Split a parameter list at its file-level commas."""
    parts = []
    start = 0
    round_depth = 0
    square_depth = 0
    for index, character in enumerate(parameters):
        if character == "(":
            round_depth += 1
        elif character == ")" and round_depth:
            round_depth -= 1
        elif character == "[":
            square_depth += 1
        elif character == "]" and square_depth:
            square_depth -= 1
        elif character == "," and round_depth == 0 and square_depth == 0:
            parts.append(parameters[start:index])
            start = index + 1
    parts.append(parameters[start:])
    return [" ".join(part.split()) for part in parts if part.strip()]


def name_parentheses(header, name):
    """Find the parameter parentheses belonging to a function name."""
    masked = STRUCTURE.mask_c(header)
    matches = list(re.finditer(r"\b" + re.escape(name) + r"\s*\(", masked))
    if not matches:
        raise ValueError(f"cannot locate parameter list for {name}")
    match = matches[-1]
    opening = masked.find("(", match.start())
    closing = STRUCTURE.matching(masked, opening, "(", ")")
    if closing < 0:
        raise ValueError(f"unterminated parameter list for {name}")
    return match.start(), opening, closing


def format_definition_header(function):
    """Put return type, name, and each argument on separate lines."""
    name_start, opening, closing = name_parentheses(
        function.header, function.name)
    prefix = " ".join(function.header[:name_start].split())
    parameters = parameter_parts(function.header[opening + 1:closing])
    suffix = " ".join(function.header[closing + 1:].split())
    lines = [prefix, function.name + "("]
    if parameters:
        for index, parameter in enumerate(parameters):
            ending = "," if index + 1 < len(parameters) else ")"
            lines.append("\t" + parameter + ending)
    else:
        lines.append(")")
    if suffix:
        lines[-1] += " " + suffix
    return "\n".join(lines)


def prototype_for(function):
    """Return the required one-line static forward declaration."""
    header = " ".join(function.header.split())
    header = header.replace("( ", "(").replace(" )", ")")
    header = re.sub(
        r"([*])\s+" + re.escape(function.name) + r"\b",
        r"\1" + function.name,
        header)
    return header + ";"


def static_call_order(functions):
    """Return static definitions in top-down first-use traversal order."""
    statics = [function for function in functions if function.is_static]
    by_name = {function.name: function for function in statics}
    calls = {}
    for function in functions:
        masked = STRUCTURE.mask_c(function.body)
        called = []
        for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", masked):
            name = match.group(1)
            if name in by_name and name not in called:
                called.append(name)
        calls[function.name] = called

    ordered = []
    visited = set()

    def visit(name):
        if name in visited or name not in by_name:
            return
        visited.add(name)
        ordered.append(by_name[name])
        for called_name in calls.get(name, []):
            visit(called_name)

    for function in functions:
        if not function.is_static:
            for name in calls.get(function.name, []):
                visit(name)
    for function in statics:
        visit(function.name)
    return ordered


def has_conditional_function_regions(text, functions):
    """Return true for preprocessor conditionals interleaved with definitions."""
    if not functions:
        return False
    masked = STRUCTURE.mask_c(text)
    depths = []
    depth = 0
    for character in masked:
        depths.append(depth)
        if character == "{":
            depth += 1
        elif character == "}" and depth:
            depth -= 1
    first = functions[0].start
    last = functions[-1].close_brace
    pattern = re.compile(
        r"^\s*#\s*(?:if|ifdef|ifndef|else|elif|endif)\b[^\n]*", re.M)
    return any(first < match.start() < last and depths[match.start()] == 0
               for match in pattern.finditer(text))


def remove_spans(text, spans):
    """Remove non-overlapping source spans."""
    result = []
    position = 0
    for start, end in sorted(spans):
        if start < position:
            raise ValueError("overlapping file-scope spans")
        result.append(text[position:start])
        position = end
    result.append(text[position:])
    return "".join(result)


def refactor_c(path, text):
    """Refactor one C file's file-scope structure."""
    functions = STRUCTURE.parse_functions(text)
    if not functions:
        return text, "no-functions"
    if path.as_posix().endswith("/libc/atomic-runtime.c"):
        return refactor_atomic_runtime(path, text), "macro-generated"
    if path.as_posix().endswith("/rtld/rtld.c"):
        return refactor_rtld(path, text), "conditional"
    conditional = has_conditional_function_regions(text, functions)
    safe_conditional_preamble = path.as_posix().endswith(
        ("/libc/posix.c", "/libc/pthread.c"))
    if conditional and not safe_conditional_preamble:
        return text, "conditional"

    prototypes = STRUCTURE.parse_static_prototypes(text)
    spans = [(function.start, function.close_brace + 1)
             for function in functions]
    spans.extend((prototype.start, prototype.end) for prototype in prototypes)
    preamble = remove_spans(text, spans).rstrip()
    preamble = re.sub(r"\n{3,}", "\n\n", preamble)

    public = [function for function in functions if not function.is_static]
    static = static_call_order(functions)
    ordered = public + static
    normal_static = [function for function in static if not function.is_inline]

    declarations = [prototype_for(function) for function in normal_static]
    if declarations:
        preamble += "\n\n" + "\n".join(declarations)

    blocks = []
    for function in ordered:
        comment = function_comment(text, function, path)
        header = format_definition_header(function)
        blocks.append(comment + "\n" + header + "\n" + function.body)
    return preamble + "\n\n" + "\n\n".join(blocks) + "\n", "refactored"


def refactor_headers_in_place(path, text):
    """Normalize comments and definition headers without moving functions."""
    functions = STRUCTURE.parse_functions(text)
    replacements = []
    for function in functions:
        comment = function_comment(text, function, path)
        header = format_definition_header(function)
        replacements.append((
            function.start,
            function.open_brace,
            comment + "\n" + header + "\n"))
    for start, end, replacement in reversed(replacements):
        text = text[:start] + replacement + text[end:]
    return text


def refactor_atomic_runtime(path, text):
    """Preserve the ABI-generating macro while placing its helper last."""
    prototypes = [prototype for prototype in STRUCTURE.parse_static_prototypes(text)
                  if prototype.name == "atomic_call"]
    text = remove_spans(text, [
        (prototype.start, prototype.end) for prototype in prototypes])
    text = re.sub(r"\n{3,}", "\n\n", text)
    marker = "static intptr_t\natomic_call"
    if marker in text and "/* Invokes the kernel atomic operation. */\n" not in text:
        text = text.replace(
            marker,
            "/* Invokes the kernel atomic operation. */\n" + marker,
            1)
    text = refactor_headers_in_place(path, text)
    functions = STRUCTURE.parse_functions(text)
    helpers = [function for function in functions
               if function.name == "atomic_call"]
    if len(helpers) != 1:
        raise ValueError("atomic runtime helper definition is ambiguous")
    helper = helpers[0]
    helper_block = text[helper.start:helper.close_brace + 1]
    text = text[:helper.start] + text[helper.close_brace + 1:]

    functions = STRUCTURE.parse_functions(text)
    if not functions:
        raise ValueError("atomic runtime public definitions are missing")
    declaration = prototype_for(helper)
    insertion = functions[0].start
    text = text[:insertion] + declaration + "\n\n" + text[insertion:]
    text = text.rstrip() + "\n\n" + helper_block + "\n"
    return text


def conditional_function_regions(text, functions):
    """Return outer file-scope conditional regions containing definitions."""
    masked = STRUCTURE.mask_c(text)
    depths = []
    depth = 0
    for character in masked:
        depths.append(depth)
        if character == "{":
            depth += 1
        elif character == "}" and depth:
            depth -= 1

    directives = list(re.finditer(
        r"^\s*#\s*(if|ifdef|ifndef|endif)\b[^\n]*(?:\n|\Z)", text, re.M))
    stack = []
    candidates = []
    for directive in directives:
        if depths[directive.start()] != 0:
            continue
        kind = directive.group(1)
        if kind in {"if", "ifdef", "ifndef"}:
            stack.append(directive)
        elif kind == "endif" and stack:
            opening = stack.pop()
            if not stack:
                candidates.append((opening.start(), directive.end()))

    regions = []
    for start, end in candidates:
        members = [function for function in functions
                   if start < function.header_start < end]
        if members:
            regions.append((start, end, members))
    return regions


def refactor_rtld(path, text):
    """Reorder runtime-linker definitions while retaining architecture guards."""
    text = re.sub(
        r"(?m)^#if[^\n]*\nstatic [^\n]+;\n#endif\n",
        "",
        text)
    text = re.sub(
        r"(?m)^#if[^\n]*\n(?:[ \t]*\n)*#endif\n",
        "",
        text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    text = refactor_headers_in_place(path, text)
    functions = STRUCTURE.parse_functions(text)
    regions = conditional_function_regions(text, functions)
    prototypes = STRUCTURE.parse_static_prototypes(text)

    region_for = {}
    for index, (_, _, members) in enumerate(regions):
        for function in members:
            region_for[function.name] = index

    spans = [(prototype.start, prototype.end) for prototype in prototypes]
    spans.extend((start, end) for start, end, _ in regions)
    spans.extend((function.start, function.close_brace + 1)
                 for function in functions if function.name not in region_for)
    preamble = remove_spans(text, spans).rstrip()
    preamble = re.sub(r"\n{3,}", "\n\n", preamble)

    unconditional = [function for function in functions
                     if function.is_static and not function.is_inline and
                     function.name not in region_for]
    declarations = [prototype_for(function) for function in unconditional]
    for start, end, members in regions:
        static_members = [function for function in members
                          if function.is_static and not function.is_inline]
        if not static_members:
            continue
        if len(members) != 1:
            raise ValueError("conditional rtld region has multiple definitions")
        lines = text[start:end].splitlines()
        opening = next(line for line in lines if line.lstrip().startswith("#"))
        closing = next(line for line in reversed(lines)
                       if line.lstrip().startswith("#endif"))
        declarations.extend(
            (opening, prototype_for(static_members[0]), closing))

    if declarations:
        preamble += "\n\n" + "\n".join(declarations)

    normal_blocks = {
        function.name: text[function.start:function.close_brace + 1]
        for function in functions if function.name not in region_for
    }
    region_blocks = {
        index: text[start:end].strip()
        for index, (start, end, _) in enumerate(regions)
    }

    def block(function):
        if function.name in region_for:
            return region_blocks[region_for[function.name]]
        return normal_blocks[function.name]

    ordered_functions = ([function for function in functions
                          if not function.is_static] +
                         static_call_order(functions))
    blocks = []
    seen_regions = set()
    for function in ordered_functions:
        region = region_for.get(function.name)
        if region is not None:
            if region in seen_regions:
                continue
            seen_regions.add(region)
        blocks.append(block(function))
    return preamble + "\n\n" + "\n\n".join(blocks) + "\n"


def main():
    """Apply or dry-run the deterministic structural refactoring."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expected-c", type=int, default=214)
    parser.add_argument("--expected-h", type=int, default=44)
    arguments = parser.parse_args()

    paths = STRUCTURE.relative_sources(pathlib.Path(arguments.root))
    c_count = sum(path.suffix == ".c" for path in paths)
    h_count = sum(path.suffix == ".h" for path in paths)
    if c_count != arguments.expected_c or h_count != arguments.expected_h:
        raise SystemExit(
            f"inventory is {c_count} C/{h_count} headers, expected "
            f"{arguments.expected_c} C/{arguments.expected_h} headers")

    statuses = {}
    changed = 0
    for path in paths:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        migrated = add_modeline(original)
        status = "header"
        if path.suffix == ".c":
            migrated, status = refactor_c(path, migrated)
        statuses[status] = statuses.get(status, 0) + 1
        if migrated != original:
            changed += 1
            if arguments.apply:
                path.write_text(
                    migrated, encoding="utf-8", errors="surrogateescape",
                    newline="\n")

    action = "changed" if arguments.apply else "would change"
    summary = ", ".join(f"{key}={value}" for key, value in sorted(statuses.items()))
    print(f"USERLAND-C-STYLE-MIGRATE: {action} {changed} files ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
