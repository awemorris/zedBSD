#!/usr/bin/env python3
"""Apply mechanically safe ANSI C declaration-layout rewrites."""

import argparse
import importlib.util
import pathlib
import re
import sys
from types import SimpleNamespace


SCRIPT_DIRECTORY = pathlib.Path(__file__).parent


def load_module(name, filename):
	"""Loads one neighboring transformation helper."""
	specification = importlib.util.spec_from_file_location(
		name, SCRIPT_DIRECTORY / filename)
	module = importlib.util.module_from_spec(specification)
	sys.modules[specification.name] = module
	specification.loader.exec_module(module)
	return module


STRUCTURE = load_module("userland_c_structure_ansi", "userland-c-structure.py")
DECLARATIONS = load_module(
	"userland_c_declarations_ansi", "refactor-userland-declarations.py")


def closing_parenthesis(masked, opening):
	"""Returns the closing parenthesis for one control expression."""
	return STRUCTURE.matching(masked, opening, "(", ")")


def first_for_semicolon(masked, opening, closing):
	"""Returns the first top-level semicolon in a for control expression."""
	round_depth = 0
	square_depth = 0
	brace_depth = 0
	for position in range(opening + 1, closing):
		character = masked[position]
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
			return position
	return -1


def skip_space(masked, position, limit):
	"""Skips masked whitespace without crossing a containing function."""
	while position < limit and masked[position].isspace():
		position += 1
	return position


def statement_end(masked, start, limit):
	"""Returns the end of one C statement, including nested control bodies."""
	start = skip_space(masked, start, limit)
	if start >= limit:
		return limit
	if masked[start] == "{":
		closing = STRUCTURE.matching(masked, start, "{", "}")
		return limit if closing < 0 else closing + 1

	control = re.match(r"(?:if|for|while|switch)\b", masked[start:limit])
	if control is not None:
		opening = masked.find("(", start + control.end(), limit)
		if opening < 0:
			return limit
		closing = closing_parenthesis(masked, opening)
		if closing < 0:
			return limit
		end = statement_end(masked, closing + 1, limit)
		if control.group(0) == "if":
			probe = skip_space(masked, end, limit)
			if masked.startswith("else", probe):
				end = statement_end(masked, probe + 4, limit)
		return end

	if masked.startswith("do", start) and not re.match(
			r"do[A-Za-z0-9_]", masked[start:limit]):
		end = statement_end(masked, start + 2, limit)
		probe = skip_space(masked, end, limit)
		match = re.match(r"while\s*\(", masked[probe:limit])
		if match is not None:
			opening = masked.find("(", probe, limit)
			closing = closing_parenthesis(masked, opening)
			semicolon = masked.find(";", closing + 1, limit)
			return limit if semicolon < 0 else semicolon + 1
		return end

	round_depth = 0
	square_depth = 0
	for position in range(start, limit):
		character = masked[position]
		if character == "(":
			round_depth += 1
		elif character == ")" and round_depth:
			round_depth -= 1
		elif character == "[":
			square_depth += 1
		elif character == "]" and square_depth:
			square_depth -= 1
		elif character == ";" and round_depth == 0 and square_depth == 0:
			return position + 1
	return limit


def unique_name(name, identifiers, serial):
	"""Builds one readable function-unique loop variable name."""
	base = name if name not in {"i", "j", "n", "p"} else name + "_index"
	candidate = base + "_for"
	while candidate in identifiers:
		serial += 1
		candidate = base + "_for" + str(serial)
	identifiers.add(candidate)
	return candidate, serial


def renamed(source, names):
	"""Renames identifier tokens in one declaration or expression."""
	return re.sub(
		r"\b[A-Za-z_][A-Za-z0-9_]*\b",
		lambda match: names.get(match.group(0), match.group(0)), source)


def declarator_names(statement):
	"""Returns plainly declared object names from one local declaration."""
	source = statement.strip()
	if source.endswith(";"):
		source = source[:-1]
	masked = STRUCTURE.mask_c(source)
	commas = DECLARATIONS.top_level_positions(masked, {","})
	boundaries = [-1] + commas + [len(source)]
	names = []
	for index in range(len(boundaries) - 1):
		part = source[boundaries[index] + 1:boundaries[index + 1]]
		part_masked = STRUCTURE.mask_c(part)
		equals = DECLARATIONS.top_level_positions(part_masked, {"="})
		left = part[:equals[0]] if equals else part
		function_pointer = re.search(
			r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", left)
		if function_pointer is not None:
			names.append(function_pointer.group(1))
			continue
		array = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\[", left)
		if array is not None:
			names.append(array.group(1))
			continue
		identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", left)
		if identifiers:
			names.append(identifiers[-1])
	return names


def movable_declaration(statement):
	"""Returns a lifted declaration and original-point assignments when safe."""
	masked = STRUCTURE.mask_c(statement)
	if "/*" in statement or "//" in statement or "#" in statement:
		return None
	if re.search(r"\bextern\b", masked):
		return None
	if re.search(r"\bstatic\b", masked):
		return statement.strip(), []
	left = masked.split("=", 1)[0]
	if "[" in left and "=" in masked:
		return None
	if "{" in masked:
		return None
	for dimension in re.findall(r"\[([^\]]*)\]", left):
		if re.search(r"\b[a-z_][A-Za-z0-9_]*\b", dimension):
			return None
	return DECLARATIONS.split_initialized_declaration(statement)


def member_or_tag(masked, function_start, position):
	"""Returns true for a member name or a structure/union/enum tag."""
	before = masked[max(function_start, position - 24):position]
	if before.endswith(".") or before.endswith("->"):
		return True
	return bool(re.search(r"\b(?:struct|union|enum)\s*$", before))


def renamed_c(source, names):
	"""Renames C identifiers while preserving member names and type tags."""
	masked = STRUCTURE.mask_c(source)
	operations = []
	for token in re.finditer(r"\b[A-Za-z_][A-Za-z0-9_]*\b", masked):
		name = token.group(0)
		if name not in names or member_or_tag(masked, 0, token.start()):
			continue
		operations.append((token.start(), token.end(), names[name]))
	for start, end, replacement in reversed(operations):
		source = source[:start] + replacement + source[end:]
	return source


def unique_local_name(name, identifiers, serial):
	"""Returns a readable unique name for one lifted shadowed object."""
	base = name + "_local"
	candidate = base
	while candidate in identifiers:
		serial += 1
		candidate = base + str(serial)
	identifiers.add(candidate)
	return candidate, serial


def compound_block(masked, function, opening):
	"""Recognizes a function/control body or an explicit standalone block."""
	if opening == function.open_brace:
		return True
	position = opening - 1
	while position > function.open_brace and masked[position].isspace():
		position -= 1
	if masked[position] == ")":
		depth = 1
		position -= 1
		while position > function.open_brace and depth:
			if masked[position] == ")":
				depth += 1
			elif masked[position] == "(":
				depth -= 1
			position -= 1
		prefix = masked[max(function.open_brace, position - 24):position + 1]
		return bool(re.search(r"\b(?:if|for|while|switch)\s*$", prefix))
	prefix = masked[max(function.open_brace, position - 24):position + 1]
	if re.search(r"\b(?:else|do)\s*$", prefix):
		return True
	return masked[position] in ";:{}"


def controlled_block(masked, function, opening):
	"""Returns true for a function or control-statement body."""
	if opening == function.open_brace:
		return True
	position = opening - 1
	while position > function.open_brace and masked[position].isspace():
		position -= 1
	if masked[position] == ")":
		depth = 1
		position -= 1
		while position > function.open_brace and depth:
			if masked[position] == ")":
				depth += 1
			elif masked[position] == "(":
				depth -= 1
			position -= 1
		prefix = masked[max(function.open_brace, position - 24):position + 1]
		return bool(re.search(r"\b(?:if|for|while|switch)\s*$", prefix))
	prefix = masked[max(function.open_brace, position - 24):position + 1]
	return bool(re.search(r"\b(?:else|do)\s*$", prefix))


def refactor_scope_only_blocks(text):
	"""Removes declaration-free standalone compound-statement braces."""
	masked = STRUCTURE.mask_c(text)
	blocks = []
	removed = 0
	for function in STRUCTURE.parse_functions(text):
		for opening, closing in DECLARATIONS.matching_braces(
				masked, function.open_brace, function.close_brace):
			if controlled_block(masked, function, opening):
				continue
			opening_start = text.rfind("\n", 0, opening) + 1
			opening_end = text.find("\n", opening)
			closing_start = text.rfind("\n", 0, closing) + 1
			closing_end = text.find("\n", closing)
			if opening_end < 0:
				opening_end = len(text)
			else:
				opening_end += 1
			if closing_end < 0:
				closing_end = len(text)
			else:
				closing_end += 1
			opening_line = text[opening_start:opening_end]
			closing_line = text[closing_start:closing_end]
			opening_text = opening_line.strip()
			closing_match = re.fullmatch(r"}\s*(break;)?", closing_line.strip())
			if opening_text == "{":
				opening_replacement = ""
			elif ("=" not in opening_text and opening_text.endswith("{") and
					(":" in opening_text or opening_text.startswith("/*"))):
				opening_replacement = opening_line[:opening_line.rfind("{")].rstrip()
				opening_replacement += "\n" if opening_line.endswith("\n") else ""
			else:
				continue
			if closing_match is None:
				continue
			closing_replacement = ""
			if closing_match.group(1) is not None:
				indentation = closing_line[:len(closing_line) - len(closing_line.lstrip())]
				closing_replacement = indentation + closing_match.group(1)
				closing_replacement += "\n" if closing_line.endswith("\n") else ""
			blocks.append((
				opening_start, opening_end, closing_start, closing_end,
				opening_replacement, closing_replacement))
			removed += 1
	if not blocks:
		return text, 0

	line_replacements = {
		opening_start: opening_replacement
		for opening_start, opening_end, closing_start, closing_end,
		opening_replacement, closing_replacement in blocks}
	line_replacements.update({
		closing_start: closing_replacement
		for opening_start, opening_end, closing_start, closing_end,
		opening_replacement, closing_replacement in blocks})
	lines = []
	position = 0
	for original_line in text.splitlines(keepends=True):
		line = original_line
		if position in line_replacements:
			lines.append(line_replacements[position])
			position += len(original_line)
			continue
		depth = sum(
			opening_end <= position < closing_start
			for opening_start, opening_end, closing_start, closing_end,
			opening_replacement, closing_replacement
			in blocks)
		while depth > 0 and line.startswith("\t"):
			line = line[1:]
			depth -= 1
		lines.append(line)
		position += len(original_line)
	return "".join(lines), removed


def refactor_declaration_spacing(text, types):
	"""Places one blank line after each function declaration group."""
	operations = []
	changed = 0
	for function in STRUCTURE.parse_functions(text):
		declarations = DECLARATIONS.leading_declarations(text, function, types)
		if not declarations:
			continue
		start = declarations[-1][1]
		end = start
		while end < function.close_brace and text[end] in " \t\r\n":
			end += 1
		whitespace = text[start:end]
		indentation = whitespace[whitespace.rfind("\n") + 1:]
		replacement = "\n\n" + indentation
		if whitespace == replacement:
			continue
		operations.append((start, end, replacement))
		changed += 1
	for start, end, replacement in sorted(operations, reverse=True):
		text = text[:start] + replacement + text[end:]
	return text, changed


def refactor_nested_declarations(text, types):
	"""Lifts collision-free nested declarations without renaming objects."""
	masked = STRUCTURE.mask_c(text)
	operations = []
	lifted_count = 0

	for function in STRUCTURE.parse_functions(text):
		function_block = SimpleNamespace(
			open_brace=function.open_brace, close_brace=function.close_brace)
		top = DECLARATIONS.leading_declarations(text, function_block, types)
		all_names = []
		for _, _, statement in top:
			all_names.extend(declarator_names(statement))

		candidates = []
		pairs = DECLARATIONS.matching_braces(
			masked, function.open_brace, function.close_brace)
		for opening, closing in pairs:
			if opening == function.open_brace or not compound_block(
					masked, function, opening):
				continue
			block = SimpleNamespace(open_brace=opening, close_brace=closing)
			for start, end, statement in DECLARATIONS.leading_declarations(
					text, block, types):
				names = declarator_names(statement)
				if not names:
					continue
				all_names.extend(names)
				candidates.append((start, end, statement, names))

		duplicates = {
			name for name in all_names if all_names.count(name) > 1}
		lifted = []
		for start, end, statement, names in candidates:
			if any(name in duplicates for name in names):
				continue
			result = movable_declaration(statement)
			if result is None:
				continue
			declaration, assignments = result
			lifted.append(declaration)
			indent_start = text.rfind("\n", 0, start) + 1
			indentation = text[indent_start:start]
			replacement = "\n".join(
				indentation + name + " = " + value + ";"
				for name, value in assignments)
			operations.append((start, end, replacement))
			lifted_count += 1

		if lifted:
			operations.append((
				function.open_brace + 1, function.open_brace + 1,
				"\n" + "\n".join("\t" + value for value in lifted)))

	for start, end, replacement in sorted(operations, reverse=True):
		text = text[:start] + replacement + text[end:]
	return text, lifted_count


def refactor_shadowed_nested_declarations(text, types):
	"""Lifts safe remaining nested declarations with scope-aware renaming."""
	masked = STRUCTURE.mask_c(text)
	operations = []
	lifted_count = 0

	for function in STRUCTURE.parse_functions(text):
		identifiers = set(re.findall(
			r"\b[A-Za-z_][A-Za-z0-9_]*\b",
			masked[function.open_brace:function.close_brace + 1]))
		records = []
		serial = 0
		for opening, closing in DECLARATIONS.matching_braces(
				masked, function.open_brace, function.close_brace):
			if opening == function.open_brace or not compound_block(
					masked, function, opening):
				continue
			block = SimpleNamespace(open_brace=opening, close_brace=closing)
			for start, end, statement in DECLARATIONS.leading_declarations(
					text, block, types):
				names = declarator_names(statement)
				if not names:
					continue
				result = movable_declaration(statement)
				mapping = {}
				if result is not None:
					for name in names:
						mapping[name], serial = unique_local_name(
							name, identifiers, serial)
				records.append({
					"start": start,
					"end": end,
					"scope_end": closing,
					"statement": statement,
					"names": set(names),
					"mapping": mapping,
					"result": result,
				})

		if not any(record["result"] is not None for record in records):
			continue
		record_names = set().union(*(record["names"] for record in records))

		def owner(name, position):
			owners = [
				record for record in records
				if (record["start"] <= position < record["scope_end"] and
					name in record["names"])]
			if not owners:
				return None
			return min(
				owners,
				key=lambda record: record["scope_end"] - record["start"])

		lifted = []
		for record in sorted(records, key=lambda item: item["start"]):
			if record["result"] is None:
				continue
			visible = {}
			for name in record_names:
				binding = owner(name, record["start"])
				if binding is not None and binding["result"] is not None:
					visible[name] = binding["mapping"][name]
			visible.update(record["mapping"])
			declaration, assignments = record["result"]
			lifted.append(renamed_c(declaration, visible))
			indent_start = text.rfind("\n", 0, record["start"]) + 1
			indentation = text[indent_start:record["start"]]
			replacement = "\n".join(
				indentation + record["mapping"][name] + " = " +
				renamed_c(value, visible) + ";"
				for name, value in assignments)
			operations.append((record["start"], record["end"], replacement))
			lifted_count += 1

		spans = [(record["start"], record["end"]) for record in records]
		for token in re.finditer(
				r"\b[A-Za-z_][A-Za-z0-9_]*\b",
				masked[function.open_brace:function.close_brace + 1]):
			position = function.open_brace + token.start()
			if any(start <= position < end for start, end in spans):
				continue
			if member_or_tag(masked, function.open_brace, position):
				continue
			binding = owner(token.group(0), position)
			if binding is None or binding["result"] is None:
				continue
			operations.append((
				position, position + len(token.group(0)),
				binding["mapping"][token.group(0)]))

		operations.append((
			function.open_brace + 1, function.open_brace + 1,
			"\n" + "\n".join("\t" + value for value in lifted)))

	for start, end, replacement in sorted(operations, reverse=True):
		text = text[:start] + replacement + text[end:]
	return text, lifted_count


def refactor_for_declarations(text, types):
	"""Moves declaration-form for variables to function declaration groups."""
	masked = STRUCTURE.mask_c(text)
	operations = []
	changed_loops = 0

	for function in STRUCTURE.parse_functions(text):
		loops = []
		identifiers = set(re.findall(
			r"\b[A-Za-z_][A-Za-z0-9_]*\b",
			masked[function.open_brace:function.close_brace + 1]))
		serial = 0
		for match in re.finditer(
				r"\bfor\s*\(",
				masked[function.open_brace:function.close_brace + 1]):
			start = function.open_brace + match.start()
			opening = masked.find("(", start, function.close_brace)
			closing = closing_parenthesis(masked, opening)
			if closing < 0:
				continue
			semicolon = first_for_semicolon(masked, opening, closing)
			if semicolon < 0:
				continue
			initializer = text[opening + 1:semicolon]
			if not DECLARATIONS.is_declaration(initializer + ";", types):
				continue
			result = DECLARATIONS.split_initialized_declaration(initializer + ";")
			if result is None or not result[1]:
				continue
			declaration, assignments = result
			names = {}
			for name, _ in assignments:
				candidate, serial = unique_name(name, identifiers, serial)
				names[name] = candidate
			body_end = statement_end(masked, closing + 1, function.close_brace)
			loops.append({
				"opening": opening,
				"semicolon": semicolon,
				"scope_start": opening + 1,
				"scope_end": body_end,
				"declaration": renamed(declaration, names),
				"assignments": assignments,
				"names": names,
			})

		if not loops:
			continue

		declarations = []
		for loop in loops:
			declarations.append(loop["declaration"])
			assignment = ", ".join(
				loop["names"][name] + " = " + renamed(value, loop["names"])
				for name, value in loop["assignments"])
			operations.append((
				loop["opening"] + 1, loop["semicolon"], assignment))

		initializer_spans = [
			(loop["opening"] + 1, loop["semicolon"]) for loop in loops]
		for token in re.finditer(
				r"\b[A-Za-z_][A-Za-z0-9_]*\b",
				masked[function.open_brace:function.close_brace + 1]):
			position = function.open_brace + token.start()
			if any(start <= position < end for start, end in initializer_spans):
				continue
			before = masked[max(function.open_brace, position - 16):position]
			if before.endswith(".") or before.endswith("->"):
				continue
			if re.search(r"\b(?:struct|union|enum)\s*$", before):
				continue
			owners = [
				loop for loop in loops
				if (loop["scope_start"] <= position < loop["scope_end"] and
					token.group(0) in loop["names"])]
			if not owners:
				continue
			owner = min(
				owners, key=lambda loop: loop["scope_end"] - loop["scope_start"])
			operations.append((
				position, position + len(token.group(0)),
				owner["names"][token.group(0)]))

		insertion = function.open_brace + 1
		operations.append((
			insertion, insertion,
			"\n" + "\n".join("\t" + value for value in declarations)))
		changed_loops += len(loops)

	for start, end, replacement in sorted(operations, reverse=True):
		text = text[:start] + replacement + text[end:]
	return text, changed_loops


def main():
	"""Applies or audits the safe ANSI C transformations."""
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
	types = DECLARATIONS.typedef_names(root)
	changed = 0
	loops = 0
	lifted = 0
	blocks = 0
	spacing = 0
	for path in paths:
		original = path.read_text(encoding="utf-8", errors="surrogateescape")
		migrated, count = refactor_for_declarations(original, types)
		loops += count
		migrated, declaration_count = refactor_nested_declarations(
			migrated, types)
		lifted += declaration_count
		if path.as_posix().endswith("/base/rtld/rtld.c"):
			shadowed_count = 0
		else:
			migrated, shadowed_count = refactor_shadowed_nested_declarations(
				migrated, types)
		lifted += shadowed_count
		migrated, block_count = refactor_scope_only_blocks(migrated)
		blocks += block_count
		migrated, spacing_count = refactor_declaration_spacing(migrated, types)
		spacing += spacing_count
		if migrated != original:
			changed += 1
			if arguments.apply:
				path.write_text(
					migrated, encoding="utf-8", errors="surrogateescape",
					newline="\n")

	action = "changed" if arguments.apply else "would change"
	print(f"USERLAND-ANSI-C-MIGRATE: {action} {changed} files, "
	      f"rewrote {loops} declaration-form for loops, "
	      f"lifted {lifted} nested declarations, "
	      f"removed {blocks} scope-only blocks, "
	      f"normalized {spacing} declaration gaps")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
