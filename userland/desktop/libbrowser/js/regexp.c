/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The regular expression engine (ws074-p027).
 *
 * A pattern is parsed (ECMAScript's grammar, with Annex B's for a pattern
 * without the u flag: literal braces and brackets, legacy octal escapes,
 * identity escapes, \c without a letter, quantified lookaheads) into a tree
 * of nodes, which is compiled into a program of 32-bit words.  The matcher
 * runs the program with backtracking over an explicit stack, never the C
 * stack: a choice point saves where to go on failing, and every change to
 * a capture or a loop's counter is logged on the same stack so that
 * failing back past it puts the old value back.  A lookaround runs its body
 * above a barrier on the stack and cuts the body's choice points when it
 * succeeds (lookarounds are atomic); a lookbehind's body is compiled to
 * match backwards.  A loop whose body always consumes and holds no capture
 * is a plain choice; another keeps a counter, the position its iteration
 * started at (an iteration that matched nothing fails once the minimum is
 * reached) and resets the captures inside at each iteration.
 *
 * Case-insensitive matching compares Canonicalize: the one-unit uppercase
 * of a character without the u flag (never from beyond ASCII into it), and
 * a simple case folding from the simple mappings with it.  Not in this
 * pass: the v flag, Unicode property escapes (\p, \P), the modifiers
 * ((?i:...)) and duplicate group names.
 */

#include "js/regexp.h"
#include "base/base.h"
#include "base/unicode.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* An unbounded quantifier's maximum. */
#define REGEXP_INFINITY		UINT32_MAX

/* The deepest nesting of groups and lookarounds a pattern may have. */
#define REGEXP_DEPTH_MAX	400U

/* The most steps one match may take, and the most entries its stack may hold, before it gives up. */
#define REGEXP_STEPS_MAX	((uint64_t)400000000)
#define REGEXP_STACK_MAX	((size_t)1 << 25)

/* How many steps the matcher takes between two calls of the input's poll (the mask of a power of two). */
#define REGEXP_POLL_MASK	((uint64_t)0xFFFF)

/* The largest code point. */
#define REGEXP_CODE_POINT_MAX	0x10FFFFU

/*
 * The kinds of node of a parsed pattern.
 */
enum regexp_node_kind {
	REGEXP_NODE_EMPTY,
	REGEXP_NODE_CHAR,
	REGEXP_NODE_ANY,
	REGEXP_NODE_CLASS,
	REGEXP_NODE_SEQUENCE,
	REGEXP_NODE_ALTERNATION,
	REGEXP_NODE_GROUP,
	REGEXP_NODE_BACKREF,
	REGEXP_NODE_NAMED_BACKREF,
	REGEXP_NODE_LINE_START,
	REGEXP_NODE_LINE_END,
	REGEXP_NODE_WORD_BOUNDARY,
	REGEXP_NODE_NOT_WORD_BOUNDARY,
	REGEXP_NODE_LOOK,
	REGEXP_NODE_REPEAT
};

/* The kinds of lookaround (a LOOK node's value). */
#define REGEXP_LOOK_AHEAD	0U
#define REGEXP_LOOK_NOT_AHEAD	1U
#define REGEXP_LOOK_BEHIND	2U
#define REGEXP_LOOK_NOT_BEHIND	3U

/*
 * One node of a parsed pattern, in the parser's vector (nodes refer to
 * each other by index, since the vector moves as it grows): its first
 * child and next sibling (-1 for none), its value (the character, the
 * class, the capture or the lookaround's kind), a repetition's bounds and
 * greediness with the captures its body holds, and a named reference's
 * name in the parser's name units.
 */
struct regexp_node {
	int kind;
	int32_t first;
	int32_t next;
	uint32_t value;
	uint32_t min;
	uint32_t max;
	int greedy;
	uint32_t capture_first;
	uint32_t capture_end;
	uint32_t name_offset;
	uint32_t name_length;
};

/* A range of code points in a class, inclusive. */
struct regexp_range {
	uint32_t first;
	uint32_t last;
};

/* A character class: its sorted, merged ranges in the range table, and whether it is negated. */
struct regexp_class {
	uint32_t range_first;
	uint32_t range_count;
	int negated;
};

/* A named capture: its number and its name in the name units. */
struct regexp_name {
	uint32_t capture;
	uint32_t offset;
	uint32_t length;
};

/*
 * The instructions of a program.  The matching ones and BACKREF may carry
 * REGEXP_OP_BACKWARD (inside a lookbehind), which reads the character
 * before the position and moves it back.
 */
enum regexp_op {
	REGEXP_OP_CHAR,			/* c */
	REGEXP_OP_CHAR_FOLD,		/* canonical c */
	REGEXP_OP_ANY,			/* - (not a line terminator) */
	REGEXP_OP_ANY_ALL,		/* - */
	REGEXP_OP_CLASS,		/* class */
	REGEXP_OP_CLASS_FOLD,		/* class */
	REGEXP_OP_LINE_START,		/* - */
	REGEXP_OP_LINE_END,		/* - */
	REGEXP_OP_WORD_BOUNDARY,	/* - */
	REGEXP_OP_NOT_WORD_BOUNDARY,	/* - */
	REGEXP_OP_SPLIT,		/* go on at, on failure at */
	REGEXP_OP_JUMP,			/* at */
	REGEXP_OP_SAVE,			/* slot */
	REGEXP_OP_BACKREF,		/* capture */
	REGEXP_OP_LOOK,			/* kind, end */
	REGEXP_OP_LOOK_END,		/* - */
	REGEXP_OP_REPEAT_INIT,		/* counter */
	REGEXP_OP_REPEAT,		/* counter, min, max, greedy, exit */
	REGEXP_OP_REPEAT_ENTER,		/* counter, first slot, end slot */
	REGEXP_OP_REPEAT_NEXT,		/* counter, min, head */
	REGEXP_OP_MATCH			/* - */
};

/* The direction bit of an instruction, and the mask of its operation. */
#define REGEXP_OP_BACKWARD	0x100U
#define REGEXP_OP_MASK		0xFFU

/*
 * A compiled pattern: its flags, its code, its classes and their ranges,
 * how many captures it has (the whole match is capture 0) and loops with
 * counters, its capture names, and the code unit every match starts with
 * (-1 when there is none to look for).
 */
struct js_regexp_program {
	unsigned flags;
	uint32_t *code;
	size_t code_length;
	struct regexp_class *classes;
	size_t class_count;
	struct regexp_range *ranges;
	size_t range_count;
	uint32_t capture_count;
	uint32_t counter_count;
	struct regexp_name *names;
	size_t name_count;
	uint16_t *name_units;
	int32_t first_unit;
};

/*
 * The parser of a pattern: the pattern and where it has got to, the flags
 * (unicode says whether the pattern is read as code points), whether the
 * pattern has a named group (Annex B's \k), the number of captures a
 * pre-scan counted and the number opened so far, how deep the groups are,
 * the nodes, classes, ranges and names made so far, and the message of a
 * syntax error.
 */
struct regexp_parser {
	const uint16_t *pattern;
	size_t length;
	size_t position;
	unsigned flags;
	int unicode;
	int named;
	uint32_t capture_total;
	uint32_t capture_count;
	unsigned depth;
	struct wb_vector nodes;
	struct wb_vector classes;
	struct wb_vector ranges;
	struct wb_vector names;
	struct wb_units name_units;
	const char *message;
};

/* The compiler of the parsed nodes: the parser, the code and the loops with counters so far. */
struct regexp_compiler {
	const struct regexp_parser *parser;
	struct wb_vector code;
	uint32_t counters;
};

/* The kinds of entry on the matcher's stack. */
enum regexp_frame_kind {
	REGEXP_FRAME_CHOICE,
	REGEXP_FRAME_SLOT,
	REGEXP_FRAME_COUNTER,
	REGEXP_FRAME_START,
	REGEXP_FRAME_BARRIER
};

/*
 * One entry on the matcher's stack: a choice point (where to go on and at
 * which position), an old value of a capture slot, a counter or a loop's
 * start (which one, and the value), or a lookaround's barrier (the kind,
 * where to go on after it and the position it started at).
 */
struct regexp_frame {
	uint32_t kind;
	uint32_t index;
	uint32_t extra;
	size_t value;
};

/*
 * The state of one match: the program and the input, the capture slots
 * (two per capture), the loops' counters and start positions, the stack,
 * where the program is and the position in the input, and the steps
 * taken.
 */
struct regexp_matcher {
	const struct js_regexp_program *program;
	const struct js_regexp_input *input;
	size_t *slots;
	size_t *counters;
	size_t *starts;
	struct regexp_frame *stack;
	size_t depth;
	size_t capacity;
	uint32_t pc;
	size_t position;
	uint64_t steps;
};

/* What one step of the matcher came to. */
enum regexp_outcome {
	REGEXP_GO,
	REGEXP_FAIL,
	REGEXP_MATCHED
};

static void regexp_prescan(struct regexp_parser *parser);
static int regexp_fail(struct regexp_parser *parser, const char *message);
static int32_t regexp_unit(const struct regexp_parser *parser, size_t offset);
static uint32_t regexp_peek(const struct regexp_parser *parser, size_t *size);
static int regexp_node_new(struct regexp_parser *parser, int kind, int32_t *index);
static struct regexp_node *regexp_node(const struct regexp_parser *parser, int32_t index);
static int regexp_parse_disjunction(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_alternative(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_term(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_quantifier(struct regexp_parser *parser, int32_t atom, uint32_t capture_first, int32_t *result);
static int regexp_parse_braces(struct regexp_parser *parser, uint32_t *min, uint32_t *max, int *valid);
static int regexp_parse_atom(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_group(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_look(struct regexp_parser *parser, uint32_t kind, int32_t *result);
static int regexp_parse_atom_escape(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_group_name(struct regexp_parser *parser, uint32_t *offset, uint32_t *length);
static int regexp_parse_character_escape(struct regexp_parser *parser, int in_class, uint32_t *code_point);
static int regexp_parse_unicode_escape(struct regexp_parser *parser, uint32_t *code_point, int *valid);
static int regexp_parse_hex(struct regexp_parser *parser, unsigned digits, uint32_t *value);
static uint32_t regexp_parse_legacy_octal(struct regexp_parser *parser);
static int regexp_parse_class(struct regexp_parser *parser, int32_t *result);
static int regexp_parse_class_atom(struct regexp_parser *parser, uint32_t *code_point, int *set);
static int regexp_char_node(struct regexp_parser *parser, uint32_t code_point, int32_t *result);
static int regexp_set_node(struct regexp_parser *parser, int letter, int32_t *result);
static int regexp_add_range(struct regexp_parser *parser, uint32_t first, uint32_t last);
static int regexp_add_set(struct regexp_parser *parser, int letter);
static void regexp_finish_class(struct regexp_parser *parser, uint32_t range_first);
static int regexp_compare_ranges(const void *left, const void *right);
static int regexp_resolve_names(struct regexp_parser *parser);
static int regexp_emit(struct regexp_compiler *compiler, uint32_t word);
static uint32_t regexp_here(const struct regexp_compiler *compiler);
static void regexp_patch(struct regexp_compiler *compiler, uint32_t at, uint32_t value);
static int regexp_compile_node(struct regexp_compiler *compiler, int32_t index, int backward);
static int regexp_compile_sequence(struct regexp_compiler *compiler, int32_t first, int backward);
static int regexp_compile_alternation(struct regexp_compiler *compiler, int32_t first, int backward);
static int regexp_compile_repeat(struct regexp_compiler *compiler, const struct regexp_node *node, int backward);
static int regexp_can_be_empty(const struct regexp_parser *parser, int32_t index);
static int regexp_is_word_char(const struct js_regexp_program *program, uint32_t code_point);
static int regexp_is_line_terminator(uint32_t code_point);
static uint32_t regexp_canonicalize(uint32_t code_point, int unicode);
static uint32_t regexp_simple_map(const struct wb_case_simple *table, size_t count, uint32_t code_point);
static int regexp_class_has(const struct js_regexp_program *program, uint32_t class_index, uint32_t code_point);
static int regexp_class_matches(const struct js_regexp_program *program, uint32_t class_index, uint32_t code_point, int fold);
static uint32_t regexp_input_unit(const struct js_regexp_input *input, size_t index);
static int regexp_read(const struct regexp_matcher *matcher, int backward, uint32_t *code_point, size_t *size);
static int regexp_push(struct regexp_matcher *matcher, uint32_t kind, uint32_t index, uint32_t extra, size_t value);
static int regexp_set_slot(struct regexp_matcher *matcher, uint32_t slot, size_t value);
static int regexp_step(struct regexp_matcher *matcher, int *outcome);
static int regexp_step_repeat(struct regexp_matcher *matcher, const uint32_t *words, int *outcome);
static int regexp_step_backref(struct regexp_matcher *matcher, const uint32_t *words, int *outcome);
static void regexp_look_end(struct regexp_matcher *matcher, int *outcome);
static int regexp_backtrack(struct regexp_matcher *matcher);
static int regexp_attempt(struct regexp_matcher *matcher, size_t start, int *matched);

/* The operand counts of the instructions, in the order of enum regexp_op. */
static const unsigned regexp_operands[] = {
	1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 2, 1, 1, 1, 2, 0, 1, 5, 3, 3, 0
};

/* The ranges of \d. */
static const struct regexp_range regexp_digits[] = {
	{ 0x30, 0x39 }
};

/* The ranges of \s: WhiteSpace and LineTerminator. */
static const struct regexp_range regexp_spaces[] = {
	{ 0x09, 0x0D },
	{ 0x20, 0x20 },
	{ 0xA0, 0xA0 },
	{ 0x1680, 0x1680 },
	{ 0x2000, 0x200A },
	{ 0x2028, 0x2029 },
	{ 0x202F, 0x202F },
	{ 0x205F, 0x205F },
	{ 0x3000, 0x3000 },
	{ 0xFEFF, 0xFEFF }
};

/* The ranges of \w (under u and i, ſ and the Kelvin sign too, which fold into it). */
static const struct regexp_range regexp_words[] = {
	{ 0x30, 0x39 },
	{ 0x41, 0x5A },
	{ 0x5F, 0x5F },
	{ 0x61, 0x7A }
};

/*
 * Reads a regular expression's flags; EINVAL for a character that is not
 * a flag, a flag given twice, or u with v.
 */
int
js_regexp_parse_flags(
	const uint16_t *text,
	size_t length,
	unsigned *flags)
{
	size_t index;
	unsigned flag;
	unsigned seen;

	/* Each character names one flag. */
	seen = 0;
	for (index = 0; index < length; index++) {
		/* Which flag the character is. */
		switch (text[index]) {
		case 'd':
			flag = JS_REGEXP_HAS_INDICES;
			break;
		case 'g':
			flag = JS_REGEXP_GLOBAL;
			break;
		case 'i':
			flag = JS_REGEXP_IGNORE_CASE;
			break;
		case 'm':
			flag = JS_REGEXP_MULTILINE;
			break;
		case 's':
			flag = JS_REGEXP_DOT_ALL;
			break;
		case 'u':
			flag = JS_REGEXP_UNICODE;
			break;
		case 'v':
			flag = JS_REGEXP_UNICODE_SETS;
			break;
		case 'y':
			flag = JS_REGEXP_STICKY;
			break;
		default:
			return EINVAL;
		}

		/* A flag may be given once. */
		if ((seen & flag) != 0)
			return EINVAL;
		seen |= flag;
	}

	/* u and v exclude each other. */
	if ((seen & JS_REGEXP_UNICODE) != 0 && (seen & JS_REGEXP_UNICODE_SETS) != 0)
		return EINVAL;

	/* Succeeded: the flags. */
	*flags = seen;
	return 0;
}

/*
 * Compiles a pattern with its flags into a program; EINVAL with a message
 * for a syntax error, ENOMEM when memory ran out.
 */
int
js_regexp_compile(
	const uint16_t *pattern,
	size_t length,
	unsigned flags,
	struct js_regexp_program **program,
	const char **message)
{
	struct regexp_parser parser;
	struct regexp_compiler compiler;
	struct js_regexp_program *made;
	int32_t root;
	uint32_t *first;
	int error;

	/* The v flag's class syntax is not in this pass. */
	*message = NULL;
	if ((flags & JS_REGEXP_UNICODE_SETS) != 0) {
		*message = "the v flag is not supported";
		return EINVAL;
	}

	/* The parser, and the captures the pattern has (a \N is a reference only when there are that many). */
	memset(&parser, 0, sizeof(parser));
	parser.pattern = pattern;
	parser.length = length;
	parser.flags = flags;
	parser.unicode = (flags & JS_REGEXP_UNICODE) != 0;
	parser.capture_count = 1;
	wb_vector_init(&parser.nodes, sizeof(struct regexp_node));
	wb_vector_init(&parser.classes, sizeof(struct regexp_class));
	wb_vector_init(&parser.ranges, sizeof(struct regexp_range));
	wb_vector_init(&parser.names, sizeof(struct regexp_name));
	wb_units_init(&parser.name_units);
	regexp_prescan(&parser);

	/* The whole pattern is one disjunction; a ) left over has no group to close. */
	error = regexp_parse_disjunction(&parser, &root);
	if (error == 0 && parser.position < parser.length)
		error = regexp_fail(&parser, "unmatched ')'");
	if (error == 0)
		error = regexp_resolve_names(&parser);

	/* The program: the tree's code, then the end of a match. */
	memset(&compiler, 0, sizeof(compiler));
	compiler.parser = &parser;
	wb_vector_init(&compiler.code, sizeof(uint32_t));
	if (error == 0)
		error = regexp_compile_node(&compiler, root, 0);
	if (error == 0)
		error = regexp_emit(&compiler, REGEXP_OP_MATCH);

	/* The program takes the code, classes, ranges and names over. */
	made = NULL;
	if (error == 0) {
		made = calloc(1, sizeof(*made));
		if (made == NULL)
			error = ENOMEM;
	}

	/* A failure leaves nothing behind: the parser's and the compiler's state go. */
	if (error != 0) {
		*message = parser.message;
		wb_vector_release(&compiler.code);
		wb_vector_release(&parser.nodes);
		wb_vector_release(&parser.classes);
		wb_vector_release(&parser.ranges);
		wb_vector_release(&parser.names);
		wb_units_release(&parser.name_units);
		return error;
	}

	/* Fills the program in; the parser's other state goes. */
	made->flags = flags;
	made->code = compiler.code.items;
	made->code_length = compiler.code.count;
	made->classes = parser.classes.items;
	made->class_count = parser.classes.count;
	made->ranges = parser.ranges.items;
	made->range_count = parser.ranges.count;
	made->capture_count = parser.capture_count;
	made->counter_count = compiler.counters;
	made->names = parser.names.items;
	made->name_count = parser.names.count;
	made->name_units = parser.name_units.data;
	wb_vector_release(&parser.nodes);

	/* A match that must start with one code unit (case-sensitively) is looked for by that unit. */
	made->first_unit = -1;
	first = made->code;
	if (made->code_length >= 2U && first[0] == REGEXP_OP_CHAR && first[1] <= 0xFFFFU) {
		if (!parser.unicode || first[1] < 0xD800U || first[1] > 0xDFFFU)
			made->first_unit = (int32_t)first[1];
	}

	/* Succeeded: the program. */
	*program = made;
	return 0;
}

/* Frees a program. */
void
js_regexp_free(
	struct js_regexp_program *program)
{
	/* Nothing to free. */
	if (program == NULL)
		return;

	/* Its tables, then itself. */
	free(program->code);
	free(program->classes);
	free(program->ranges);
	free(program->names);
	free(program->name_units);
	free(program);
}

/* Reports a program's flags. */
unsigned
js_regexp_flags(
	const struct js_regexp_program *program)
{
	/* The flags it was compiled with. */
	return program->flags;
}

/* Reports how many captures a program has, the whole match (capture 0) included. */
uint32_t
js_regexp_capture_count(
	const struct js_regexp_program *program)
{
	/* The groups and the whole match. */
	return program->capture_count;
}

/* Tells whether a program has named captures. */
int
js_regexp_has_names(
	const struct js_regexp_program *program)
{
	/* Some group has a name. */
	if (program->name_count != 0)
		return 1;

	/* None. */
	return 0;
}

/* Finds a capture's name; 0 when it has none. */
int
js_regexp_capture_name(
	const struct js_regexp_program *program,
	uint32_t capture,
	const uint16_t **name,
	size_t *length)
{
	size_t index;

	/* The names are few; each is looked at. */
	for (index = 0; index < program->name_count; index++) {
		if (program->names[index].capture != capture)
			continue;
		*name = &program->name_units[program->names[index].offset];
		*length = program->names[index].length;
		return 1;
	}

	/* The capture has no name. */
	return 0;
}

/*
 * Matches a program against an input from a position: at that position
 * only when the program is sticky, otherwise at the first position from
 * there where it matches.  The captures (two positions each, starts and
 * ends, JS_REGEXP_UNSET for a capture that did not take part) are stored
 * when it matched.  E2BIG when the match went on too long, ENOMEM when
 * memory ran out, and the status of the input's poll when it said to stop.
 */
int
js_regexp_match(
	const struct js_regexp_program *program,
	const struct js_regexp_input *input,
	size_t start,
	size_t *captures,
	int *matched)
{
	struct regexp_matcher matcher;
	size_t position;
	size_t slot_count;
	uint32_t unit;
	uint32_t next;
	int sticky;
	int found;
	int error;

	/* The matcher's arrays (at least one entry each, so that none is NULL). */
	memset(&matcher, 0, sizeof(matcher));
	matcher.program = program;
	matcher.input = input;
	slot_count = (size_t)program->capture_count * 2U;
	matcher.slots = malloc(slot_count * sizeof(*matcher.slots));
	if (matcher.slots == NULL)
		return ENOMEM;
	matcher.counters = calloc((size_t)program->counter_count + 1U, sizeof(*matcher.counters));
	if (matcher.counters == NULL) {
		free(matcher.slots);
		return ENOMEM;
	}

	/* The loops' start positions. */
	matcher.starts = calloc((size_t)program->counter_count + 1U, sizeof(*matcher.starts));
	if (matcher.starts == NULL) {
		free(matcher.counters);
		free(matcher.slots);
		return ENOMEM;
	}

	/* Each position from the start, or only the start when the program is sticky. */
	*matched = 0;
	error = 0;
	found = 0;
	sticky = (program->flags & JS_REGEXP_STICKY) != 0;
	position = start;
	while (position <= input->length) {
		/* A match that must start with one unit skips the positions without it. */
		if (!sticky && program->first_unit >= 0) {
			while (position < input->length) {
				unit = regexp_input_unit(input, position);
				if (unit == (uint32_t)program->first_unit)
					break;
				position++;
			}

			/* No unit left to start a match with. */
			if (position >= input->length)
				break;
		}

		/* The program at this position. */
		error = regexp_attempt(&matcher, position, &found);
		if (error != 0 || found)
			break;
		if (sticky)
			break;

		/* The next position; under u a surrogate pair is one step. */
		next = position + 1U;
		if ((program->flags & JS_REGEXP_UNICODE) != 0 && position + 1U < input->length) {
			unit = regexp_input_unit(input, position);
			if (unit >= 0xD800U && unit <= 0xDBFFU) {
				unit = regexp_input_unit(input, position + 1U);
				if (unit >= 0xDC00U && unit <= 0xDFFFU)
					next = position + 2U;
			}
		}

		/* The attempt moves on. */
		position = next;
	}

	/* The captures of a match. */
	if (error == 0 && found && position <= input->length) {
		memcpy(captures, matcher.slots, slot_count * sizeof(*captures));
		*matched = 1;
	}

	/* The matcher's arrays go. */
	free(matcher.stack);
	free(matcher.starts);
	free(matcher.counters);
	free(matcher.slots);
	if (error != 0)
		return error;

	/* Succeeded: *matched says whether it matched. */
	return 0;
}

/*
 * Counts the captures of a pattern before it is parsed (a \N is a
 * reference only when the pattern has N groups, wherever they are) and
 * notes whether any group has a name.
 */
static void
regexp_prescan(
	struct regexp_parser *parser)
{
	size_t index;
	uint16_t unit;
	int in_class;

	/* Every ( that opens a capturing group, skipping escapes and classes. */
	in_class = 0;
	for (index = 0; index < parser->length; index++) {
		unit = parser->pattern[index];
		if (unit == '\\') {
			index++;
			continue;
		}

		/* A class holds no groups. */
		if (in_class) {
			if (unit == ']')
				in_class = 0;
			continue;
		}

		/* A class starts at its [. */
		if (unit == '[') {
			in_class = 1;
			continue;
		}

		/* Only a ( can open a group. */
		if (unit != '(')
			continue;

		/* ( without ? captures; (?< without = or ! captures with a name. */
		if (index + 1U >= parser->length || parser->pattern[index + 1U] != '?') {
			parser->capture_total++;
			continue;
		}

		/* A named group captures too. */
		if (index + 3U < parser->length && parser->pattern[index + 2U] == '<' && parser->pattern[index + 3U] != '=' &&
		    parser->pattern[index + 3U] != '!') {
			parser->capture_total++;
			parser->named = 1;
		}
	}
}

/* Records a syntax error. */
static int
regexp_fail(
	struct regexp_parser *parser,
	const char *message)
{
	/* The first error is the one reported. */
	if (parser->message == NULL)
		parser->message = message;

	/* A syntax error. */
	return EINVAL;
}

/* Reads the code unit at an offset from the position; -1 past the end. */
static int32_t
regexp_unit(
	const struct regexp_parser *parser,
	size_t offset)
{
	/* Past the end. */
	if (parser->position + offset >= parser->length)
		return -1;

	/* The unit. */
	return parser->pattern[parser->position + offset];
}

/* Reads the character at the position: a code unit, or under u a surrogate pair's code point. */
static uint32_t
regexp_peek(
	const struct regexp_parser *parser,
	size_t *size)
{
	uint32_t lead;
	uint32_t trail;

	/* One unit. */
	lead = parser->pattern[parser->position];
	*size = 1;
	if (!parser->unicode || lead < 0xD800U || lead > 0xDBFFU || parser->position + 1U >= parser->length)
		return lead;

	/* A pair under u is one code point. */
	trail = parser->pattern[parser->position + 1U];
	if (trail < 0xDC00U || trail > 0xDFFFU)
		return lead;
	*size = 2;
	return 0x10000U + ((lead - 0xD800U) << 10) + (trail - 0xDC00U);
}

/* Makes a node of a kind with no children. */
static int
regexp_node_new(
	struct regexp_parser *parser,
	int kind,
	int32_t *index)
{
	struct regexp_node node;
	int error;

	/* A node with nothing in it. */
	memset(&node, 0, sizeof(node));
	node.kind = kind;
	node.first = -1;
	node.next = -1;
	node.greedy = 1;
	error = wb_vector_push(&parser->nodes, &node);
	if (error != 0)
		return error;

	/* Succeeded: its index. */
	*index = (int32_t)(parser->nodes.count - 1U);
	return 0;
}

/* Finds a node by index (good until the next node is made). */
static struct regexp_node *
regexp_node(
	const struct regexp_parser *parser,
	int32_t index)
{
	/* The node in the vector. */
	return wb_vector_at(&parser->nodes, (size_t)index);
}

/* Parses alternatives separated by |. */
static int
regexp_parse_disjunction(
	struct regexp_parser *parser,
	int32_t *result)
{
	int32_t alternation;
	int32_t alternative;
	int32_t last;
	int32_t unit;
	int error;

	/* The first alternative; without a | it is the whole disjunction. */
	error = regexp_parse_alternative(parser, &alternative);
	if (error != 0)
		return error;
	unit = regexp_unit(parser, 0);
	if (unit != '|') {
		*result = alternative;
		return 0;
	}

	/* The alternation of it and the others. */
	error = regexp_node_new(parser, REGEXP_NODE_ALTERNATION, &alternation);
	if (error != 0)
		return error;
	regexp_node(parser, alternation)->first = alternative;
	last = alternative;
	while (unit == '|') {
		parser->position++;
		error = regexp_parse_alternative(parser, &alternative);
		if (error != 0)
			return error;
		regexp_node(parser, last)->next = alternative;
		last = alternative;
		unit = regexp_unit(parser, 0);
	}

	/* Succeeded: the alternation. */
	*result = alternation;
	return 0;
}

/* Parses the terms of one alternative, up to a |, a ) or the end. */
static int
regexp_parse_alternative(
	struct regexp_parser *parser,
	int32_t *result)
{
	int32_t sequence;
	int32_t term;
	int32_t last;
	int32_t unit;
	int error;

	/* The sequence of the terms. */
	error = regexp_node_new(parser, REGEXP_NODE_SEQUENCE, &sequence);
	if (error != 0)
		return error;

	/* Each term in order. */
	last = -1;
	for (;;) {
		unit = regexp_unit(parser, 0);
		if (unit < 0 || unit == '|' || unit == ')')
			break;
		error = regexp_parse_term(parser, &term);
		if (error != 0)
			return error;

		/* The term after the one before. */
		if (last < 0) {
			regexp_node(parser, sequence)->first = term;
		} else {
			regexp_node(parser, last)->next = term;
		}

		/* The next term follows this one. */
		last = term;
	}

	/* Succeeded: the sequence. */
	*result = sequence;
	return 0;
}

/* Parses a term: an assertion, or an atom with an optional quantifier. */
static int
regexp_parse_term(
	struct regexp_parser *parser,
	int32_t *result)
{
	int32_t unit;
	int32_t next;
	int32_t after;
	int32_t fourth;
	int32_t atom;
	uint32_t capture_first;
	uint32_t kind;
	uint32_t min;
	uint32_t max;
	int quantifiable;
	int valid;
	int error;

	/* The assertions that take no quantifier. */
	unit = regexp_unit(parser, 0);
	next = regexp_unit(parser, 1);
	after = regexp_unit(parser, 2);
	fourth = regexp_unit(parser, 3);
	quantifiable = 1;
	capture_first = parser->capture_count;
	if (unit == '^' || unit == '$') {
		/* The start or the end of the input (or of a line under m). */
		error = regexp_node_new(parser, REGEXP_NODE_LINE_START, &atom);
		if (error != 0)
			return error;
		if (unit == '$')
			regexp_node(parser, atom)->kind = REGEXP_NODE_LINE_END;
		parser->position++;
		quantifiable = 0;
	} else if (unit == '\\' && (next == 'b' || next == 'B')) {
		/* A word boundary, or its absence. */
		error = regexp_node_new(parser, REGEXP_NODE_WORD_BOUNDARY, &atom);
		if (error != 0)
			return error;
		if (next == 'B')
			regexp_node(parser, atom)->kind = REGEXP_NODE_NOT_WORD_BOUNDARY;
		parser->position += 2U;
		quantifiable = 0;
	} else if (unit == '(' && next == '?' && (after == '=' || after == '!')) {
		/* A lookahead; Annex B lets one be quantified without u. */
		parser->position += 3U;
		kind = REGEXP_LOOK_AHEAD;
		if (after == '!')
			kind = REGEXP_LOOK_NOT_AHEAD;
		error = regexp_parse_look(parser, kind, &atom);
		if (error != 0)
			return error;
		quantifiable = !parser->unicode;
	} else if (unit == '(' && next == '?' && after == '<' && (fourth == '=' || fourth == '!')) {
		/* A lookbehind. */
		parser->position += 4U;
		kind = REGEXP_LOOK_BEHIND;
		if (fourth == '!')
			kind = REGEXP_LOOK_NOT_BEHIND;
		error = regexp_parse_look(parser, kind, &atom);
		if (error != 0)
			return error;
		quantifiable = 0;
	} else {
		/* An atom. */
		error = regexp_parse_atom(parser, &atom);
		if (error != 0)
			return error;
	}

	/* A quantifier after something that takes none is an error (without u, a { that is no quantifier is a literal). */
	unit = regexp_unit(parser, 0);
	if ((unit == '*' || unit == '+' || unit == '?' || unit == '{') && !quantifiable) {
		if (unit != '{' || parser->unicode)
			return regexp_fail(parser, "nothing to repeat");
		error = regexp_parse_braces(parser, &min, &max, &valid);
		if (error != 0)
			return error;
		if (valid)
			return regexp_fail(parser, "nothing to repeat");
	} else if (unit == '*' || unit == '+' || unit == '?' || unit == '{') {
		error = regexp_parse_quantifier(parser, atom, capture_first, &atom);
		if (error != 0)
			return error;
	}

	/* Succeeded: the term. */
	*result = atom;
	return 0;
}

/*
 * Parses the quantifier after an atom, if there is one (a { that does not
 * start a valid quantifier is left for the next term without u), and
 * makes the repetition.
 */
static int
regexp_parse_quantifier(
	struct regexp_parser *parser,
	int32_t atom,
	uint32_t capture_first,
	int32_t *result)
{
	struct regexp_node *node;
	int32_t repeat;
	int32_t unit;
	uint32_t min;
	uint32_t max;
	int valid;
	int error;

	/* The bounds of the quantifier. */
	unit = regexp_unit(parser, 0);
	min = 0;
	max = REGEXP_INFINITY;
	if (unit == '*') {
		parser->position++;
	} else if (unit == '+') {
		min = 1;
		parser->position++;
	} else if (unit == '?') {
		max = 1;
		parser->position++;
	} else {
		/* {n}, {n,} or {n,m}; anything else is a literal { without u. */
		error = regexp_parse_braces(parser, &min, &max, &valid);
		if (error != 0)
			return error;
		if (!valid && parser->unicode)
			return regexp_fail(parser, "incomplete quantifier");
		if (!valid) {
			*result = atom;
			return 0;
		}
	}

	/* The repetition, lazy after a ?. */
	unit = regexp_unit(parser, 0);
	error = regexp_node_new(parser, REGEXP_NODE_REPEAT, &repeat);
	if (error != 0)
		return error;
	node = regexp_node(parser, repeat);
	node->first = atom;
	node->min = min;
	node->max = max;
	node->capture_first = capture_first;
	node->capture_end = parser->capture_count;
	if (unit == '?') {
		node->greedy = 0;
		parser->position++;
	}

	/* Another quantifier straight after is an error. */
	unit = regexp_unit(parser, 0);
	if (unit == '*' || unit == '+' || unit == '?')
		return regexp_fail(parser, "nothing to repeat");
	if (unit == '{') {
		error = regexp_parse_braces(parser, &min, &max, &valid);
		if (error != 0)
			return error;
		if (valid || parser->unicode)
			return regexp_fail(parser, "nothing to repeat");
	}

	/* Succeeded: the repetition. */
	*result = repeat;
	return 0;
}

/*
 * Parses {n}, {n,} or {n,m} at the position; *valid is 0 (and nothing is
 * read) when the brace does not start one.  Numbers too big to count are
 * as big as can be counted.
 */
static int
regexp_parse_braces(
	struct regexp_parser *parser,
	uint32_t *min,
	uint32_t *max,
	int *valid)
{
	size_t start;
	uint64_t number;
	int32_t unit;
	int digits;

	/* The {. */
	*valid = 0;
	start = parser->position;
	parser->position++;

	/* The least: at least one digit. */
	number = 0;
	digits = 0;
	for (;;) {
		unit = regexp_unit(parser, 0);
		if (unit < '0' || unit > '9')
			break;
		number = number * 10U + (uint64_t)(unit - '0');
		if (number > REGEXP_INFINITY - 1U)
			number = REGEXP_INFINITY - 1U;
		digits++;
		parser->position++;
	}

	/* A brace without a digit is no quantifier. */
	if (digits == 0) {
		parser->position = start;
		return 0;
	}

	/* One number is both bounds until a comma says otherwise. */
	*min = (uint32_t)number;
	*max = (uint32_t)number;

	/* A comma, then the most (none for no limit). */
	unit = regexp_unit(parser, 0);
	if (unit == ',') {
		parser->position++;
		number = 0;
		digits = 0;
		for (;;) {
			unit = regexp_unit(parser, 0);
			if (unit < '0' || unit > '9')
				break;
			number = number * 10U + (uint64_t)(unit - '0');
			if (number > REGEXP_INFINITY - 1U)
				number = REGEXP_INFINITY - 1U;
			digits++;
			parser->position++;
		}

		/* An empty most is no limit. */
		*max = REGEXP_INFINITY;
		if (digits != 0)
			*max = (uint32_t)number;
	}

	/* The }. */
	unit = regexp_unit(parser, 0);
	if (unit != '}') {
		parser->position = start;
		return 0;
	}

	/* The } is taken. */
	parser->position++;

	/* The bounds must be in order. */
	*valid = 1;
	if (*min > *max)
		return regexp_fail(parser, "numbers out of order in {} quantifier");

	/* Succeeded: the bounds. */
	return 0;
}

/* Parses an atom: a character, ., a class, a group or an escape. */
static int
regexp_parse_atom(
	struct regexp_parser *parser,
	int32_t *result)
{
	uint32_t code_point;
	uint32_t min;
	uint32_t max;
	size_t size;
	int32_t unit;
	int valid;
	int error;

	/* What the atom starts with. */
	unit = regexp_unit(parser, 0);
	switch (unit) {
	case '.':
		/* Any character (but a line terminator without s). */
		parser->position++;
		error = regexp_node_new(parser, REGEXP_NODE_ANY, result);
		return error;
	case '(':
		error = regexp_parse_group(parser, result);
		return error;
	case '[':
		error = regexp_parse_class(parser, result);
		return error;
	case '\\':
		error = regexp_parse_atom_escape(parser, result);
		return error;
	case '*':
	case '+':
	case '?':
		return regexp_fail(parser, "nothing to repeat");
	case '{':
		/* A literal { without u, unless it would be a quantifier. */
		if (parser->unicode)
			return regexp_fail(parser, "lone quantifier brackets");
		error = regexp_parse_braces(parser, &min, &max, &valid);
		if (error != 0)
			return error;
		if (valid)
			return regexp_fail(parser, "nothing to repeat");
		break;
	case '}':
	case ']':
		/* Literal without u. */
		if (parser->unicode)
			return regexp_fail(parser, "lone quantifier brackets");
		break;
	default:
		break;
	}

	/* A pattern character. */
	code_point = regexp_peek(parser, &size);
	parser->position += size;
	error = regexp_char_node(parser, code_point, result);
	if (error != 0)
		return error;

	/* Succeeded: the character. */
	return 0;
}

/* Parses a group after its (: capturing, named or not capturing. */
static int
regexp_parse_group(
	struct regexp_parser *parser,
	int32_t *result)
{
	struct regexp_name name;
	struct regexp_name *other;
	int32_t group;
	int32_t child;
	int32_t unit;
	uint32_t capture;
	uint32_t offset;
	uint32_t length;
	size_t index;
	int capturing;
	int same;
	int error;

	/* Too deep a nesting is refused rather than run out of stack. */
	if (parser->depth >= REGEXP_DEPTH_MAX)
		return regexp_fail(parser, "regular expression too large");

	/* (?: does not capture, (?<name> captures with a name, ( captures. */
	parser->position++;
	capturing = 1;
	capture = 0;
	unit = regexp_unit(parser, 0);
	if (unit == '?') {
		unit = regexp_unit(parser, 1);
		if (unit == ':') {
			capturing = 0;
			parser->position += 2U;
		} else if (unit == '<') {
			/* The name, unique in the pattern. */
			parser->position += 2U;
			error = regexp_parse_group_name(parser, &offset, &length);
			if (error != 0)
				return error;
			for (index = 0; index < parser->names.count; index++) {
				other = wb_vector_at(&parser->names, index);
				if (other->length != length)
					continue;
				same = memcmp(&parser->name_units.data[other->offset], &parser->name_units.data[offset], length * sizeof(uint16_t));
				if (same == 0)
					return regexp_fail(parser, "duplicate capture group name");
			}

			/* The name belongs to the next capture. */
			name.capture = parser->capture_count;
			name.offset = offset;
			name.length = length;
			error = wb_vector_push(&parser->names, &name);
			if (error != 0)
				return error;
		} else {
			return regexp_fail(parser, "invalid group");
		}
	}

	/* A capturing group takes the next number. */
	if (capturing) {
		capture = parser->capture_count;
		parser->capture_count++;
	}

	/* The disjunction inside, up to the ). */
	parser->depth++;
	error = regexp_parse_disjunction(parser, &child);
	parser->depth--;
	if (error != 0)
		return error;
	unit = regexp_unit(parser, 0);
	if (unit != ')')
		return regexp_fail(parser, "unterminated group");
	parser->position++;

	/* A group that does not capture is its disjunction. */
	if (!capturing) {
		*result = child;
		return 0;
	}

	/* Succeeded: the capturing group. */
	error = regexp_node_new(parser, REGEXP_NODE_GROUP, &group);
	if (error != 0)
		return error;
	regexp_node(parser, group)->value = capture;
	regexp_node(parser, group)->first = child;
	*result = group;
	return 0;
}

/* Parses a lookaround's disjunction after its opening, up to the ). */
static int
regexp_parse_look(
	struct regexp_parser *parser,
	uint32_t kind,
	int32_t *result)
{
	int32_t look;
	int32_t child;
	int32_t unit;
	int error;

	/* Too deep a nesting is refused rather than run out of stack. */
	if (parser->depth >= REGEXP_DEPTH_MAX)
		return regexp_fail(parser, "regular expression too large");

	/* The body. */
	parser->depth++;
	error = regexp_parse_disjunction(parser, &child);
	parser->depth--;
	if (error != 0)
		return error;
	unit = regexp_unit(parser, 0);
	if (unit != ')')
		return regexp_fail(parser, "unterminated group");
	parser->position++;

	/* Succeeded: the lookaround. */
	error = regexp_node_new(parser, REGEXP_NODE_LOOK, &look);
	if (error != 0)
		return error;
	regexp_node(parser, look)->value = kind;
	regexp_node(parser, look)->first = child;
	*result = look;
	return 0;
}

/* Parses an escape that is an atom: a class escape, a back reference or a character. */
static int
regexp_parse_atom_escape(
	struct regexp_parser *parser,
	int32_t *result)
{
	uint32_t code_point;
	uint32_t number;
	uint32_t offset;
	uint32_t length;
	size_t start;
	int32_t unit;
	int32_t reference;
	int error;

	/* A \ must be followed by something. */
	parser->position++;
	unit = regexp_unit(parser, 0);
	if (unit < 0)
		return regexp_fail(parser, "\\ at end of pattern");

	/* What the escape is. */
	switch (unit) {
	case 'd':
	case 'D':
	case 's':
	case 'S':
	case 'w':
	case 'W':
		/* A class escape. */
		parser->position++;
		error = regexp_set_node(parser, unit, result);
		return error;
	case 'p':
	case 'P':
		/* Unicode property escapes are not in this pass; without u they are the letters. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid property name");
		break;
	case 'k':
		/* A named reference under u or with named groups; the letter otherwise. */
		if (!parser->unicode && !parser->named)
			break;
		parser->position++;
		unit = regexp_unit(parser, 0);
		if (unit != '<')
			return regexp_fail(parser, "invalid named reference");
		parser->position++;
		error = regexp_parse_group_name(parser, &offset, &length);
		if (error != 0)
			return error;
		error = regexp_node_new(parser, REGEXP_NODE_NAMED_BACKREF, &reference);
		if (error != 0)
			return error;
		regexp_node(parser, reference)->name_offset = offset;
		regexp_node(parser, reference)->name_length = length;
		*result = reference;
		return 0;
	case '0':
		/* NUL, unless digits follow (a legacy octal escape without u). */
		unit = regexp_unit(parser, 1);
		if (unit >= '0' && unit <= '9') {
			if (parser->unicode)
				return regexp_fail(parser, "invalid decimal escape");
			code_point = regexp_parse_legacy_octal(parser);
			error = regexp_char_node(parser, code_point, result);
			return error;
		}

		/* A lone \0 is NUL. */
		parser->position++;
		error = regexp_char_node(parser, 0, result);
		return error;
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		/* A back reference when the pattern has that many captures. */
		start = parser->position;
		number = 0;
		for (;;) {
			unit = regexp_unit(parser, 0);
			if (unit < '0' || unit > '9')
				break;
			if (number < 100000U)
				number = number * 10U + (uint32_t)(unit - '0');
			parser->position++;
		}

		/* A reference to an existing capture. */
		if (number <= parser->capture_total) {
			error = regexp_node_new(parser, REGEXP_NODE_BACKREF, &reference);
			if (error != 0)
				return error;
			regexp_node(parser, reference)->value = number;
			*result = reference;
			return 0;
		}

		/* Otherwise an error under u, a legacy octal escape or the digit (8, 9) without it. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid escape");
		parser->position = start;
		unit = regexp_unit(parser, 0);
		if (unit >= '8') {
			parser->position++;
			error = regexp_char_node(parser, (uint32_t)unit, result);
			return error;
		}

		/* A legacy octal escape. */
		code_point = regexp_parse_legacy_octal(parser);
		error = regexp_char_node(parser, code_point, result);
		return error;
	default:
		break;
	}

	/* A character escape. */
	error = regexp_parse_character_escape(parser, 0, &code_point);
	if (error != 0)
		return error;
	error = regexp_char_node(parser, code_point, result);
	if (error != 0)
		return error;

	/* Succeeded: the character. */
	return 0;
}

/*
 * Parses a group name after its <, up to and past the >, into the name
 * units: an identifier's characters (a \u escape may spell one; a surrogate
 * pair is one character).
 */
static int
regexp_parse_group_name(
	struct regexp_parser *parser,
	uint32_t *offset,
	uint32_t *length)
{
	uint32_t code_point;
	uint32_t trail;
	int32_t unit;
	size_t start;
	int valid;
	int first;
	int allowed;
	int error;

	/* Each character up to the >. */
	start = parser->name_units.length;
	first = 1;
	for (;;) {
		unit = regexp_unit(parser, 0);
		if (unit < 0)
			return regexp_fail(parser, "invalid capture group name");
		if (unit == '>')
			break;

		/* An escape spells a character; a pair is one. */
		if (unit == '\\') {
			parser->position++;
			unit = regexp_unit(parser, 0);
			if (unit != 'u')
				return regexp_fail(parser, "invalid capture group name");
			parser->position++;
			error = regexp_parse_unicode_escape(parser, &code_point, &valid);
			if (error != 0)
				return error;
			if (!valid)
				return regexp_fail(parser, "invalid capture group name");
		} else {
			code_point = (uint32_t)unit;
			parser->position++;
			if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
				unit = regexp_unit(parser, 0);
				trail = (uint32_t)unit;
				if (unit >= 0xDC00 && unit <= 0xDFFF) {
					code_point = 0x10000U + ((code_point - 0xD800U) << 10) + (trail - 0xDC00U);
					parser->position++;
				}
			}
		}

		/* An identifier's character: $, _, letters (and beyond ASCII, taken as letters), digits after the first. */
		allowed = 0;
		if (code_point == '$' || code_point == '_')
			allowed = 1;
		else if ((code_point >= 'a' && code_point <= 'z') || (code_point >= 'A' && code_point <= 'Z'))
			allowed = 1;
		else if (!first && code_point >= '0' && code_point <= '9')
			allowed = 1;
		else if (code_point >= 0x80U && (code_point < 0xD800U || code_point > 0xDFFFU))
			allowed = 1;
		if (!allowed)
			return regexp_fail(parser, "invalid capture group name");
		error = wb_units_append_code_point(&parser->name_units, code_point);
		if (error != 0)
			return error;
		first = 0;
	}

	/* The > ends a name that is not empty. */
	parser->position++;
	if (first)
		return regexp_fail(parser, "invalid capture group name");

	/* Succeeded: where the name is. */
	*offset = (uint32_t)start;
	*length = (uint32_t)(parser->name_units.length - start);
	return 0;
}

/*
 * Parses a character escape after its \ (in a class or not): the control
 * escapes, \c, \x, \u and the identity escapes (under u only the syntax
 * characters and /, and - in a class).
 */
static int
regexp_parse_character_escape(
	struct regexp_parser *parser,
	int in_class,
	uint32_t *code_point)
{
	const char *syntax;
	uint32_t value;
	size_t size;
	int32_t unit;
	int32_t letter;
	int valid;
	int error;

	/* The letter after the \. */
	unit = regexp_unit(parser, 0);
	switch (unit) {
	case 'f':
		parser->position++;
		*code_point = 0x0C;
		return 0;
	case 'n':
		parser->position++;
		*code_point = 0x0A;
		return 0;
	case 'r':
		parser->position++;
		*code_point = 0x0D;
		return 0;
	case 't':
		parser->position++;
		*code_point = 0x09;
		return 0;
	case 'v':
		parser->position++;
		*code_point = 0x0B;
		return 0;
	case 'c':
		/* A control letter; without u (Annex B) the \ is literal before anything else (digits and _ in a class). */
		letter = regexp_unit(parser, 1);
		if ((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z')) {
			parser->position += 2U;
			*code_point = (uint32_t)letter % 32U;
			return 0;
		}

		/* Annex B lets a digit or _ follow \c in a class. */
		if (!parser->unicode && in_class && ((letter >= '0' && letter <= '9') || letter == '_')) {
			parser->position += 2U;
			*code_point = (uint32_t)letter % 32U;
			return 0;
		}

		/* \c followed by anything else. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid unicode escape");
		*code_point = '\\';
		return 0;
	case 'x':
		/* Two hexadecimal digits, or the letter x without u. */
		parser->position++;
		error = regexp_parse_hex(parser, 2, &value);
		if (error == 0) {
			*code_point = value;
			return 0;
		}

		/* \x without two digits. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid escape");
		*code_point = 'x';
		return 0;
	case 'u':
		/* \uXXXX (a pair of them under u is one code point, as is \u{...}), or the letter u without u. */
		parser->position++;
		error = regexp_parse_unicode_escape(parser, &value, &valid);
		if (error != 0)
			return error;
		if (valid) {
			*code_point = value;
			return 0;
		}

		/* \u without its digits. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid unicode escape");
		*code_point = 'u';
		return 0;
	default:
		break;
	}

	/* An identity escape: under u a syntax character, /, or - in a class. */
	value = regexp_peek(parser, &size);
	if (parser->unicode) {
		valid = 0;
		syntax = NULL;
		if (value < 0x80U && value != 0)
			syntax = strchr("^$\\.*+?()[]{}|/", (int)value);
		if (syntax != NULL)
			valid = 1;
		if (in_class && value == '-')
			valid = 1;
		if (!valid)
			return regexp_fail(parser, "invalid escape");
	}

	/* Succeeded: the character itself. */
	parser->position += size;
	*code_point = value;
	return 0;
}

/*
 * Parses what follows \u: four hexadecimal digits (under u a lead
 * surrogate's escape and a trail's make one code point), or under u
 * {hex digits} up to 10FFFF; *valid is 0 (and nothing is read) when
 * neither follows.
 */
static int
regexp_parse_unicode_escape(
	struct regexp_parser *parser,
	uint32_t *code_point,
	int *valid)
{
	uint32_t value;
	uint32_t trail;
	size_t start;
	int32_t unit;
	int32_t letter;
	int digits;
	int error;

	/* \u{...} under u. */
	*valid = 0;
	start = parser->position;
	unit = regexp_unit(parser, 0);
	if (unit == '{' && parser->unicode) {
		parser->position++;
		value = 0;
		digits = 0;
		for (;;) {
			unit = regexp_unit(parser, 0);
			if (unit >= '0' && unit <= '9') {
				value = value * 16U + (uint32_t)(unit - '0');
			} else if (unit >= 'a' && unit <= 'f') {
				value = value * 16U + (uint32_t)(unit - 'a' + 10);
			} else if (unit >= 'A' && unit <= 'F') {
				value = value * 16U + (uint32_t)(unit - 'A' + 10);
			} else {
				break;
			}

			/* A code point past the last is an error. */
			if (value > REGEXP_CODE_POINT_MAX)
				return regexp_fail(parser, "invalid unicode escape");
			digits++;
			parser->position++;
		}

		/* The braces must hold digits and close. */
		if (digits == 0 || unit != '}')
			return regexp_fail(parser, "invalid unicode escape");
		parser->position++;
		*code_point = value;
		*valid = 1;
		return 0;
	}

	/* Four digits. */
	error = regexp_parse_hex(parser, 4, &value);
	if (error != 0) {
		parser->position = start;
		return 0;
	}

	/* Under u, a lead surrogate's escape followed by a trail's is one code point. */
	if (parser->unicode && value >= 0xD800U && value <= 0xDBFFU) {
		start = parser->position;
		unit = regexp_unit(parser, 0);
		letter = regexp_unit(parser, 1);
		if (unit == '\\' && letter == 'u') {
			parser->position += 2U;
			error = regexp_parse_hex(parser, 4, &trail);
			if (error == 0 && trail >= 0xDC00U && trail <= 0xDFFFU) {
				value = 0x10000U + ((value - 0xD800U) << 10) + (trail - 0xDC00U);
			} else {
				parser->position = start;
			}
		}
	}

	/* Succeeded: the code unit or point. */
	*code_point = value;
	*valid = 1;
	return 0;
}

/* Reads a number of hexadecimal digits; EINVAL (nothing read) when they are not there. */
static int
regexp_parse_hex(
	struct regexp_parser *parser,
	unsigned digits,
	uint32_t *value)
{
	unsigned index;
	int32_t unit;
	uint32_t result;

	/* Each digit. */
	result = 0;
	for (index = 0; index < digits; index++) {
		unit = regexp_unit(parser, index);
		if (unit >= '0' && unit <= '9') {
			result = result * 16U + (uint32_t)(unit - '0');
		} else if (unit >= 'a' && unit <= 'f') {
			result = result * 16U + (uint32_t)(unit - 'a' + 10);
		} else if (unit >= 'A' && unit <= 'F') {
			result = result * 16U + (uint32_t)(unit - 'A' + 10);
		} else {
			return EINVAL;
		}
	}

	/* Succeeded: the value. */
	parser->position += digits;
	*value = result;
	return 0;
}

/* Reads a legacy octal escape's digits (Annex B): up to three, at most 0377. */
static uint32_t
regexp_parse_legacy_octal(
	struct regexp_parser *parser)
{
	uint32_t value;
	int32_t unit;
	unsigned most;
	unsigned count;

	/* A first digit of 0 to 3 allows three digits, 4 to 7 two. */
	unit = regexp_unit(parser, 0);
	value = (uint32_t)(unit - '0');
	parser->position++;
	most = 2;
	if (value <= 3U)
		most = 3;

	/* The digits that follow. */
	for (count = 1; count < most; count++) {
		unit = regexp_unit(parser, 0);
		if (unit < '0' || unit > '7')
			break;
		value = value * 8U + (uint32_t)(unit - '0');
		parser->position++;
	}

	/* The value. */
	return value;
}

/* Parses a character class after its [. */
static int
regexp_parse_class(
	struct regexp_parser *parser,
	int32_t *result)
{
	struct regexp_class made;
	uint32_t range_first;
	uint32_t low;
	uint32_t high;
	int32_t node;
	int32_t unit;
	int32_t next;
	int low_set;
	int high_set;
	int error;

	/* [^ negates the class. */
	parser->position++;
	memset(&made, 0, sizeof(made));
	unit = regexp_unit(parser, 0);
	if (unit == '^') {
		made.negated = 1;
		parser->position++;
	}

	/* The class's ranges start here in the table. */
	range_first = (uint32_t)parser->ranges.count;

	/* Each atom or range up to the ]. */
	for (;;) {
		unit = regexp_unit(parser, 0);
		if (unit < 0)
			return regexp_fail(parser, "unterminated character class");
		if (unit == ']')
			break;
		error = regexp_parse_class_atom(parser, &low, &low_set);
		if (error != 0)
			return error;

		/* A - between two atoms (not before the ]) makes a range. */
		unit = regexp_unit(parser, 0);
		next = regexp_unit(parser, 1);
		if (unit != '-' || next == ']' || next < 0) {
			if (low_set) {
				error = regexp_add_set(parser, (int)low);
			} else {
				error = regexp_add_range(parser, low, low);
			}

			/* A failure to add the atom. */
			if (error != 0)
				return error;
			continue;
		}

		/* The - is taken; the range's other end follows. */
		parser->position++;
		error = regexp_parse_class_atom(parser, &high, &high_set);
		if (error != 0)
			return error;

		/* A class escape at either end: an error under u, the parts and a - without it (Annex B). */
		if (low_set || high_set) {
			if (parser->unicode)
				return regexp_fail(parser, "invalid character class");
			if (low_set) {
				error = regexp_add_set(parser, (int)low);
			} else {
				error = regexp_add_range(parser, low, low);
			}

			/* The - itself, then the other end. */
			if (error == 0)
				error = regexp_add_range(parser, '-', '-');
			if (error == 0 && high_set) {
				error = regexp_add_set(parser, (int)high);
			} else if (error == 0) {
				error = regexp_add_range(parser, high, high);
			}

			/* A failure to add the parts. */
			if (error != 0)
				return error;
			continue;
		}

		/* The range's ends in order. */
		if (low > high)
			return regexp_fail(parser, "range out of order in character class");
		error = regexp_add_range(parser, low, high);
		if (error != 0)
			return error;
	}

	/* The ] ends the class; its ranges sorted and merged. */
	parser->position++;
	regexp_finish_class(parser, range_first);
	made.range_first = range_first;
	made.range_count = (uint32_t)parser->ranges.count - range_first;
	error = wb_vector_push(&parser->classes, &made);
	if (error != 0)
		return error;

	/* Succeeded: the class's node. */
	error = regexp_node_new(parser, REGEXP_NODE_CLASS, &node);
	if (error != 0)
		return error;
	regexp_node(parser, node)->value = (uint32_t)(parser->classes.count - 1U);
	*result = node;
	return 0;
}

/*
 * Parses one atom of a class: a character, or a class escape (*set is 1
 * and *code_point its letter).
 */
static int
regexp_parse_class_atom(
	struct regexp_parser *parser,
	uint32_t *code_point,
	int *set)
{
	size_t size;
	int32_t unit;
	int32_t next;
	int error;

	/* A character that is not an escape. */
	*set = 0;
	unit = regexp_unit(parser, 0);
	if (unit != '\\') {
		*code_point = regexp_peek(parser, &size);
		parser->position += size;
		return 0;
	}

	/* An escape. */
	parser->position++;
	unit = regexp_unit(parser, 0);
	if (unit < 0)
		return regexp_fail(parser, "\\ at end of pattern");
	switch (unit) {
	case 'd':
	case 'D':
	case 's':
	case 'S':
	case 'w':
	case 'W':
		/* A class escape. */
		parser->position++;
		*code_point = (uint32_t)unit;
		*set = 1;
		return 0;
	case 'p':
	case 'P':
		/* Unicode property escapes are not in this pass. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid property name");
		break;
	case 'b':
		/* Backspace. */
		parser->position++;
		*code_point = 0x08;
		return 0;
	case '-':
		/* A - (under u too). */
		if (parser->unicode) {
			parser->position++;
			*code_point = '-';
			return 0;
		}

		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		/* NUL; without u (Annex B) a legacy octal escape, or 8 or 9 themselves. */
		next = regexp_unit(parser, 1);
		if (unit == '0' && (next < '0' || next > '9')) {
			parser->position++;
			*code_point = 0;
			return 0;
		}

		/* Under u only \0 is a digit escape. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid class escape");
		if (unit >= '8') {
			parser->position++;
			*code_point = (uint32_t)unit;
			return 0;
		}

		/* A legacy octal escape. */
		*code_point = regexp_parse_legacy_octal(parser);
		return 0;
	case 'k':
		/* The letter k without u. */
		if (parser->unicode)
			return regexp_fail(parser, "invalid escape");
		parser->position++;
		*code_point = 'k';
		return 0;
	default:
		break;
	}

	/* A character escape. */
	error = regexp_parse_character_escape(parser, 1, code_point);
	if (error != 0)
		return error;

	/* Succeeded: the character. */
	return 0;
}

/* Makes the node of one character. */
static int
regexp_char_node(
	struct regexp_parser *parser,
	uint32_t code_point,
	int32_t *result)
{
	int error;

	/* The node. */
	error = regexp_node_new(parser, REGEXP_NODE_CHAR, result);
	if (error != 0)
		return error;

	/* Succeeded: it holds the character. */
	regexp_node(parser, *result)->value = code_point;
	return 0;
}

/* Makes the node of a class escape outside a class (\d, \D, \s, \S, \w, \W). */
static int
regexp_set_node(
	struct regexp_parser *parser,
	int letter,
	int32_t *result)
{
	struct regexp_class made;
	uint32_t range_first;
	int32_t node;
	int error;

	/* The class of the escape's ranges. */
	memset(&made, 0, sizeof(made));
	range_first = (uint32_t)parser->ranges.count;
	error = regexp_add_set(parser, letter);
	if (error != 0)
		return error;
	regexp_finish_class(parser, range_first);
	made.range_first = range_first;
	made.range_count = (uint32_t)parser->ranges.count - range_first;
	error = wb_vector_push(&parser->classes, &made);
	if (error != 0)
		return error;

	/* Succeeded: the node. */
	error = regexp_node_new(parser, REGEXP_NODE_CLASS, &node);
	if (error != 0)
		return error;
	regexp_node(parser, node)->value = (uint32_t)(parser->classes.count - 1U);
	*result = node;
	return 0;
}

/* Adds a range to the class being built. */
static int
regexp_add_range(
	struct regexp_parser *parser,
	uint32_t first,
	uint32_t last)
{
	struct regexp_range range;
	int error;

	/* The range at the table's end. */
	range.first = first;
	range.last = last;
	error = wb_vector_push(&parser->ranges, &range);
	if (error != 0)
		return error;

	/* Succeeded: the range is added. */
	return 0;
}

/*
 * Adds a class escape's ranges to the class being built: \d, \s, \w, or
 * the ranges between them for \D, \S, \W.
 */
static int
regexp_add_set(
	struct regexp_parser *parser,
	int letter)
{
	struct regexp_range extra[2];
	const struct regexp_range *table;
	struct regexp_range sorted[16];
	size_t count;
	size_t index;
	uint32_t next;
	int negated;
	int error;

	/* The escape's own ranges (under u and i, \w takes ſ and the Kelvin sign too). */
	negated = 0;
	if (letter == 'D' || letter == 'S' || letter == 'W')
		negated = 1;
	count = 0;
	table = NULL;
	switch (letter) {
	case 'd':
	case 'D':
		table = regexp_digits;
		count = sizeof(regexp_digits) / sizeof(regexp_digits[0]);
		break;
	case 's':
	case 'S':
		table = regexp_spaces;
		count = sizeof(regexp_spaces) / sizeof(regexp_spaces[0]);
		break;
	default:
		table = regexp_words;
		count = sizeof(regexp_words) / sizeof(regexp_words[0]);
		break;
	}

	/* A copy that the Kelvin sign and long s can join. */
	memcpy(sorted, table, count * sizeof(sorted[0]));
	if ((letter == 'w' || letter == 'W') && parser->unicode && (parser->flags & JS_REGEXP_IGNORE_CASE) != 0) {
		extra[0].first = 0x017FU;
		extra[0].last = 0x017FU;
		extra[1].first = 0x212AU;
		extra[1].last = 0x212AU;
		sorted[count] = extra[0];
		sorted[count + 1U] = extra[1];
		count += 2U;
	}

	/* The escape itself: its ranges. */
	if (!negated) {
		for (index = 0; index < count; index++) {
			error = regexp_add_range(parser, sorted[index].first, sorted[index].last);
			if (error != 0)
				return error;
		}

		/* The escape's ranges are added. */
		return 0;
	}

	/* The negated one: the gaps between them (the ranges are sorted). */
	next = 0;
	for (index = 0; index < count; index++) {
		if (sorted[index].first > next) {
			error = regexp_add_range(parser, next, sorted[index].first - 1U);
			if (error != 0)
				return error;
		}

		/* The next gap starts after this range. */
		next = sorted[index].last + 1U;
	}

	/* The last gap runs to the last code point. */
	error = regexp_add_range(parser, next, REGEXP_CODE_POINT_MAX);
	if (error != 0)
		return error;

	/* Succeeded: the ranges are added. */
	return 0;
}

/* Sorts and merges the ranges of the class being built (those from range_first on). */
static void
regexp_finish_class(
	struct regexp_parser *parser,
	uint32_t range_first)
{
	struct regexp_range *ranges;
	size_t count;
	size_t read;
	size_t write;

	/* Nothing to merge in fewer than two. */
	count = parser->ranges.count - range_first;
	if (count < 2U)
		return;

	/* Sorted by their first code point. */
	ranges = wb_vector_at(&parser->ranges, range_first);
	qsort(ranges, count, sizeof(*ranges), regexp_compare_ranges);

	/* Overlapping and adjacent ranges become one. */
	write = 0;
	for (read = 1; read < count; read++) {
		if (ranges[read].first <= ranges[write].last + 1U) {
			if (ranges[read].last > ranges[write].last)
				ranges[write].last = ranges[read].last;
			continue;
		}

		/* A range that does not touch the last starts a new one. */
		write++;
		ranges[write] = ranges[read];
	}

	/* The table ends after the merged ranges. */
	parser->ranges.count = range_first + write + 1U;
}

/* Orders two ranges by their first code point (qsort). */
static int
regexp_compare_ranges(
	const void *left,
	const void *right)
{
	const struct regexp_range *a;
	const struct regexp_range *b;

	/* The first code points. */
	a = left;
	b = right;
	if (a->first < b->first)
		return -1;
	if (a->first > b->first)
		return 1;

	/* The same start. */
	return 0;
}

/* Resolves each named reference to its group's number; a name no group has is an error. */
static int
regexp_resolve_names(
	struct regexp_parser *parser)
{
	struct regexp_node *node;
	struct regexp_name *name;
	size_t index;
	size_t other;
	int found;
	int same;

	/* Each named reference. */
	for (index = 0; index < parser->nodes.count; index++) {
		node = wb_vector_at(&parser->nodes, index);
		if (node->kind != REGEXP_NODE_NAMED_BACKREF)
			continue;

		/* The group of that name. */
		found = 0;
		for (other = 0; other < parser->names.count; other++) {
			name = wb_vector_at(&parser->names, other);
			if (name->length != node->name_length)
				continue;
			same = memcmp(&parser->name_units.data[name->offset], &parser->name_units.data[node->name_offset],
			    name->length * sizeof(uint16_t));
			if (same != 0)
				continue;
			node->kind = REGEXP_NODE_BACKREF;
			node->value = name->capture;
			found = 1;
			break;
		}

		/* A reference to a name no group has. */
		if (!found)
			return regexp_fail(parser, "invalid named capture referenced");
	}

	/* Succeeded: every reference has its group. */
	return 0;
}

/* Adds a word to the program being compiled. */
static int
regexp_emit(
	struct regexp_compiler *compiler,
	uint32_t word)
{
	int error;

	/* The word at the code's end. */
	error = wb_vector_push(&compiler->code, &word);
	if (error != 0)
		return error;

	/* Succeeded: the word is added. */
	return 0;
}

/* Reports where the next word of the program goes. */
static uint32_t
regexp_here(
	const struct regexp_compiler *compiler)
{
	/* The code's length. */
	return (uint32_t)compiler->code.count;
}

/* Fills a word emitted earlier in (a jump's target). */
static void
regexp_patch(
	struct regexp_compiler *compiler,
	uint32_t at,
	uint32_t value)
{
	uint32_t *word;

	/* The word in place. */
	word = wb_vector_at(&compiler->code, at);
	*word = value;
}

/* Compiles a node, forwards or (inside a lookbehind) backwards. */
static int
regexp_compile_node(
	struct regexp_compiler *compiler,
	int32_t index,
	int backward)
{
	const struct regexp_parser *parser;
	struct regexp_node node;
	uint32_t direction;
	uint32_t fold;
	uint32_t at;
	int error;

	/* The node's own copy (the vector does not move while compiling, but the copy is plain). */
	parser = compiler->parser;
	node = *regexp_node(parser, index);
	direction = 0;
	if (backward)
		direction = REGEXP_OP_BACKWARD;
	fold = 0;
	if ((parser->flags & JS_REGEXP_IGNORE_CASE) != 0)
		fold = 1;

	/* Each kind of node. */
	error = 0;
	switch (node.kind) {
	case REGEXP_NODE_EMPTY:
		break;
	case REGEXP_NODE_CHAR:
		/* The character, canonical when case does not matter. */
		if (fold) {
			error = regexp_emit(compiler, REGEXP_OP_CHAR_FOLD | direction);
			if (error == 0)
				error = regexp_emit(compiler, regexp_canonicalize(node.value, parser->unicode));
		} else {
			error = regexp_emit(compiler, REGEXP_OP_CHAR | direction);
			if (error == 0)
				error = regexp_emit(compiler, node.value);
		}

		break;
	case REGEXP_NODE_ANY:
		/* Any character; under s line terminators too. */
		if ((parser->flags & JS_REGEXP_DOT_ALL) != 0) {
			error = regexp_emit(compiler, REGEXP_OP_ANY_ALL | direction);
		} else {
			error = regexp_emit(compiler, REGEXP_OP_ANY | direction);
		}

		break;
	case REGEXP_NODE_CLASS:
		if (fold) {
			error = regexp_emit(compiler, REGEXP_OP_CLASS_FOLD | direction);
		} else {
			error = regexp_emit(compiler, REGEXP_OP_CLASS | direction);
		}

		/* The class it matches. */
		if (error == 0)
			error = regexp_emit(compiler, node.value);
		break;
	case REGEXP_NODE_SEQUENCE:
		error = regexp_compile_sequence(compiler, node.first, backward);
		break;
	case REGEXP_NODE_ALTERNATION:
		error = regexp_compile_alternation(compiler, node.first, backward);
		break;
	case REGEXP_NODE_GROUP:
		/* The capture's start and end around its body (the end first backwards). */
		error = regexp_emit(compiler, REGEXP_OP_SAVE);
		if (error == 0)
			error = regexp_emit(compiler, node.value * 2U + (uint32_t)backward);
		if (error == 0)
			error = regexp_compile_node(compiler, node.first, backward);
		if (error == 0)
			error = regexp_emit(compiler, REGEXP_OP_SAVE);
		if (error == 0)
			error = regexp_emit(compiler, node.value * 2U + (uint32_t)!backward);
		break;
	case REGEXP_NODE_BACKREF:
		error = regexp_emit(compiler, REGEXP_OP_BACKREF | direction);
		if (error == 0)
			error = regexp_emit(compiler, node.value);
		break;
	case REGEXP_NODE_LINE_START:
		error = regexp_emit(compiler, REGEXP_OP_LINE_START);
		break;
	case REGEXP_NODE_LINE_END:
		error = regexp_emit(compiler, REGEXP_OP_LINE_END);
		break;
	case REGEXP_NODE_WORD_BOUNDARY:
		error = regexp_emit(compiler, REGEXP_OP_WORD_BOUNDARY);
		break;
	case REGEXP_NODE_NOT_WORD_BOUNDARY:
		error = regexp_emit(compiler, REGEXP_OP_NOT_WORD_BOUNDARY);
		break;
	case REGEXP_NODE_LOOK:
		/* The body between the lookaround and its end; a lookbehind's body matches backwards. */
		error = regexp_emit(compiler, REGEXP_OP_LOOK);
		if (error == 0)
			error = regexp_emit(compiler, node.value);
		at = regexp_here(compiler);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0) {
			error = regexp_compile_node(
				compiler,
				node.first,
				node.value == REGEXP_LOOK_BEHIND || node.value == REGEXP_LOOK_NOT_BEHIND);
		}

		/* The lookaround's end, where its barrier sends a success. */
		if (error == 0)
			error = regexp_emit(compiler, REGEXP_OP_LOOK_END);
		if (error == 0)
			regexp_patch(compiler, at, regexp_here(compiler));
		break;
	case REGEXP_NODE_REPEAT:
		error = regexp_compile_repeat(compiler, &node, backward);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Reports a node that could not be compiled. */
	if (error != 0)
		return error;

	/* Succeeded: the node's code is emitted. */
	return 0;
}

/* Compiles a sequence's terms in order (backwards in reverse order). */
static int
regexp_compile_sequence(
	struct regexp_compiler *compiler,
	int32_t first,
	int backward)
{
	struct wb_vector terms;
	int32_t index;
	int32_t *term;
	size_t count;
	int error;

	/* Forwards: each term in order. */
	if (!backward) {
		for (index = first; index >= 0; index = regexp_node(compiler->parser, index)->next) {
			error = regexp_compile_node(compiler, index, 0);
			if (error != 0)
				return error;
		}

		/* The terms' code is emitted. */
		return 0;
	}

	/* Backwards: the terms gathered, then compiled from the last. */
	wb_vector_init(&terms, sizeof(int32_t));
	for (index = first; index >= 0; index = regexp_node(compiler->parser, index)->next) {
		error = wb_vector_push(&terms, &index);
		if (error != 0) {
			wb_vector_release(&terms);
			return error;
		}
	}

	/* The gathered terms from the last. */
	error = 0;
	for (count = terms.count; count > 0 && error == 0; count--) {
		term = wb_vector_at(&terms, count - 1U);
		error = regexp_compile_node(compiler, *term, 1);
	}

	/* The list goes. */
	wb_vector_release(&terms);
	if (error != 0)
		return error;

	/* Succeeded: the terms' code is emitted. */
	return 0;
}

/*
 * Compiles an alternation: each alternative but the last tried with a
 * choice to fall to the next, each jumping past the others when it
 * matched.
 */
static int
regexp_compile_alternation(
	struct regexp_compiler *compiler,
	int32_t first,
	int backward)
{
	struct wb_vector jumps;
	uint32_t *jump;
	uint32_t split;
	uint32_t end;
	int32_t index;
	int32_t next;
	size_t count;
	int error;

	/* Each alternative. */
	wb_vector_init(&jumps, sizeof(uint32_t));
	error = 0;
	for (index = first; index >= 0 && error == 0; index = next) {
		next = regexp_node(compiler->parser, index)->next;

		/* The last alternative has no choice after it. */
		if (next < 0) {
			error = regexp_compile_node(compiler, index, backward);
			break;
		}

		/* SPLIT here, next alternative; the alternative; a jump past the rest. */
		error = regexp_emit(compiler, REGEXP_OP_SPLIT);
		if (error == 0)
			error = regexp_emit(compiler, regexp_here(compiler) + 2U);
		split = regexp_here(compiler);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = regexp_compile_node(compiler, index, backward);
		if (error == 0)
			error = regexp_emit(compiler, REGEXP_OP_JUMP);
		end = regexp_here(compiler);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = wb_vector_push(&jumps, &end);
		if (error == 0)
			regexp_patch(compiler, split, regexp_here(compiler));
	}

	/* The jumps land after the last alternative. */
	for (count = 0; count < jumps.count && error == 0; count++) {
		jump = wb_vector_at(&jumps, count);
		regexp_patch(compiler, *jump, regexp_here(compiler));
	}

	/* The list goes. */
	wb_vector_release(&jumps);
	if (error != 0)
		return error;

	/* Succeeded: the alternation's code is emitted. */
	return 0;
}

/*
 * Compiles a repetition: a body that always consumes and holds no capture
 * as plain choices (?, *, +), any other with a counter (the body's
 * captures reset at each iteration, an empty iteration past the minimum
 * failing).
 */
static int
regexp_compile_repeat(
	struct regexp_compiler *compiler,
	const struct regexp_node *node,
	int backward)
{
	uint32_t head;
	uint32_t split;
	uint32_t exit;
	uint32_t counter;
	int empty;
	int simple;
	int error;

	/* No repetition at all matches nothing. */
	if (node->max == 0)
		return 0;

	/* The plain forms. */
	empty = regexp_can_be_empty(compiler->parser, node->first);
	simple = 0;
	if (!empty && node->capture_first == node->capture_end)
		simple = 1;
	if (simple && node->min == 0 && node->max == 1) {
		/* x?: the body or not, in the order greediness says. */
		error = regexp_emit(compiler, REGEXP_OP_SPLIT);
		split = regexp_here(compiler);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = regexp_compile_node(compiler, node->first, backward);
		if (error != 0)
			return error;
		exit = regexp_here(compiler);
		if (node->greedy) {
			regexp_patch(compiler, split, split + 2U);
			regexp_patch(compiler, split + 1U, exit);
		} else {
			regexp_patch(compiler, split, exit);
			regexp_patch(compiler, split + 1U, split + 2U);
		}

		/* The optional body's code is emitted. */
		return 0;
	}

	/* x* and x+. */
	if (simple && node->max == REGEXP_INFINITY && node->min <= 1U) {
		/* x+ is x then x*; x* is a choice at the head of a loop. */
		error = 0;
		if (node->min == 1U)
			error = regexp_compile_node(compiler, node->first, backward);
		head = regexp_here(compiler);
		if (error == 0)
			error = regexp_emit(compiler, REGEXP_OP_SPLIT);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = regexp_emit(compiler, 0);
		if (error == 0)
			error = regexp_compile_node(compiler, node->first, backward);
		if (error == 0)
			error = regexp_emit(compiler, REGEXP_OP_JUMP);
		if (error == 0)
			error = regexp_emit(compiler, head);
		if (error != 0)
			return error;
		exit = regexp_here(compiler);
		if (node->greedy) {
			regexp_patch(compiler, head + 1U, head + 3U);
			regexp_patch(compiler, head + 2U, exit);
		} else {
			regexp_patch(compiler, head + 1U, exit);
			regexp_patch(compiler, head + 2U, head + 3U);
		}

		/* The loop's code is emitted. */
		return 0;
	}

	/* The counted loop: INIT, then at its head REPEAT decides, ENTER starts an iteration, NEXT ends it. */
	counter = compiler->counters;
	compiler->counters++;
	error = regexp_emit(compiler, REGEXP_OP_REPEAT_INIT);
	if (error == 0)
		error = regexp_emit(compiler, counter);
	head = regexp_here(compiler);
	if (error == 0)
		error = regexp_emit(compiler, REGEXP_OP_REPEAT);
	if (error == 0)
		error = regexp_emit(compiler, counter);
	if (error == 0)
		error = regexp_emit(compiler, node->min);
	if (error == 0)
		error = regexp_emit(compiler, node->max);
	if (error == 0)
		error = regexp_emit(compiler, (uint32_t)node->greedy);
	if (error == 0)
		error = regexp_emit(compiler, 0);
	if (error == 0)
		error = regexp_emit(compiler, REGEXP_OP_REPEAT_ENTER);
	if (error == 0)
		error = regexp_emit(compiler, counter);
	if (error == 0)
		error = regexp_emit(compiler, node->capture_first * 2U);
	if (error == 0)
		error = regexp_emit(compiler, node->capture_end * 2U);
	if (error == 0)
		error = regexp_compile_node(compiler, node->first, backward);
	if (error == 0)
		error = regexp_emit(compiler, REGEXP_OP_REPEAT_NEXT);
	if (error == 0)
		error = regexp_emit(compiler, counter);
	if (error == 0)
		error = regexp_emit(compiler, node->min);
	if (error == 0)
		error = regexp_emit(compiler, head);
	if (error != 0)
		return error;

	/* Succeeded: REPEAT's exit is after the loop. */
	regexp_patch(compiler, head + 5U, regexp_here(compiler));
	return 0;
}

/* Tells whether a node can match without consuming a character. */
static int
regexp_can_be_empty(
	const struct regexp_parser *parser,
	int32_t index)
{
	const struct regexp_node *node;
	int32_t child;
	int empty;

	/* Each kind of node. */
	node = regexp_node(parser, index);
	switch (node->kind) {
	case REGEXP_NODE_CHAR:
	case REGEXP_NODE_ANY:
	case REGEXP_NODE_CLASS:
		return 0;
	case REGEXP_NODE_SEQUENCE:
		/* Empty only when every term can be. */
		for (child = node->first; child >= 0; child = regexp_node(parser, child)->next) {
			empty = regexp_can_be_empty(parser, child);
			if (!empty)
				return 0;
		}

		/* Every term can match nothing. */
		return 1;
	case REGEXP_NODE_ALTERNATION:
		/* Empty when any alternative can be. */
		for (child = node->first; child >= 0; child = regexp_node(parser, child)->next) {
			empty = regexp_can_be_empty(parser, child);
			if (empty)
				return 1;
		}

		/* No alternative can match nothing. */
		return 0;
	case REGEXP_NODE_GROUP:
		empty = regexp_can_be_empty(parser, node->first);
		return empty;
	case REGEXP_NODE_REPEAT:
		if (node->min == 0)
			return 1;
		empty = regexp_can_be_empty(parser, node->first);
		return empty;
	default:
		break;
	}

	/* Assertions, lookarounds, references and the empty node. */
	return 1;
}

/* Tells whether a character is a word character for \b and \B. */
static int
regexp_is_word_char(
	const struct js_regexp_program *program,
	uint32_t code_point)
{
	/* The ASCII letters, digits and _. */
	if ((code_point >= 'a' && code_point <= 'z') || (code_point >= 'A' && code_point <= 'Z'))
		return 1;
	if ((code_point >= '0' && code_point <= '9') || code_point == '_')
		return 1;

	/* Under u and i, ſ and the Kelvin sign, which fold into them. */
	if ((program->flags & JS_REGEXP_UNICODE) != 0 && (program->flags & JS_REGEXP_IGNORE_CASE) != 0) {
		if (code_point == 0x017FU || code_point == 0x212AU)
			return 1;
	}

	/* Anything else. */
	return 0;
}

/* Tells whether a character ends a line (., ^ and $ under m). */
static int
regexp_is_line_terminator(
	uint32_t code_point)
{
	/* LF, CR, LS and PS. */
	if (code_point == 0x0AU || code_point == 0x0DU)
		return 1;
	if (code_point == 0x2028U || code_point == 0x2029U)
		return 1;

	/* Anything else. */
	return 0;
}

/*
 * Canonicalizes a character for case-insensitive matching: under u a
 * simple case folding (the simple lowercase of the simple uppercase; the
 * dotted and dotless i fold to nothing else), otherwise the one-unit
 * uppercase (never from beyond ASCII into it).
 */
static uint32_t
regexp_canonicalize(
	uint32_t code_point,
	int unicode)
{
	uint32_t mapped[WB_CASE_MAX];
	uint32_t upper;
	unsigned count;

	/* Under u: the simple folding. */
	if (unicode) {
		if (code_point == 0x0130U || code_point == 0x0131U)
			return code_point;
		upper = regexp_simple_map(wb_case_upper, wb_case_upper_count, code_point);
		upper = regexp_simple_map(wb_case_lower, wb_case_lower_count, upper);
		return upper;
	}

	/* Otherwise the uppercase, when it is one unit and does not fall into ASCII from beyond it. */
	count = wb_case_map(code_point, 1, mapped);
	if (count != 1U || mapped[0] > 0xFFFFU)
		return code_point;
	if (code_point >= 0x80U && mapped[0] < 0x80U)
		return code_point;

	/* The uppercase. */
	return mapped[0];
}

/* Maps a code point by a table of simple case mappings (itself when the table has none). */
static uint32_t
regexp_simple_map(
	const struct wb_case_simple *table,
	size_t count,
	uint32_t code_point)
{
	size_t low;
	size_t high;
	size_t middle;

	/* A binary search of the sorted table. */
	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (table[middle].code_point == code_point)
			return table[middle].mapping;
		if (table[middle].code_point < code_point) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* No mapping. */
	return code_point;
}

/* Tells whether a class's ranges hold a code point (negation aside). */
static int
regexp_class_has(
	const struct js_regexp_program *program,
	uint32_t class_index,
	uint32_t code_point)
{
	const struct regexp_class *class;
	const struct regexp_range *ranges;
	size_t low;
	size_t high;
	size_t middle;

	/* A binary search of the class's sorted ranges. */
	class = &program->classes[class_index];
	ranges = &program->ranges[class->range_first];
	low = 0;
	high = class->range_count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (code_point < ranges[middle].first) {
			high = middle;
		} else if (code_point > ranges[middle].last) {
			low = middle + 1U;
		} else {
			return 1;
		}
	}

	/* In no range. */
	return 0;
}

/*
 * Tells whether a class matches a character: when case does not matter,
 * the character, its canonical form or its simple lower- or uppercase in
 * the ranges counts.
 */
static int
regexp_class_matches(
	const struct js_regexp_program *program,
	uint32_t class_index,
	uint32_t code_point,
	int fold)
{
	uint32_t other;
	int has;

	/* The character itself, or its case variants when case does not matter. */
	has = regexp_class_has(program, class_index, code_point);
	if (!has && fold) {
		other = regexp_canonicalize(code_point, (program->flags & JS_REGEXP_UNICODE) != 0);
		has = regexp_class_has(program, class_index, other);
		if (!has) {
			other = regexp_simple_map(wb_case_lower, wb_case_lower_count, code_point);
			has = regexp_class_has(program, class_index, other);
		}

		/* The simple uppercase. */
		if (!has) {
			other = regexp_simple_map(wb_case_upper, wb_case_upper_count, code_point);
			has = regexp_class_has(program, class_index, other);
		}
	}

	/* A negated class matches what the ranges do not hold. */
	if (program->classes[class_index].negated) {
		if (has)
			return 0;
		return 1;
	}

	/* The ranges' answer. */
	return has;
}

/* Reads a code unit of the input. */
static uint32_t
regexp_input_unit(
	const struct js_regexp_input *input,
	size_t index)
{
	/* UTF-16 or Latin-1. */
	if (input->units != NULL)
		return input->units[index];

	/* A Latin-1 byte is its code unit. */
	return input->latin1[index];
}

/*
 * Reads the character after the position (or before it, backwards): a
 * code unit, or under u a surrogate pair's code point; 0 at the input's
 * end (or start).
 */
static int
regexp_read(
	const struct regexp_matcher *matcher,
	int backward,
	uint32_t *code_point,
	size_t *size)
{
	const struct js_regexp_input *input;
	uint32_t unit;
	uint32_t other;
	size_t position;
	int unicode;

	/* The unit next to the position. */
	input = matcher->input;
	position = matcher->position;
	unicode = (matcher->program->flags & JS_REGEXP_UNICODE) != 0;
	if (!backward) {
		if (position >= input->length)
			return 0;
		unit = regexp_input_unit(input, position);
		*code_point = unit;
		*size = 1;

		/* Under u a lead surrogate and its trail are one character. */
		if (unicode && unit >= 0xD800U && unit <= 0xDBFFU && position + 1U < input->length) {
			other = regexp_input_unit(input, position + 1U);
			if (other >= 0xDC00U && other <= 0xDFFFU) {
				*code_point = 0x10000U + ((unit - 0xD800U) << 10) + (other - 0xDC00U);
				*size = 2;
			}
		}

		/* A character is there. */
		return 1;
	}

	/* Backwards: the unit before the position. */
	if (position == 0)
		return 0;
	unit = regexp_input_unit(input, position - 1U);
	*code_point = unit;
	*size = 1;

	/* Under u a trail surrogate after its lead is one character. */
	if (unicode && unit >= 0xDC00U && unit <= 0xDFFFU && position >= 2U) {
		other = regexp_input_unit(input, position - 2U);
		if (other >= 0xD800U && other <= 0xDBFFU) {
			*code_point = 0x10000U + ((other - 0xD800U) << 10) + (unit - 0xDC00U);
			*size = 2;
		}
	}

	/* A character is there. */
	return 1;
}

/* Pushes an entry on the matcher's stack, growing it; E2BIG past its limit. */
static int
regexp_push(
	struct regexp_matcher *matcher,
	uint32_t kind,
	uint32_t index,
	uint32_t extra,
	size_t value)
{
	struct regexp_frame *grown;
	size_t capacity;

	/* Room for one more. */
	if (matcher->depth == matcher->capacity) {
		if (matcher->capacity >= REGEXP_STACK_MAX)
			return E2BIG;
		capacity = matcher->capacity * 2U;
		if (capacity == 0)
			capacity = 64;
		grown = realloc(matcher->stack, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		matcher->stack = grown;
		matcher->capacity = capacity;
	}

	/* Succeeded: the entry is on top. */
	matcher->stack[matcher->depth].kind = kind;
	matcher->stack[matcher->depth].index = index;
	matcher->stack[matcher->depth].extra = extra;
	matcher->stack[matcher->depth].value = value;
	matcher->depth++;
	return 0;
}

/* Sets a capture slot, logging its old value for backtracking. */
static int
regexp_set_slot(
	struct regexp_matcher *matcher,
	uint32_t slot,
	size_t value)
{
	int error;

	/* The old value, then the new. */
	error = regexp_push(matcher, REGEXP_FRAME_SLOT, slot, 0, matcher->slots[slot]);
	if (error != 0)
		return error;

	/* Succeeded: the slot holds the value. */
	matcher->slots[slot] = value;
	return 0;
}

/* Runs one instruction; *outcome says whether to go on, fail back or stop with a match. */
static int
regexp_step(
	struct regexp_matcher *matcher,
	int *outcome)
{
	const struct js_regexp_program *program;
	const uint32_t *words;
	uint32_t operation;
	uint32_t code_point;
	uint32_t before;
	size_t size;
	int backward;
	int present;
	int word_before;
	int word_after;
	int error;

	/* The instruction and its direction. */
	program = matcher->program;
	words = &program->code[matcher->pc];
	operation = words[0] & REGEXP_OP_MASK;
	backward = (words[0] & REGEXP_OP_BACKWARD) != 0;
	*outcome = REGEXP_GO;

	/* What the instruction does. */
	switch (operation) {
	case REGEXP_OP_CHAR:
	case REGEXP_OP_CHAR_FOLD:
	case REGEXP_OP_ANY:
	case REGEXP_OP_ANY_ALL:
	case REGEXP_OP_CLASS:
	case REGEXP_OP_CLASS_FOLD:
		/* A character must be there, and be what the instruction wants. */
		present = regexp_read(matcher, backward, &code_point, &size);
		if (!present) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* A character must be that character. */
		if (operation == REGEXP_OP_CHAR && code_point != words[1]) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* Or that character canonically. */
		if (operation == REGEXP_OP_CHAR_FOLD) {
			code_point = regexp_canonicalize(code_point, (program->flags & JS_REGEXP_UNICODE) != 0);
			if (code_point != words[1]) {
				*outcome = REGEXP_FAIL;
				return 0;
			}
		}

		/* Any character but a line terminator. */
		if (operation == REGEXP_OP_ANY) {
			present = regexp_is_line_terminator(code_point);
			if (present) {
				*outcome = REGEXP_FAIL;
				return 0;
			}
		}

		/* A character the class matches. */
		if (operation == REGEXP_OP_CLASS || operation == REGEXP_OP_CLASS_FOLD) {
			present = regexp_class_matches(program, words[1], code_point, operation == REGEXP_OP_CLASS_FOLD);
			if (!present) {
				*outcome = REGEXP_FAIL;
				return 0;
			}
		}

		/* The character is taken. */
		if (backward) {
			matcher->position -= size;
		} else {
			matcher->position += size;
		}

		/* The next instruction. */
		matcher->pc += 1U + regexp_operands[operation];
		return 0;
	case REGEXP_OP_LINE_START:
		/* The input's start, or under m after a line terminator. */
		if (matcher->position != 0) {
			if ((program->flags & JS_REGEXP_MULTILINE) == 0) {
				*outcome = REGEXP_FAIL;
				return 0;
			}

			/* Under m, the unit before must end a line. */
			before = regexp_input_unit(matcher->input, matcher->position - 1U);
			present = regexp_is_line_terminator(before);
			if (!present) {
				*outcome = REGEXP_FAIL;
				return 0;
			}
		}

		/* The next instruction. */
		matcher->pc++;
		return 0;
	case REGEXP_OP_LINE_END:
		/* The input's end, or under m before a line terminator. */
		if (matcher->position != matcher->input->length) {
			if ((program->flags & JS_REGEXP_MULTILINE) == 0) {
				*outcome = REGEXP_FAIL;
				return 0;
			}

			/* Under m, the unit after must end a line. */
			before = regexp_input_unit(matcher->input, matcher->position);
			present = regexp_is_line_terminator(before);
			if (!present) {
				*outcome = REGEXP_FAIL;
				return 0;
			}
		}

		/* The next instruction. */
		matcher->pc++;
		return 0;
	case REGEXP_OP_WORD_BOUNDARY:
	case REGEXP_OP_NOT_WORD_BOUNDARY:
		/* Whether a word character is on each side. */
		word_before = 0;
		if (matcher->position != 0) {
			before = regexp_input_unit(matcher->input, matcher->position - 1U);
			word_before = regexp_is_word_char(program, before);
		}

		/* The unit after the position. */
		word_after = 0;
		if (matcher->position < matcher->input->length) {
			before = regexp_input_unit(matcher->input, matcher->position);
			word_after = regexp_is_word_char(program, before);
		}

		/* A boundary is where the two sides differ. */
		if ((word_before != word_after) != (operation == REGEXP_OP_WORD_BOUNDARY)) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* The next instruction. */
		matcher->pc++;
		return 0;
	case REGEXP_OP_SPLIT:
		/* The first way now, the second on failure. */
		error = regexp_push(matcher, REGEXP_FRAME_CHOICE, words[2], 0, matcher->position);
		if (error != 0)
			return error;
		matcher->pc = words[1];
		return 0;
	case REGEXP_OP_JUMP:
		matcher->pc = words[1];
		return 0;
	case REGEXP_OP_SAVE:
		error = regexp_set_slot(matcher, words[1], matcher->position);
		if (error != 0)
			return error;
		matcher->pc += 2U;
		return 0;
	case REGEXP_OP_BACKREF:
		error = regexp_step_backref(matcher, words, outcome);
		return error;
	case REGEXP_OP_LOOK:
		/* The body runs above a barrier that remembers the position and where the lookaround ends. */
		error = regexp_push(matcher, REGEXP_FRAME_BARRIER, words[2], words[1], matcher->position);
		if (error != 0)
			return error;
		matcher->pc += 3U;
		return 0;
	case REGEXP_OP_LOOK_END:
		regexp_look_end(matcher, outcome);
		return 0;
	case REGEXP_OP_REPEAT_INIT:
	case REGEXP_OP_REPEAT:
	case REGEXP_OP_REPEAT_ENTER:
	case REGEXP_OP_REPEAT_NEXT:
		error = regexp_step_repeat(matcher, words, outcome);
		return error;
	case REGEXP_OP_MATCH:
		*outcome = REGEXP_MATCHED;
		return 0;
	default:
		break;
	}

	/* An instruction the compiler never makes. */
	return EINVAL;
}

/* Runs the instructions of a counted loop. */
static int
regexp_step_repeat(
	struct regexp_matcher *matcher,
	const uint32_t *words,
	int *outcome)
{
	uint32_t counter;
	uint32_t slot;
	size_t count;
	int error;

	/* Which loop. */
	counter = words[1];
	count = matcher->counters[counter];

	/* What the instruction does. */
	switch (words[0] & REGEXP_OP_MASK) {
	case REGEXP_OP_REPEAT_INIT:
		/* No iteration yet. */
		error = regexp_push(matcher, REGEXP_FRAME_COUNTER, counter, 0, count);
		if (error != 0)
			return error;
		matcher->counters[counter] = 0;
		matcher->pc += 2U;
		return 0;
	case REGEXP_OP_REPEAT:
		/* Below the minimum another iteration must come; at the maximum none may; between, greediness chooses. */
		if (count < words[2]) {
			matcher->pc += 6U;
			return 0;
		}

		/* At the maximum the loop ends. */
		if (words[3] != REGEXP_INFINITY && count >= words[3]) {
			matcher->pc = words[5];
			return 0;
		}

		/* A greedy loop tries another iteration first. */
		if (words[4] != 0) {
			error = regexp_push(matcher, REGEXP_FRAME_CHOICE, words[5], 0, matcher->position);
			if (error != 0)
				return error;
			matcher->pc += 6U;
			return 0;
		}

		/* A lazy loop tries what follows first. */
		error = regexp_push(matcher, REGEXP_FRAME_CHOICE, matcher->pc + 6U, 0, matcher->position);
		if (error != 0)
			return error;
		matcher->pc = words[5];
		return 0;
	case REGEXP_OP_REPEAT_ENTER:
		/* The iteration's start, and the body's captures cleared. */
		error = regexp_push(matcher, REGEXP_FRAME_START, counter, 0, matcher->starts[counter]);
		if (error != 0)
			return error;
		matcher->starts[counter] = matcher->position;
		for (slot = words[2]; slot < words[3]; slot++) {
			if (matcher->slots[slot] == JS_REGEXP_UNSET)
				continue;
			error = regexp_set_slot(matcher, slot, JS_REGEXP_UNSET);
			if (error != 0)
				return error;
		}

		/* The body follows. */
		matcher->pc += 4U;
		return 0;
	case REGEXP_OP_REPEAT_NEXT:
		/* An iteration that matched nothing fails once the minimum is reached; otherwise it counts. */
		if (count >= words[2] && matcher->position == matcher->starts[counter]) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* The iteration counts; the loop's head decides again. */
		error = regexp_push(matcher, REGEXP_FRAME_COUNTER, counter, 0, count);
		if (error != 0)
			return error;
		matcher->counters[counter] = count + 1U;
		matcher->pc = words[3];
		return 0;
	default:
		break;
	}

	/* Not a loop's instruction. */
	return EINVAL;
}

/*
 * Matches a back reference: what the capture matched, again at the
 * position (before it, backwards); a capture that did not take part
 * matches nothing.
 */
static int
regexp_step_backref(
	struct regexp_matcher *matcher,
	const uint32_t *words,
	int *outcome)
{
	const struct js_regexp_input *input;
	size_t start;
	size_t end;
	size_t length;
	size_t at;
	size_t index;
	uint32_t left;
	uint32_t right;
	int backward;
	int fold;
	int unicode;

	/* The capture's text. */
	input = matcher->input;
	start = matcher->slots[words[1] * 2U];
	end = matcher->slots[words[1] * 2U + 1U];
	matcher->pc += 2U;
	if (start == JS_REGEXP_UNSET || end == JS_REGEXP_UNSET || end < start)
		return 0;
	length = end - start;

	/* The same units next to the position (canonically equal when case does not matter). */
	backward = (words[0] & REGEXP_OP_BACKWARD) != 0;
	fold = (matcher->program->flags & JS_REGEXP_IGNORE_CASE) != 0;
	unicode = (matcher->program->flags & JS_REGEXP_UNICODE) != 0;
	if (backward) {
		if (matcher->position < length) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* The reference ends at the position. */
		at = matcher->position - length;
	} else {
		if (input->length - matcher->position < length) {
			*outcome = REGEXP_FAIL;
			return 0;
		}

		/* The reference starts at the position. */
		at = matcher->position;
	}

	/* Each unit must be the capture's. */
	for (index = 0; index < length; index++) {
		left = regexp_input_unit(input, start + index);
		right = regexp_input_unit(input, at + index);
		if (left == right)
			continue;
		if (fold) {
			left = regexp_canonicalize(left, unicode);
			right = regexp_canonicalize(right, unicode);
			if (left == right)
				continue;
		}

		/* A unit that differs. */
		*outcome = REGEXP_FAIL;
		return 0;
	}

	/* The position moves past the text (before it, backwards). */
	if (backward) {
		matcher->position = at;
	} else {
		matcher->position = at + length;
	}

	/* Succeeded: the text is taken. */
	return 0;
}

/*
 * Ends a lookaround's body, which matched: a positive lookaround keeps
 * the captures its body made but not its choice points (it is atomic) and
 * goes on at its start position after it; a negative one fails, its
 * body's captures undone.
 */
static void
regexp_look_end(
	struct regexp_matcher *matcher,
	int *outcome)
{
	struct regexp_frame barrier;
	struct regexp_frame *frame;
	size_t index;
	size_t read;
	size_t write;

	/* The barrier of this lookaround is the nearest one. */
	index = matcher->depth;
	while (index > 0) {
		index--;
		if (matcher->stack[index].kind == REGEXP_FRAME_BARRIER)
			break;
	}

	/* The barrier's saved position and end. */
	barrier = matcher->stack[index];

	/* A negative lookaround: its body matched, so it fails; the body's changes are undone first. */
	if (barrier.extra == REGEXP_LOOK_NOT_AHEAD || barrier.extra == REGEXP_LOOK_NOT_BEHIND) {
		while (matcher->depth > index + 1U) {
			matcher->depth--;
			frame = &matcher->stack[matcher->depth];
			if (frame->kind == REGEXP_FRAME_SLOT)
				matcher->slots[frame->index] = frame->value;
			else if (frame->kind == REGEXP_FRAME_COUNTER)
				matcher->counters[frame->index] = frame->value;
			else if (frame->kind == REGEXP_FRAME_START)
				matcher->starts[frame->index] = frame->value;
		}

		/* The barrier goes too. */
		matcher->depth = index;
		*outcome = REGEXP_FAIL;
		return;
	}

	/* A positive one: the logged changes stay (for later failures to undo), the choices and the barrier go. */
	write = index;
	for (read = index + 1U; read < matcher->depth; read++) {
		if (matcher->stack[read].kind == REGEXP_FRAME_CHOICE || matcher->stack[read].kind == REGEXP_FRAME_BARRIER)
			continue;
		matcher->stack[write] = matcher->stack[read];
		write++;
	}

	/* The stack ends after what stays. */
	matcher->depth = write;
	matcher->position = barrier.value;
	matcher->pc = barrier.index;
}

/*
 * Fails back to the latest choice point, undoing the logged changes on
 * the way; 0 when there is none left.  A negative lookaround's barrier
 * reached this way means its body could not match: the lookaround holds.
 */
static int
regexp_backtrack(
	struct regexp_matcher *matcher)
{
	struct regexp_frame *frame;

	/* Each entry from the top. */
	while (matcher->depth > 0) {
		matcher->depth--;
		frame = &matcher->stack[matcher->depth];

		/* What the entry is. */
		switch (frame->kind) {
		case REGEXP_FRAME_SLOT:
			matcher->slots[frame->index] = frame->value;
			break;
		case REGEXP_FRAME_COUNTER:
			matcher->counters[frame->index] = frame->value;
			break;
		case REGEXP_FRAME_START:
			matcher->starts[frame->index] = frame->value;
			break;
		case REGEXP_FRAME_CHOICE:
			/* The other way from here. */
			matcher->pc = frame->index;
			matcher->position = frame->value;
			return 1;
		default:
			/* A negative lookaround whose body failed holds; a positive one fails further back. */
			if (frame->extra == REGEXP_LOOK_NOT_AHEAD || frame->extra == REGEXP_LOOK_NOT_BEHIND) {
				matcher->pc = frame->index;
				matcher->position = frame->value;
				return 1;
			}

			break;
		}
	}

	/* No choice left. */
	return 0;
}

/* Runs the program from one position; *matched says whether it matched (the slots then hold the captures). */
static int
regexp_attempt(
	struct regexp_matcher *matcher,
	size_t start,
	int *matched)
{
	size_t slot;
	int outcome;
	int resumed;
	int error;

	/* No capture yet, an empty stack, the program's start. */
	for (slot = 0; slot < (size_t)matcher->program->capture_count * 2U; slot++)
		matcher->slots[slot] = JS_REGEXP_UNSET;
	matcher->depth = 0;
	matcher->pc = 0;
	matcher->position = start;
	*matched = 0;

	/* Steps until a match or no choice is left. */
	for (;;) {
		matcher->steps++;
		if (matcher->steps > REGEXP_STEPS_MAX)
			return E2BIG;

		/* Now and then, the input's owner says whether the match goes on. */
		if ((matcher->steps & REGEXP_POLL_MASK) == 0U && matcher->input->poll != NULL) {
			error = matcher->input->poll(matcher->input->poll_context);
			if (error != 0)
				return error;
		}

		/* One step of the program. */
		error = regexp_step(matcher, &outcome);
		if (error != 0)
			return error;
		if (outcome == REGEXP_MATCHED)
			break;
		if (outcome == REGEXP_FAIL) {
			resumed = regexp_backtrack(matcher);
			if (!resumed)
				return 0;
		}
	}

	/* Succeeded: the whole match is capture 0. */
	matcher->slots[0] = start;
	matcher->slots[1] = matcher->position;
	*matched = 1;
	return 0;
}
