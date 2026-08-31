#!/usr/bin/env python3
"""Apply the mechanically decidable semantic-paragraph layout rules."""

import argparse
import importlib.util
import pathlib
import re
import sys


STRUCTURE_PATH = pathlib.Path(__file__).with_name("userland-c-structure.py")
STRUCTURE_SPEC = importlib.util.spec_from_file_location(
    "userland_c_structure_semantic", STRUCTURE_PATH)
STRUCTURE = importlib.util.module_from_spec(STRUCTURE_SPEC)
sys.modules[STRUCTURE_SPEC.name] = STRUCTURE
STRUCTURE_SPEC.loader.exec_module(STRUCTURE)


def words(identifier):
    """Turn one C identifier into a short English noun phrase."""
    value = re.sub(r"^(?:is|has|have|can|should|needs?)_", "", identifier)
    value = value.strip("_").replace("_", " ")
    return value or "operation"


def first_call(expression):
    """Return the first significant call name in one expression."""
    ignored = {"sizeof", "strcmp", "strncmp", "memcmp", "likely", "unlikely"}
    for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", expression):
        if match.group(1) not in ignored:
            return match.group(1)
    return None


def purpose_for_if(expression):
    """Derive a bounded verb-and-object purpose from one decision."""
    lower = expression.lower()
    call = first_call(expression)
    identifiers = re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", expression)

    if "argc" in identifiers or "argv" in identifiers or "optind" in identifiers:
        if any(name in lower for name in ("strcmp", "strncmp", "getopt")):
            return "Handles the selected command-line operation."
        return "Validates the command-line arguments."
    if "errno" in identifiers:
        return "Handles the reported system error."
    if re.search(r"\bEOF\b", expression):
        return "Handles the end-of-file condition."
    if any(name in lower for name in ("failed", "failure", "error")):
        return "Handles an operation failure."
    if call is not None and re.search(r"(?:!|==|!=|<|>)", expression):
        return "Handles a failed " + words(call) + " operation."
    null_match = re.search(
        r"\b([A-Za-z_][A-Za-z0-9_]*)\b\s*(?:==|!=)\s*NULL", expression)
    if null_match is not None:
        return "Handles the " + words(null_match.group(1)) + " availability."
    descriptions = {
        "result": "Checks the operation result.",
        "status": "Checks the operation status.",
        "count": "Checks the remaining item count.",
        "length": "Checks the current data length.",
        "size": "Checks the current data size.",
        "index": "Checks the current index.",
        "descriptor": "Checks the file descriptor.",
        "fd": "Checks the file descriptor.",
        "child": "Checks the child process state.",
        "flags": "Checks the active flags.",
        "options": "Checks the selected options.",
        "parser": "Checks the parser state.",
        "character": "Classifies the current input character.",
        "value": "Validates the current value.",
        "text": "Validates the current text.",
        "name": "Validates the current name.",
        "strcmp": "Selects the matching value.",
        "strncmp": "Selects the matching prefix.",
        "n": "Checks the current item count.",
        "cursor": "Checks the current cursor position.",
        "c": "Classifies the current input character.",
        "p": "Checks the current pointer.",
        "end": "Checks the current endpoint.",
        "s": "Checks the current string state.",
        "object": "Checks the current object.",
        "f": "Checks the current file state.",
        "used": "Checks the current capacity usage.",
        "d": "Checks the current descriptor.",
        "terminal": "Checks the terminal state.",
        "operation": "Validates the selected operation.",
        "offset": "Checks the current offset.",
        "input": "Validates the current input.",
        "i": "Checks the current index.",
        "byte": "Classifies the current byte.",
        "who": "Checks the selected user entry.",
        "key": "Handles the selected key.",
        "mode": "Validates the selected mode.",
        "x": "Checks the current horizontal value.",
    }
    for identifier in identifiers:
        if identifier.lower() in {
                "if", "null", "true", "false", "int", "char", "long",
                "unsigned", "size_t", "ssize_t"}:
            continue
        if identifier.isupper():
            continue
        if identifier.lower() in descriptions:
            return descriptions[identifier.lower()]
        return "Handles the " + words(identifier) + " condition."
    return "Handles the current operation condition."


def control_expression(source, masked, keyword_end):
    """Return the parenthesized expression following a control keyword."""
    opening = keyword_end
    while opening < len(masked) and masked[opening].isspace():
        opening += 1
    if opening >= len(masked) or masked[opening] != "(":
        return ""
    closing = STRUCTURE.matching(masked, opening, "(", ")")
    if closing < 0:
        return ""
    return " ".join(source[opening + 1:closing].split())


def previous_line(text, line_start):
    """Return the preceding physical line without its newline."""
    if line_start == 0:
        return ""
    end = line_start - 1
    start = text.rfind("\n", 0, end) + 1
    return text[start:end]


def preceding_comment(text, line_start):
    """Return true when the previous nonblank line ends a block comment."""
    prefix = text[:line_start].rstrip()
    return prefix.endswith("*/")


def add_if_comments(text):
    """Add purpose comments before decisions that have no paragraph comment."""
    masked = STRUCTURE.mask_c(text)
    additions = []
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\bif\s*\(", region):
            start = base + match.start()
            line_start = text.rfind("\n", 0, start) + 1
            before = masked[line_start:start]
            if before.strip():
                continue
            if preceding_comment(text, line_start):
                continue
            expression = control_expression(text, masked, start + 2)
            indentation = text[line_start:start]
            comment = indentation + "/* " + purpose_for_if(expression) + " */\n"
            additions.append((line_start, comment))
    for position, comment in reversed(additions):
        text = text[:position] + comment + text[position:]
    return text, len(additions)


def refresh_generated_if_comments(text):
    """Refresh bounded generated decision comments from their expressions."""
    masked = STRUCTURE.mask_c(text)
    replacements = []
    generated = re.compile(
        r"/\* (?:Handles (?:the|an|a) .*|Validates the command-line arguments\.) \*/")
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\bif\s*\(", region):
            start = base + match.start()
            line_start = text.rfind("\n", 0, start) + 1
            if text[line_start:start].strip():
                continue
            comment_end = line_start - 1
            comment_start = text.rfind("\n", 0, comment_end) + 1
            comment = text[comment_start:comment_end].strip()
            if generated.fullmatch(comment) is None:
                continue
            expression = control_expression(text, masked, start + 2)
            indentation = text[comment_start:comment_end][
                :len(text[comment_start:comment_end]) -
                len(text[comment_start:comment_end].lstrip())]
            replacement = indentation + "/* " + purpose_for_if(expression) + " */"
            if replacement != text[comment_start:comment_end]:
                replacements.append((comment_start, comment_end, replacement))
    for start, end, replacement in reversed(replacements):
        text = text[:start] + replacement + text[end:]
    return text, len(replacements)


def line_is_assignment(line):
    """Recognize one simple complete assignment statement."""
    stripped = line.strip()
    if not stripped.endswith(";") or stripped.startswith(("return ", "#")):
        return False
    masked = STRUCTURE.mask_c(stripped)
    return bool(re.search(r"(?<![!<>=+\-*/%&|^])=(?!=)", masked))


def move_loop_comments(text):
    """Move loop comments above immediately preceding preparation assignments."""
    lines = text.splitlines(keepends=True)
    moves = 0
    index = 1
    while index + 1 < len(lines):
        comment = re.match(r"^(\s*)/\* [^\n]* \*/\s*$", lines[index])
        following = re.match(r"^\s*(?:for|while)\s*\(", lines[index + 1])
        if comment is None or following is None or not line_is_assignment(lines[index - 1]):
            index += 1
            continue
        start = index - 1
        while start > 0 and line_is_assignment(lines[start - 1]):
            start -= 1
        value = lines.pop(index)
        lines.insert(start, value)
        moves += 1
        index += 2
    return "".join(lines), moves


def comment_blank_lines(text):
    """Put a paragraph break before comments inside function bodies."""
    lines = text.splitlines(keepends=True)
    function_lines = set()
    for function in STRUCTURE.parse_functions(text):
        first = text.count("\n", 0, function.open_brace) + 1
        last = text.count("\n", 0, function.close_brace) + 1
        function_lines.update(range(first + 1, last + 1))

    additions = 0
    result = []
    in_comment = False
    for number, line in enumerate(lines, 1):
        stripped = line.strip()
        opening = stripped.startswith("/*") and not in_comment
        if opening and number in function_lines and result:
            previous = result[-1].strip()
            exempt = (not previous or previous.endswith("{") or
                      re.match(r"^(?:case\b.*|default):$", previous) or
                      previous.startswith("/*") or previous.startswith("*"))
            if not exempt:
                result.append("\n")
                additions += 1
        result.append(line)
        if opening and "*/" not in stripped:
            in_comment = True
        if in_comment and "*/" in stripped:
            in_comment = False
    return "".join(result), additions


def add_return_comments(text):
    """Explain independent returns that have no decision or result comment."""
    masked = STRUCTURE.mask_c(text)
    additions = []
    for function in STRUCTURE.parse_functions(text):
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        for match in re.finditer(r"\breturn\b", region):
            start = base + match.start()
            line_start = text.rfind("\n", 0, start) + 1
            if masked[line_start:start].strip() or preceding_comment(text, line_start):
                continue
            prior = previous_line(text, line_start).strip()
            if prior.startswith(("if ", "if(")):
                continue
            end = masked.find(";", start)
            expression = masked[start + len("return"):end].strip()
            if expression in {"0", "EXIT_SUCCESS", "true"}:
                prose = "Reports successful completion."
            elif expression in {"1", "2", "-1", "EXIT_FAILURE", "false"}:
                prose = "Reports operation failure."
            elif expression in {"NULL", "EOF"}:
                prose = "Reports that no result is available."
            else:
                prose = "Returns the computed result."
            indentation = text[line_start:start]
            additions.append((line_start, indentation + "/* " + prose + " */\n"))
    for position, comment in reversed(additions):
        text = text[:position] + comment + text[position:]
    return text, len(additions)


def function_return_type(function):
    """Return a declaration-ready result type for one ordinary function."""
    matches = list(re.finditer(
        r"\b" + re.escape(function.name) + r"\s*\(", function.header))
    if not matches:
        return None
    value = function.header[:matches[-1].start()].strip()
    value = re.sub(r"\b(?:static|inline|__inline|_Noreturn)\b", "", value)
    while "__attribute__" in value:
        start = value.find("__attribute__")
        opening = value.find("(", start)
        if opening < 0:
            break
        masked = STRUCTURE.mask_c(value)
        closing = STRUCTURE.matching(masked, opening, "(", ")")
        if closing < 0:
            break
        value = value[:start] + value[closing + 1:]
    value = " ".join(value.split())
    if not value or value == "void":
        return None
    return value


def direct_call_returns(text):
    """Store direct call results before returning them to the caller."""
    masked = STRUCTURE.mask_c(text)
    replacements = []
    declarations = []
    count = 0
    for function in STRUCTURE.parse_functions(text):
        result_type = function_return_type(function)
        if result_type is None:
            continue
        region = masked[function.open_brace + 1:function.close_brace]
        base = function.open_brace + 1
        matches = []
        for match in re.finditer(
                r"(?m)^([ \t]*)return\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(",
                region):
            start = base + match.start()
            opening = masked.find("(", base + match.start(), base + match.end())
            closing = STRUCTURE.matching(masked, opening, "(", ")")
            if closing < 0:
                continue
            semicolon = closing + 1
            while semicolon < function.close_brace and masked[semicolon].isspace():
                semicolon += 1
            if semicolon >= function.close_brace or masked[semicolon] != ";":
                continue
            matches.append((start, semicolon + 1, match.group(1), match.group(2)))
        if not matches:
            continue

        identifiers = set(re.findall(
            r"\b[A-Za-z_][A-Za-z0-9_]*\b",
            masked[function.open_brace:function.close_brace + 1]))
        name = "function_result"
        suffix = 2
        while name in identifiers:
            name = "function_result_" + str(suffix)
            suffix += 1

        line_end = text.find("\n", function.open_brace)
        if line_end < 0:
            continue
        separator = "" if result_type.endswith("*") else " "
        declaration = "\t" + result_type + separator + name + ";\n"
        declarations.append((line_end + 1, declaration))
        for start, end, indentation, callee in matches:
            statement = text[start:end]
            return_match = re.search(r"\breturn\s+", statement)
            expression = statement[return_match.end():].rstrip()
            if expression.endswith(";"):
                expression = expression[:-1].rstrip()
            indent = re.match(r"\s*", statement).group(0)
            generated = indent + "/* Returns the computed result. */\n"
            if text[max(0, start - len(generated)):start] == generated:
                start -= len(generated)
            replacement = (
                indent + "/* Obtains the " + words(callee) + " result. */\n" +
                indent + name + " = " + expression + ";\n\n" +
                indent + "/* Returns the computed result. */\n" +
                indent + "return " + name + ";")
            replacements.append((start, end, replacement))
            count += 1

    operations = replacements + [
        (position, position, declaration)
        for position, declaration in declarations]
    for start, end, replacement in sorted(operations, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, count


def compound_call_returns(text):
    """Store compound expressions containing calls before returning them."""
    masked = STRUCTURE.mask_c(text)
    replacements = []
    declarations = []
    count = 0
    for function in STRUCTURE.parse_functions(text):
        result_type = function_return_type(function)
        if result_type is None:
            continue
        body_masked = masked[function.open_brace + 1:function.close_brace]
        body_text = text[function.open_brace + 1:function.close_brace]
        names = re.findall(r"\b(function_result(?:_[0-9]+)?)\b", body_masked)
        if names:
            name = names[0]
        else:
            identifiers = set(re.findall(
                r"\b[A-Za-z_][A-Za-z0-9_]*\b", body_masked))
            name = "function_result"
            suffix = 2
            while name in identifiers:
                name = "function_result_" + str(suffix)
                suffix += 1

        matches = []
        base = function.open_brace + 1
        for match in re.finditer(r"(?m)^([ \t]*)return\b", body_masked):
            start = base + match.start()
            semicolon = masked.find(";", start + len(match.group(0)))
            if semicolon < 0 or semicolon >= function.close_brace:
                continue
            expression = masked[start + len(match.group(0)):semicolon].strip()
            if expression == name:
                continue
            if not re.search(r"\b[A-Za-z_][A-Za-z0-9_]*\s*\(", expression):
                continue
            matches.append((start, semicolon + 1))
        if not matches:
            continue
        if not names:
            line_end = text.find("\n", function.open_brace)
            separator = "" if result_type.endswith("*") else " "
            declaration = "\t" + result_type + separator + name + ";\n"
            declarations.append((line_end + 1, declaration))

        for start, end in matches:
            statement = text[start:end]
            return_match = re.search(r"\breturn\s+", statement)
            expression = statement[return_match.end():].rstrip()
            if expression.endswith(";"):
                expression = expression[:-1].rstrip()
            indent = re.match(r"[ \t]*", statement).group(0)
            generated = indent + "/* Returns the computed result. */\n"
            if text[max(0, start - len(generated)):start] == generated:
                start -= len(generated)
            replacement = (
                indent + "/* Computes the function result. */\n" +
                indent + name + " = " + expression + ";\n\n" +
                indent + "/* Returns the computed result. */\n" +
                indent + "return " + name + ";")
            replacements.append((start, end, replacement))
            count += 1

    operations = replacements + [
        (position, position, declaration)
        for position, declaration in declarations]
    for start, end, replacement in sorted(operations, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, count


def normalize_result_declaration_gaps(text):
    """Separate an inserted result-only declaration from executable code."""
    pattern = re.compile(
        r"(?m)^(\t[^\n;]*\bfunction_result(?:_[0-9]+)?;)\n"
        r"(?=(?:\t/\*|\t(?:if|for|while|switch|return|assert)\b|"
        r"\t(?:UNUSED_PARAMETER)\b|"
        r"\t(?:\([^)]+\)|[A-Za-z_][A-Za-z0-9_]*)\s*\(|"
        r"\t\([^)]+\)[^;]+;|"
        r"\t(?:\([^)]+\)|\*?[A-Za-z_][A-Za-z0-9_]*(?:\.|->[^ ]+)?)\s*=|#))")
    return pattern.sub(r"\1\n\n", text)


def matching_open_parenthesis(masked, closing):
    """Return the opening parenthesis paired with one closing parenthesis."""
    depth = 0
    for position in range(closing, -1, -1):
        if masked[position] == ")":
            depth += 1
        elif masked[position] == "(":
            depth -= 1
            if depth == 0:
                return position
    return -1


def brace_controlled_results(text):
    """Keep expanded call-and-return pairs inside an unbraced decision."""
    masked = STRUCTURE.mask_c(text)
    operations = []
    pattern = re.compile(
        r"(?m)^([ \t]*)(function_result(?:_[0-9]+)?)\s*=.*?;",
        re.S)
    for match in pattern.finditer(masked):
        assignment_start = match.start()
        assignment_end = match.end()
        result_name = match.group(2)
        position = assignment_start - 1
        while position >= 0 and masked[position].isspace():
            position -= 1
        if position < 0 or masked[position] != ")":
            continue
        closing = position
        opening = matching_open_parenthesis(masked, closing)
        if opening < 0:
            continue
        probe = opening - 1
        while probe >= 0 and masked[probe].isspace():
            probe -= 1
        word_end = probe + 1
        while probe >= 0 and (masked[probe].isalnum() or masked[probe] == "_"):
            probe -= 1
        if masked[probe + 1:word_end] != "if":
            continue
        if masked[closing + 1:assignment_start].strip():
            continue

        return_match = re.match(
            r"\s*return\s+" + re.escape(result_name) + r"\s*;",
            masked[assignment_end:])
        if return_match is None:
            continue
        return_end = assignment_end + return_match.end()
        if_line_start = text.rfind("\n", 0, probe + 1) + 1
        indentation = re.match(
            r"[ \t]*", text[if_line_start:probe + 1]).group(0)
        operations.append((closing + 1, closing + 1, " {"))
        operations.append((return_end, return_end, "\n" + indentation + "}"))
    for start, end, replacement in sorted(operations, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, len(operations) // 2


def refactor(text):
    """Apply the deterministic layout passes to one implementation."""
    counts = []
    text, count = move_loop_comments(text)
    counts.append(count)
    text, count = direct_call_returns(text)
    counts.append(count)
    text, count = compound_call_returns(text)
    counts.append(count)
    text, _ = brace_controlled_results(text)
    text = normalize_result_declaration_gaps(text)
    text = re.sub(r"\{\n\n([ \t]*/\*)", r"{\n\1", text)
    text, count = add_if_comments(text)
    counts.append(count)
    text, _ = refresh_generated_if_comments(text)
    text, count = add_return_comments(text)
    counts.append(count)
    text, count = comment_blank_lines(text)
    counts.append(count)
    return text, counts


def main():
    """Apply or dry-run the semantic-layout transformer."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expected-c", type=int, default=214)
    arguments = parser.parse_args()
    paths = sorted(pathlib.Path(arguments.root).rglob("*.c"))
    if len(paths) != arguments.expected_c:
        raise SystemExit(
            f"C inventory is {len(paths)}, expected {arguments.expected_c}")

    totals = [0, 0, 0, 0, 0, 0]
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
        f"USERLAND-SEMANTIC-LAYOUT: {action} {changed} files; "
        f"moved {totals[0]} loop comments, expanded {totals[1]} call returns, "
        f"expanded {totals[2]} compound call returns, "
        f"added {totals[3]} if comments, added {totals[4]} return comments, "
        f"added {totals[5]} paragraph gaps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
