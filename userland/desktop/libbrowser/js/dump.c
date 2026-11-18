/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The syntax tree as text, for the tests and for debugging
 * (`browser --dump=ast FILE.js`): one node a line, indented by
 * its depth, with its kind, operator, flags, text or number.
 */

#include "js/internal.h"

#include <string.h>

/* The deepest node written (the parser caps nesting at a smaller depth anyway). */
#define DUMP_DEPTH_MAX		2000

/* The names of the node kinds, in the order of enum js_node_kind. */
static const char *const dump_kinds[JS_NODE_KINDS] = {
	"Program", "Variables", "Declarator", "FunctionDeclaration", "ClassDeclaration", "Block", "Empty",
	"ExpressionStatement", "If", "For", "ForIn", "ForOf", "While", "DoWhile", "Continue", "Break", "Return", "With",
	"Switch", "Case", "Labeled", "Throw", "Try", "Debugger", "Import", "ImportSpecifier", "Export", "ExportSpecifier",
	"Identifier", "PrivateName", "Number", "BigInt", "String", "Template", "TemplateString", "TaggedTemplate", "RegExp",
	"Null", "True", "False", "This", "Super", "Array", "Hole", "Object", "Property", "Function", "Class", "Method",
	"Field", "StaticBlock", "Unary", "Update", "Binary", "Logical", "Assign", "Conditional", "Call", "New", "Member",
	"OptionalChain", "Sequence", "Spread", "Yield", "Await", "MetaProperty", "ImportCall", "Parenthesized",
	"ArrayPattern", "ObjectPattern", "AssignmentPattern", "Rest"
};

/* The spellings of the operators, in the order of enum js_punctuator. */
static const char *const dump_operators[JS_PUNCTUATORS] = {
	"", "{", "}", "(", ")", "[", "]", ".", "...", ";", ",", "<", ">", "<=", ">=", "==", "!=", "===", "!==",
	"+", "-", "*", "/", "%", "**", "++", "--", "<<", ">>", ">>>", "&", "|", "^", "!", "~", "&&", "||", "??",
	"?", "?.", ":", "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=",
	"||=", "?\?=", "=>", "typeof", "void", "delete", "in", "instanceof", "var", "let", "const", "using"
};

/* The names of the property kinds. */
static const char *const dump_property_kinds[] = {
	"init", "get", "set", "method", "spread", "constructor"
};

/*
 * One flag's name.
 */
struct dump_flag {
	uint32_t flag;
	const char *name;
};

/* The flags' names. */
static const struct dump_flag dump_flags[] = {
	{ JS_FLAG_STRICT, "strict" }, { JS_FLAG_MODULE, "module" }, { JS_FLAG_AWAIT, "await" }, { JS_FLAG_ASYNC, "async" },
	{ JS_FLAG_GENERATOR, "generator" }, { JS_FLAG_ARROW, "arrow" }, { JS_FLAG_EXPRESSION_BODY, "expression" },
	{ JS_FLAG_COMPUTED, "computed" }, { JS_FLAG_SHORTHAND, "shorthand" }, { JS_FLAG_STATIC, "static" },
	{ JS_FLAG_OPTIONAL, "optional" }, { JS_FLAG_PREFIX, "prefix" }, { JS_FLAG_DELEGATE, "delegate" },
	{ JS_FLAG_DEFAULT, "default" }, { JS_FLAG_ALL, "all" }, { JS_FLAG_INVALID_COOKED, "invalid" },
	{ JS_FLAG_METHOD, "method" }, { 0, NULL }
};

static int dump_node(const struct js_node *node, int depth, struct wb_buffer *out);
static int dump_list(const struct js_node *list, int depth, struct wb_buffer *out);
static int dump_text(const uint16_t *text, size_t length, struct wb_buffer *out);

/*
 * Writes a tree (from its root, or any node) as text.
 */
int
js_dump(
	const struct js_node *node,
	struct wb_buffer *out)
{
	int error;

	/* The node and everything under it. */
	error = dump_node(node, 0, out);
	if (error != 0)
		return error;

	/* Succeeded: the tree is written. */
	return 0;
}

/*
 * Reports a node kind's name.
 */
const char *
js_node_kind_name(
	int kind)
{
	/* A kind outside the table. */
	if (kind < 0 || kind >= JS_NODE_KINDS)
		return "?";

	/* The name. */
	return dump_kinds[kind];
}

/* Writes a node on its line and its children below it. */
static int
dump_node(
	const struct js_node *node,
	int depth,
	struct wb_buffer *out)
{
	const struct dump_flag *flag;
	int level;
	int error;

	/* Too deep a tree is cut short. */
	if (depth > DUMP_DEPTH_MAX)
		return 0;

	/* The indentation and the kind. */
	error = 0;
	for (level = 0; error == 0 && level < depth; level++)
		error = wb_buffer_append_string(out, "  ");
	if (error == 0)
		error = wb_buffer_append_string(out, js_node_kind_name(node->kind));

	/* The operator: a property's kind, or a punctuator's spelling. */
	if (error == 0 && (node->kind == JS_NODE_PROPERTY || node->kind == JS_NODE_METHOD) && node->op >= 0 && node->op <= JS_PROPERTY_CONSTRUCTOR)
		error = wb_buffer_printf(out, " %s", dump_property_kinds[node->op]);
	if (error == 0 && node->kind != JS_NODE_PROPERTY && node->kind != JS_NODE_METHOD && node->op > 0 && node->op < JS_PUNCTUATORS)
		error = wb_buffer_printf(out, " %s", dump_operators[node->op]);

	/* The flags. */
	for (flag = dump_flags; error == 0 && flag->name != NULL; flag++) {
		if ((node->flags & flag->flag) != 0U)
			error = wb_buffer_printf(out, " %s", flag->name);
	}

	/* A number's value; a text in quotes; a raw text (a regular expression's flags) after it. */
	if (error == 0 && node->kind == JS_NODE_NUMBER)
		error = wb_buffer_printf(out, " %.17g", node->number);
	if (error == 0 && node->text != NULL && node->kind != JS_NODE_NUMBER) {
		error = wb_buffer_append_string(out, " \"");
		if (error == 0)
			error = dump_text(node->text, node->text_length, out);
		if (error == 0)
			error = wb_buffer_append_string(out, "\"");
	}

	/* A regular expression's flags after its pattern. */
	if (error == 0 && node->raw != NULL && node->kind == JS_NODE_REGEXP) {
		error = wb_buffer_append_string(out, " /");
		if (error == 0)
			error = dump_text(node->raw, node->raw_length, out);
	}

	/* The line's end. */
	if (error == 0)
		error = wb_buffer_append_string(out, "\n");

	/* The children, each a list. */
	if (error == 0 && node->first != NULL)
		error = dump_list(node->first, depth + 1, out);
	if (error == 0 && node->second != NULL)
		error = dump_list(node->second, depth + 1, out);
	if (error == 0 && node->third != NULL)
		error = dump_list(node->third, depth + 1, out);
	if (error == 0 && node->fourth != NULL)
		error = dump_list(node->fourth, depth + 1, out);

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the node is written. */
	return 0;
}

/* Writes each node of a list. */
static int
dump_list(
	const struct js_node *list,
	int depth,
	struct wb_buffer *out)
{
	int error;

	/* Each member in order. */
	for (; list != NULL; list = list->next) {
		error = dump_node(list, depth, out);
		if (error != 0)
			return error;
	}

	/* Succeeded: the list is written. */
	return 0;
}

/* Writes a text as UTF-8, with quotes, backslashes and line breaks escaped. */
static int
dump_text(
	const uint16_t *text,
	size_t length,
	struct wb_buffer *out)
{
	size_t index;
	int error;

	/* Each unit: the escapes, the rest as UTF-8. */
	error = 0;
	for (index = 0; error == 0 && index < length; index++) {
		if (text[index] == '"' || text[index] == '\\')
			error = wb_buffer_printf(out, "\\%c", (char)text[index]);
		else if (text[index] == '\n')
			error = wb_buffer_append_string(out, "\\n");
		else if (text[index] < 0x20U)
			error = wb_buffer_printf(out, "\\x%02x", (unsigned)text[index]);
		else
			error = wb_units_to_utf8(&text[index], 1, out);
	}

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}
