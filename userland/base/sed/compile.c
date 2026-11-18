/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compiles a sed script into its array of commands.
 *
 * A command is read as [address[,address]][!]name[arguments], with blanks
 * allowed around the addresses and before the name; commands are separated
 * by newlines or semicolons.  The text of a, i and c, the file of r and w,
 * and a label run to the end of their line (a label also ends at a
 * semicolon, as GNU sed reads it).
 *
 * A regular expression is turned into one regcomp reads: the delimiter,
 * escaped, stands for itself, \n is a newline, and a bracket expression is
 * copied whole (a delimiter inside one does not end the expression).  An
 * empty regular expression stands for the last one used, at run time.
 *
 * GNU's escapes that make a byte (\a, \f, \n, \r, \t, \v, \dNNN, \oNNN,
 * \xHH and \cX) are made into the byte in a regex (inside a bracket
 * expression too), a replacement, a string of y and the text of a, i and
 * c.  In a regex the byte keeps its meaning (as in GNU sed); in a
 * replacement & and a backslash made so stand for themselves.  \` and \'
 * (the start and the end of the pattern space) become ^ and $.
 */

#include "userland/base/sed/sed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What read_delimited reads. */
#define KIND_REGEX		0
#define KIND_REPLACEMENT	1
#define KIND_TRANSLATION	2

/* A string being built. */
struct text_buffer {
	char *data;
	size_t length;
	size_t capacity;
};

/* The state of the compilation. */
struct parser {
	const char *text;
	size_t position;
	struct sed_program *program;

	/* The indexes of the { commands not yet closed. */
	size_t *blocks;
	size_t depth;
	size_t block_capacity;
};

static void parse_command(struct parser *parser);
static size_t add_command(struct parser *parser);
static void parse_addresses(struct parser *parser, struct sed_command *command);
static void parse_name(struct parser *parser, size_t index);
static void parse_block_end(struct parser *parser, size_t index);
static void parse_comment(struct parser *parser, size_t index);
static int parse_address(struct parser *parser, struct sed_address *address);
static int parse_second_address(struct parser *parser, struct sed_address *address);
static unsigned long parse_number(struct parser *parser);
static void parse_regex_address(struct parser *parser, char delimiter, struct sed_address *address);
static void read_delimited(struct parser *parser, char delimiter, int kind, struct text_buffer *out);
static void read_escape(struct parser *parser, char delimiter, int kind, struct text_buffer *out);
static int make_escape(struct parser *parser, char letter, char *made);
static int escape_digits(struct parser *parser, unsigned int base, size_t most, unsigned int *value);
static void add_made(struct text_buffer *out, char made, int kind);
static void copy_bracket(struct parser *parser, struct text_buffer *out);
static regex_t *compile_regex(struct parser *parser, const char *text, int ignore_case, int multiline);
static char *parse_text(struct parser *parser);
static char *parse_label(struct parser *parser);
static char *parse_filename(struct parser *parser);
static char *parse_command_line(struct parser *parser);
static struct sed_reader *find_reader(struct parser *parser, const char *name);
static void parse_substitute(struct parser *parser, struct sed_command *command);
static void parse_substitute_flags(struct parser *parser, struct sed_command *command, int *ignore_case, int *multiline);
static void check_references(struct parser *parser, const struct sed_substitute *substitute);
static void parse_translation(struct parser *parser, struct sed_command *command);
static struct sed_output *open_output(struct parser *parser, const char *name);
static void end_command(struct parser *parser);
static void check_sandbox(const struct parser *parser);
static void skip_blanks(struct parser *parser);
static char peek(const struct parser *parser);
static void resolve_labels(struct parser *parser);
static void compile_error(const struct parser *parser, const char *message);
static void buffer_add(struct text_buffer *buffer, char value);
static char *buffer_finish(struct text_buffer *buffer);
static int is_regex_special(char value);
static int is_digit(char value);

/*
 * Compiles a script.  An error in it ends sed with a message.
 */
int
sed_compile(
	const char *script,
	struct sed_program *program)
{
	struct parser parser;

	/* A script that begins with #n is as -n. */
	if (script[0] == '#' && script[1] == 'n' &&
	    (script[2] == '\n' || script[2] == '\0'))
		program->quiet = 1;

	/* The commands, one after another. */
	memset(&parser, 0, sizeof(parser));
	parser.text = script;
	parser.program = program;
	while (parser.text[parser.position] != '\0')
		parse_command(&parser);

	/* Every { needs its }. */
	if (parser.depth != 0)
		compile_error(&parser, "unmatched `{'");

	/* Succeeded: the branches know their labels. */
	resolve_labels(&parser);
	free(parser.blocks);
	return 1;
}

/* Parses one command, or the separators before the end of the script. */
static void
parse_command(
	struct parser *parser)
{
	size_t index;
	char value;

	/* Blanks, newlines and semicolons separate commands. */
	for (;;) {
		value = peek(parser);
		if (value != ' ' && value != '\t' && value != '\n' &&
		    value != ';')
			break;
		parser->position++;
	}

	/* The end of the script ends the commands. */
	value = peek(parser);
	if (value == '\0')
		return;

	/* The addresses, a !, and the command itself. */
	index = add_command(parser);
	parse_addresses(parser, &parser->program->commands[index]);
	parse_name(parser, index);
}

/* Adds an empty command to the program and returns its index. */
static size_t
add_command(
	struct parser *parser)
{
	struct sed_program *program;
	size_t index;

	/* Room for one more. */
	program = parser->program;
	if (program->count == program->capacity) {
		program->capacity = program->capacity * 2U + 16U;
		program->commands = sed_realloc(program->commands,
		    program->capacity * sizeof(*program->commands));
	}

	/* Succeeded: a command with no address. */
	index = program->count;
	program->count++;
	memset(&program->commands[index], 0, sizeof(program->commands[index]));
	return index;
}

/* Parses the addresses of a command and the ! that negates them. */
static void
parse_addresses(
	struct parser *parser,
	struct sed_command *command)
{
	int found;
	char value;

	/* The first address, and the second after a comma. */
	found = parse_address(parser, &command->first);
	skip_blanks(parser);
	value = peek(parser);
	if (found && value == ',') {
		parser->position++;
		skip_blanks(parser);
		found = parse_second_address(parser, &command->second);
		if (!found)
			compile_error(parser, "unexpected `,'");
		skip_blanks(parser);
	}

	/* A second address of 0 is no line at all. */
	if (command->second.kind == SED_ADDRESS_ZERO)
		compile_error(parser, "invalid usage of line address 0");

	/* 0 only starts 0,/re/, whose range is open before the first line. */
	if (command->first.kind == SED_ADDRESS_ZERO) {
		if (command->second.kind != SED_ADDRESS_REGEX)
			compile_error(parser, "invalid usage of line address 0");
		command->in_range = 1;
	}

	/* ! selects the lines the addresses do not. */
	for (;;) {
		value = peek(parser);
		if (value != '!')
			break;
		command->negate = 1;
		parser->position++;
		skip_blanks(parser);
	}
}

/* Parses the name of a command and its arguments. */
static void
parse_name(
	struct parser *parser,
	size_t index)
{
	struct sed_command *command;
	char *label;
	char name;
	char value;
	int digit;

	/* The command's slot, and its name. */
	command = &parser->program->commands[index];
	name = peek(parser);
	if (name == '\0')
		compile_error(parser, "missing command");
	parser->position++;
	command->name = name;

	/* The arguments the command takes. */
	switch (name) {
	case '{':
		/* A block: its } is found later. */
		if (parser->depth == parser->block_capacity) {
			parser->block_capacity = parser->block_capacity * 2U + 8U;
			parser->blocks = sed_realloc(parser->blocks,
			    parser->block_capacity * sizeof(*parser->blocks));
		}

		/* The block's place, until its } is found. */
		parser->blocks[parser->depth] = index;
		parser->depth++;
		return;
	case '}':
		parse_block_end(parser, index);
		break;
	case '#':
		parse_comment(parser, index);
		return;
	case ':':
		if (command->first.kind != SED_ADDRESS_NONE)
			compile_error(parser, ": doesn't want any addresses");
		command->text = parse_label(parser);
		if (command->text == NULL)
			compile_error(parser, "\":\" lacks a label");
		break;
	case 'a':
	case 'i':
	case 'c':
		command->text = parse_text(parser);
		return;
	case 'b':
	case 't':
	case 'T':
		command->text = parse_label(parser);
		break;
	case 'v':
		/* GNU's v [version]: the version asked for is not checked. */
		label = parse_label(parser);
		free(label);
		break;
	case 'e':
		/* GNU's e [command]: the rest of the line. */
		check_sandbox(parser);
		command->text = parse_command_line(parser);
		return;
	case 'R':
		/* GNU's R file: a line of the file at a time. */
		check_sandbox(parser);
		command->text = parse_filename(parser);
		command->reader = find_reader(parser, command->text);
		return;
	case 'W':
		/* GNU's W file: the first line of the pattern space. */
		check_sandbox(parser);
		command->text = parse_filename(parser);
		command->output = open_output(parser, command->text);
		return;
	case 'r':
		check_sandbox(parser);
		command->text = parse_filename(parser);
		return;
	case 'w':
		check_sandbox(parser);
		command->text = parse_filename(parser);
		command->output = open_output(parser, command->text);
		return;
	case 's':
		/* A w flag takes the rest of the line, separator and all. */
		parse_substitute(parser, command);
		if (command->substitute.output != NULL)
			return;
		break;
	case 'y':
		parse_translation(parser, command);
		break;
	case 'l':
	case 'q':
	case 'Q':
		/* An optional number: the line length, or the exit status. */
		skip_blanks(parser);
		value = peek(parser);
		digit = is_digit(value);
		if (digit) {
			command->exit_status = (int)parse_number(parser);
			command->number_given = 1;
		}

		/* Only a separator may follow. */
		break;
	case '=':
	case 'd':
	case 'D':
	case 'g':
	case 'G':
	case 'h':
	case 'H':
	case 'n':
	case 'N':
	case 'p':
	case 'P':
	case 'x':
	case 'F':
	case 'z':
		break;
	default:
		compile_error(parser, "unknown command");
	}

	/* Only a separator may follow. */
	end_command(parser);
}

/* Parses a }: it ends the innermost block. */
static void
parse_block_end(
	struct parser *parser,
	size_t index)
{
	size_t open;

	/* A } takes no address and needs an open block. */
	if (parser->program->commands[index].first.kind != SED_ADDRESS_NONE)
		compile_error(parser, "} doesn't want any addresses");
	if (parser->depth == 0)
		compile_error(parser, "unexpected `}'");

	/* The { skips to it when its addresses do not select a line. */
	parser->depth--;
	open = parser->blocks[parser->depth];
	parser->program->commands[open].jump = index;
}

/* Skips a comment to the end of its line; the comment is no command. */
static void
parse_comment(
	struct parser *parser,
	size_t index)
{
	char value;

	/* A comment takes no address. */
	if (parser->program->commands[index].first.kind != SED_ADDRESS_NONE)
		compile_error(parser, "comments don't accept any addresses");

	/* The rest of the line. */
	for (;;) {
		value = peek(parser);
		if (value == '\0' || value == '\n')
			break;
		parser->position++;
	}

	/* The comment takes no slot among the commands. */
	parser->program->count--;
}

/*
 * Parses an address: a line number, $, /RE/ or \cREc.  Returns 0 when
 * there is none.
 */
static int
parse_address(
	struct parser *parser,
	struct sed_address *address)
{
	char value;
	char delimiter;
	int digit;

	/* The character the address starts with. */
	value = peek(parser);

	/* A line number, GNU's first~step, or the 0 of 0,/re/. */
	digit = is_digit(value);
	if (digit) {
		address->kind = SED_ADDRESS_LINE;
		address->line = parse_number(parser);
		value = peek(parser);
		if (value == '~') {
			parser->position++;
			address->kind = SED_ADDRESS_STEP;
			address->step = parse_number(parser);
			return 1;
		}

		/* 0 is checked with the address after it. */
		if (address->line == 0)
			address->kind = SED_ADDRESS_ZERO;
		return 1;
	}

	/* The last line. */
	if (value == '$') {
		address->kind = SED_ADDRESS_LAST;
		parser->position++;
		return 1;
	}

	/* /RE/. */
	if (value == '/') {
		parser->position++;
		parse_regex_address(parser, '/', address);
		return 1;
	}

	/* \cREc, with any delimiter but a backslash or a newline. */
	if (value == '\\') {
		parser->position++;
		delimiter = peek(parser);
		if (delimiter == '\0' || delimiter == '\n' || delimiter == '\\')
			compile_error(parser, "unexpected `,'");
		parser->position++;
		parse_regex_address(parser, delimiter, address);
		return 1;
	}

	/* No address. */
	return 0;
}

/*
 * Parses the second address of a range: an address, or GNU's +N (N lines
 * more) or ~N (up to the next line whose number is a multiple of N).
 * Returns 0 when there is none.
 */
static int
parse_second_address(
	struct parser *parser,
	struct sed_address *address)
{
	char value;
	int digit;
	int found;

	/* +N and ~N. */
	value = peek(parser);
	if (value == '+' || value == '~') {
		parser->position++;
		address->kind = SED_ADDRESS_PLUS;
		if (value == '~')
			address->kind = SED_ADDRESS_MULTIPLE;
		value = peek(parser);
		digit = is_digit(value);
		if (!digit)
			compile_error(parser, "expected number after `+' or `~'");
		address->line = parse_number(parser);
		return 1;
	}

	/* Succeeded: an address of the first kind, or none. */
	found = parse_address(parser, address);
	return found;
}

/* Parses a decimal number. */
static unsigned long
parse_number(
	struct parser *parser)
{
	unsigned long number;
	char value;
	int digit;

	/* The digits. */
	number = 0;
	for (;;) {
		value = peek(parser);
		digit = is_digit(value);
		if (!digit)
			break;
		number = number * 10UL + (unsigned long)(value - '0');
		parser->position++;
	}

	/* Succeeded. */
	return number;
}

/*
 * Parses the regex of an address after its opening delimiter, and the
 * GNU I flag after it.
 */
static void
parse_regex_address(
	struct parser *parser,
	char delimiter,
	struct sed_address *address)
{
	struct text_buffer text;
	int ignore_case;
	int multiline;
	char value;

	/* The regex. */
	memset(&text, 0, sizeof(text));
	read_delimited(parser, delimiter, KIND_REGEX, &text);
	buffer_finish(&text);

	/* I: case is ignored.  M: ^ and $ match at each newline too. */
	ignore_case = 0;
	multiline = 0;
	for (;;) {
		value = peek(parser);
		if (value != 'I' && value != 'M')
			break;
		if (value == 'I')
			ignore_case = 1;
		if (value == 'M')
			multiline = 1;
		parser->position++;
	}

	/* Succeeded: the compiled regex, or NULL for the last one used. */
	address->kind = SED_ADDRESS_REGEX;
	address->regex = compile_regex(parser, text.data, ignore_case,
				       multiline);
	free(text.data);
}

/*
 * Reads up to an unescaped delimiter, which is taken but not kept.  What
 * is read is a regex, the replacement of s, or a string of y.
 */
static void
read_delimited(
	struct parser *parser,
	char delimiter,
	int kind,
	struct text_buffer *out)
{
	char value;

	/* Each character up to the delimiter. */
	for (;;) {
		/* A newline or the end of the script leaves it unterminated. */
		value = peek(parser);
		if (value == '\0' || value == '\n')
			compile_error(parser, "unterminated `s' command");

		/* The delimiter ends it. */
		if (value == delimiter) {
			parser->position++;
			break;
		}

		/* An escape. */
		if (value == '\\') {
			read_escape(parser, delimiter, kind, out);
			continue;
		}

		/* A bracket expression of a regex is copied whole. */
		if (value == '[' && kind == KIND_REGEX) {
			copy_bracket(parser, out);
			continue;
		}

		/* An ordinary character. */
		buffer_add(out, value);
		parser->position++;
	}
}

/*
 * Reads a backslash and the character after it.  An escaped delimiter is
 * the delimiter itself; \n (and a backslash-newline) is a newline, and \t a
 * tab.  Anything else is kept escaped for regcomp or the replacement.
 */
static void
read_escape(
	struct parser *parser,
	char delimiter,
	int kind,
	struct text_buffer *out)
{
	char next;
	char made;
	int made_byte;
	int special;

	/* The character after the backslash. */
	next = parser->text[parser->position + 1];
	if (next == '\0')
		compile_error(parser, "unterminated `s' command");
	parser->position += 2;

	/* The delimiter: literal, and still escaped if it means something. */
	if (next == delimiter) {
		special = 0;
		if (kind == KIND_REGEX)
			special = is_regex_special(next);
		if (kind == KIND_REPLACEMENT && next == '&')
			special = 1;
		if (special)
			buffer_add(out, '\\');
		buffer_add(out, next);
		return;
	}

	/* A backslash-newline is a newline. */
	if (next == '\n') {
		buffer_add(out, '\n');
		return;
	}

	/* An escape that makes a byte. */
	made_byte = make_escape(parser, next, &made);
	if (made_byte) {
		add_made(out, made, kind);
		return;
	}

	/* \` in a regex: the start of the pattern space. */
	if (kind == KIND_REGEX && next == '`') {
		buffer_add(out, '^');
		return;
	}

	/* \' in a regex: the end of the pattern space. */
	if (kind == KIND_REGEX && next == '\'') {
		buffer_add(out, '$');
		return;
	}

	/* In y, \\ is a backslash and any other escape the character. */
	if (kind == KIND_TRANSLATION) {
		buffer_add(out, next);
		return;
	}

	/* Otherwise the escape is kept for regcomp or the replacement. */
	buffer_add(out, '\\');
	buffer_add(out, next);
}

/*
 * Makes the byte of one of GNU's escapes, whose letter is at the position
 * before the parser's (\a \f \n \r \t \v, \dNNN decimal, \oNNN octal,
 * \xHH hexadecimal, \cX control).  The digits after the letter are taken.
 * Returns 0 when the letter makes no byte.
 */
static int
make_escape(
	struct parser *parser,
	char letter,
	char *made)
{
	unsigned int value;
	char control;
	int found;

	/* The escapes of one letter. */
	switch (letter) {
	case 'a':
		*made = '\a';
		return 1;
	case 'f':
		*made = '\f';
		return 1;
	case 'n':
		*made = '\n';
		return 1;
	case 'r':
		*made = '\r';
		return 1;
	case 't':
		*made = '\t';
		return 1;
	case 'v':
		*made = '\v';
		return 1;
	case 'd':
		found = escape_digits(parser, 10U, 3U, &value);
		break;
	case 'o':
		found = escape_digits(parser, 8U, 3U, &value);
		break;
	case 'x':
		found = escape_digits(parser, 16U, 2U, &value);
		break;
	case 'c':
		/* \cX: X as a control character. */
		control = peek(parser);
		if (control == '\0' || control == '\n')
			return 0;
		parser->position++;
		if (control >= 'a' && control <= 'z')
			control = (char)(control - 'a' + 'A');
		*made = (char)(control ^ 0x40);
		return 1;
	default:
		return 0;
	}

	/* A number escape without its digits makes nothing. */
	if (!found)
		return 0;

	/* Succeeded: the byte of the number. */
	*made = (char)(unsigned char)value;
	return 1;
}

/*
 * Reads up to most digits of a base at the parser's position.  Returns 0
 * when there is not even one.
 */
static int
escape_digits(
	struct parser *parser,
	unsigned int base,
	size_t most,
	unsigned int *value)
{
	unsigned int digit;
	size_t count;
	char next;

	/* Each digit, up to the most. */
	*value = 0;
	for (count = 0; count < most; count++) {
		/* The digit's value, or the end of the number. */
		next = peek(parser);
		if (next >= '0' && next <= '9') {
			digit = (unsigned int)(next - '0');
		} else if (next >= 'a' && next <= 'f') {
			digit = (unsigned int)(next - 'a' + 10);
		} else if (next >= 'A' && next <= 'F') {
			digit = (unsigned int)(next - 'A' + 10);
		} else {
			break;
		}

		/* A digit of another base ends the number. */
		if (digit >= base)
			break;
		*value = *value * base + digit;
		parser->position++;
	}

	/* Succeeded: whether there was a digit. */
	if (count == 0)
		return 0;
	return 1;
}

/*
 * Adds a byte an escape made.  In a regex the byte keeps what it means
 * there (GNU's \x2e is any character), except a backslash; in a
 * replacement & and the backslash stand for themselves.
 */
static void
add_made(
	struct text_buffer *out,
	char made,
	int kind)
{
	int special;

	/* Whether the byte is escaped where it goes. */
	special = 0;
	if (kind == KIND_REGEX && made == '\\')
		special = 1;
	if (kind == KIND_REPLACEMENT && (made == '&' || made == '\\'))
		special = 1;

	/* Succeeded: the byte, escaped when it must be. */
	if (special)
		buffer_add(out, '\\');
	buffer_add(out, made);
}

/*
 * Copies a bracket expression: [ then an optional ^ or !, a ] that is first
 * in the set, and everything up to the closing ], including [:class:],
 * [=equivalence=] and [.collating.] elements.  GNU's escapes that make a
 * byte (such as \t and \n) are made into it here too.
 */
static void
copy_bracket(
	struct parser *parser,
	struct text_buffer *out)
{
	char value;
	char kind;
	char next;
	char letter;
	char made;
	int made_byte;

	/* [ and the start of the set. */
	buffer_add(out, '[');
	parser->position++;
	value = peek(parser);
	if (value == '^' || value == '!') {
		buffer_add(out, value);
		parser->position++;
	}

	/* A ] first in the set is a character. */
	value = peek(parser);
	if (value == ']') {
		buffer_add(out, ']');
		parser->position++;
	}

	/* The characters up to the ] that closes the set. */
	for (;;) {
		/* The set must close before the line does. */
		value = peek(parser);
		if (value == '\0' || value == '\n')
			compile_error(parser, "unterminated address regex");

		/* An escape that makes a byte is the byte. */
		if (value == '\\') {
			letter = parser->text[parser->position + 1];
			parser->position += 2;
			made_byte = make_escape(parser, letter, &made);
			if (made_byte) {
				buffer_add(out, made);
				continue;
			}

			/* Any other escape is copied as it is. */
			parser->position -= 2;
		}

		/* Any other character, which the ] ends the set. */
		buffer_add(out, value);
		parser->position++;
		if (value == ']')
			return;

		/* [:class:] and the like are copied to their closing :]. */
		kind = peek(parser);
		if (value != '[' || (kind != ':' && kind != '=' && kind != '.'))
			continue;
		buffer_add(out, kind);
		parser->position++;
		for (;;) {
			value = peek(parser);
			if (value == '\0' || value == '\n')
				compile_error(parser, "unterminated address regex");
			buffer_add(out, value);
			parser->position++;
			next = peek(parser);
			if (value == kind && next == ']') {
				buffer_add(out, ']');
				parser->position++;
				break;
			}
		}
	}
}

/*
 * Compiles a regex (basic, or extended with -E), ignoring case or with ^
 * and $ at each newline (GNU's I and M) when asked.  An empty one is NULL,
 * which stands for the last regex used.
 */
static regex_t *
compile_regex(
	struct parser *parser,
	const char *text,
	int ignore_case,
	int multiline)
{
	char message[256];
	regex_t *regex;
	int flags;
	int error;

	/* The empty regex. */
	if (text[0] == '\0')
		return NULL;

	/* The flags. */
	flags = 0;
	if (parser->program->extended)
		flags |= REG_EXTENDED;
	if (ignore_case)
		flags |= REG_ICASE;
	if (multiline)
		flags |= REG_NEWLINE;

	/* The regex, compiled. */
	regex = sed_malloc(sizeof(*regex));
	error = regcomp(regex, text, flags);
	if (error != 0) {
		regerror(error, regex, message, sizeof(message));
		compile_error(parser, message);
	}

	/* Succeeded. */
	return regex;
}

/*
 * Reads the text of a, i or c: after a\ and a newline (POSIX), after a\ on
 * the same line, or after blanks (the one-line form).  A backslash takes
 * the next character literally; a backslash-newline continues the text.
 */
static char *
parse_text(
	struct parser *parser)
{
	struct text_buffer text;
	char value;
	char next;
	char made;
	int made_byte;
	char *finished;

	/* The form: a\ and a newline, a\text, or a text. */
	skip_blanks(parser);
	value = peek(parser);
	if (value == '\\') {
		parser->position++;
		value = peek(parser);
		if (value == '\n')
			parser->position++;
	}

	/* The text, line by line. */
	memset(&text, 0, sizeof(text));
	for (;;) {
		/* The end of the script or of the line ends the text. */
		value = peek(parser);
		if (value == '\0')
			break;
		if (value == '\n') {
			parser->position++;
			break;
		}

		/* A backslash: the next character, or a line that continues. */
		if (value == '\\') {
			next = parser->text[parser->position + 1];
			if (next == '\0') {
				parser->position++;
				break;
			}

			/* An escape that makes a byte (GNU), or the character. */
			parser->position += 2;
			made_byte = make_escape(parser, next, &made);
			if (made_byte)
				buffer_add(&text, made);
			else
				buffer_add(&text, next);
			continue;
		}

		/* An ordinary character. */
		buffer_add(&text, value);
		parser->position++;
	}

	/* Succeeded: the text, as a string. */
	finished = buffer_finish(&text);
	return finished;
}

/*
 * Reads a label, to a newline or a semicolon, less trailing blanks.
 * Returns NULL when there is none.
 */
static char *
parse_label(
	struct parser *parser)
{
	struct text_buffer label;
	char value;
	char *finished;

	/* Up to the end of the line or a semicolon. */
	skip_blanks(parser);
	memset(&label, 0, sizeof(label));
	for (;;) {
		value = peek(parser);
		if (value == '\0' || value == '\n' || value == ';')
			break;
		buffer_add(&label, value);
		parser->position++;
	}

	/* Trailing blanks are not part of it. */
	while (label.length > 0 &&
	       (label.data[label.length - 1U] == ' ' ||
		label.data[label.length - 1U] == '\t'))
		label.length--;
	if (label.length == 0) {
		free(label.data);
		return NULL;
	}

	/* Succeeded: the label, as a string. */
	finished = buffer_finish(&label);
	return finished;
}

/* Reads a file name of r or w, to the end of the line. */
static char *
parse_filename(
	struct parser *parser)
{
	struct text_buffer name;
	char value;
	char *finished;

	/* The rest of the line. */
	skip_blanks(parser);
	memset(&name, 0, sizeof(name));
	for (;;) {
		value = peek(parser);
		if (value == '\0')
			break;
		parser->position++;
		if (value == '\n')
			break;
		buffer_add(&name, value);
	}

	/* A command that writes or reads needs a name. */
	if (name.length == 0)
		compile_error(parser, "missing filename in r/R/w/W commands");

	/* Succeeded: the name, as a string. */
	finished = buffer_finish(&name);
	return finished;
}

/*
 * Reads the command of e, to the end of the line.  Returns NULL when there
 * is none (e alone runs the pattern space).
 */
static char *
parse_command_line(
	struct parser *parser)
{
	struct text_buffer line;
	char value;
	char *finished;

	/* The rest of the line. */
	skip_blanks(parser);
	memset(&line, 0, sizeof(line));
	for (;;) {
		value = peek(parser);
		if (value == '\0')
			break;
		parser->position++;
		if (value == '\n')
			break;
		buffer_add(&line, value);
	}

	/* No command. */
	if (line.length == 0) {
		free(line.data);
		return NULL;
	}

	/* Succeeded: the command, as a string. */
	finished = buffer_finish(&line);
	return finished;
}

/* Returns the reader of R for a file, shared by every R that names it. */
static struct sed_reader *
find_reader(
	struct parser *parser,
	const char *name)
{
	struct sed_reader *reader;
	int compare;

	/* A file already named. */
	for (reader = parser->program->readers; reader != NULL;
	     reader = reader->next) {
		compare = strcmp(reader->name, name);
		if (compare == 0)
			return reader;
	}

	/* Succeeded: a new one, opened when first read. */
	reader = sed_malloc(sizeof(*reader));
	memset(reader, 0, sizeof(*reader));
	reader->name = sed_strndup(name, strlen(name));
	reader->next = parser->program->readers;
	parser->program->readers = reader;
	return reader;
}

/* Parses s/regex/replacement/flags. */
static void
parse_substitute(
	struct parser *parser,
	struct sed_command *command)
{
	struct text_buffer regex;
	struct text_buffer replacement;
	char delimiter;
	int ignore_case;
	int multiline;

	/* The delimiter: any character but a backslash or a newline. */
	delimiter = peek(parser);
	if (delimiter == '\0' || delimiter == '\n' || delimiter == '\\')
		compile_error(parser, "unterminated `s' command");
	parser->position++;

	/* The regex and the replacement. */
	memset(&regex, 0, sizeof(regex));
	memset(&replacement, 0, sizeof(replacement));
	read_delimited(parser, delimiter, KIND_REGEX, &regex);
	read_delimited(parser, delimiter, KIND_REPLACEMENT, &replacement);
	command->substitute.replacement = buffer_finish(&replacement);
	buffer_finish(&regex);

	/* The flags, then the regex compiled with them. */
	command->substitute.occurrence = 1;
	parse_substitute_flags(parser, command, &ignore_case, &multiline);
	command->substitute.regex = compile_regex(parser, regex.data,
						  ignore_case, multiline);
	free(regex.data);

	/* Succeeded: the replacement names only groups the regex has. */
	check_references(parser, &command->substitute);
}

/* Parses the flags of s: g, p, a number, I, M, e, and w file (last). */
static void
parse_substitute_flags(
	struct parser *parser,
	struct sed_command *command,
	int *ignore_case,
	int *multiline)
{
	struct sed_substitute *substitute;
	char *name;
	char value;
	int digit;

	/* No flags yet. */
	substitute = &command->substitute;
	*ignore_case = 0;
	*multiline = 0;
	for (;;) {
		value = peek(parser);
		switch (value) {
		case 'g':
			substitute->global = 1;
			break;
		case 'p':
			substitute->print = 1;
			break;
		case 'i':
		case 'I':
			*ignore_case = 1;
			break;
		case 'm':
		case 'M':
			*multiline = 1;
			break;
		case 'e':
			check_sandbox(parser);
			substitute->evaluate = 1;
			break;
		case 'w':
			/* w file takes the rest of the line. */
			check_sandbox(parser);
			parser->position++;
			name = parse_filename(parser);
			substitute->output = open_output(parser, name);
			free(name);
			return;
		default:
			/* A number: the match to replace. */
			digit = is_digit(value);
			if (!digit)
				return;
			substitute->occurrence = parse_number(parser);
			if (substitute->occurrence == 0)
				compile_error(parser, "number option to `s' command may not be zero");
			continue;
		}

		/* Past a flag of one character. */
		parser->position++;
	}
}

/* Checks that the replacement names no group the regex lacks. */
static void
check_references(
	struct parser *parser,
	const struct sed_substitute *substitute)
{
	const char *cursor;
	size_t group;
	int digit;

	/* The last regex's groups are known only when it is used. */
	if (substitute->regex == NULL)
		return;

	/* Each \1 to \9. */
	for (cursor = substitute->replacement; *cursor != '\0'; cursor++) {
		if (*cursor != '\\' || cursor[1] == '\0')
			continue;
		cursor++;
		digit = is_digit(*cursor);
		if (!digit)
			continue;
		group = (size_t)(*cursor - '0');
		if (group > substitute->regex->re_nsub) {
			compile_error(parser, "invalid reference on `s' "
				      "command's RHS");
		}
	}
}

/* Parses y/source/target/: the map of each byte. */
static void
parse_translation(
	struct parser *parser,
	struct sed_command *command)
{
	struct text_buffer source;
	struct text_buffer target;
	size_t index;
	char delimiter;

	/* The delimiter and the two strings. */
	delimiter = peek(parser);
	if (delimiter == '\0' || delimiter == '\n' || delimiter == '\\')
		compile_error(parser, "unterminated `y' command");
	parser->position++;
	memset(&source, 0, sizeof(source));
	memset(&target, 0, sizeof(target));
	read_delimited(parser, delimiter, KIND_TRANSLATION, &source);
	read_delimited(parser, delimiter, KIND_TRANSLATION, &target);
	if (source.length != target.length) {
		compile_error(parser, "strings for `y' command are different "
			      "lengths");
	}

	/* Every byte maps to itself, except those of the source. */
	command->map = sed_malloc(256U);
	for (index = 0; index < 256U; index++)
		command->map[index] = (unsigned char)index;
	for (index = 0; index < source.length; index++)
		command->map[(unsigned char)source.data[index]] = (unsigned char)target.data[index];
	free(source.data);
	free(target.data);
}

/*
 * Returns the output of a w command or flag: opened (and emptied) when the
 * script is compiled, and shared by every command that names the file.
 */
static struct sed_output *
open_output(
	struct parser *parser,
	const char *name)
{
	struct sed_output *output;
	int compare;

	/* A file already named. */
	for (output = parser->program->outputs; output != NULL;
	     output = output->next) {
		compare = strcmp(output->name, name);
		if (compare == 0)
			return output;
	}

	/* A new one; /dev/stdout and /dev/stderr are the streams. */
	output = sed_malloc(sizeof(*output));
	memset(output, 0, sizeof(*output));
	output->name = sed_strndup(name, strlen(name));
	compare = strcmp(name, "/dev/stdout");
	if (compare == 0)
		output->stream = stdout;
	compare = strcmp(name, "/dev/stderr");
	if (compare == 0)
		output->stream = stderr;
	if (output->stream == NULL)
		output->stream = fopen(name, "w");
	if (output->stream == NULL)
		sed_fatal("couldn't open file", name);

	/* Succeeded. */
	output->next = parser->program->outputs;
	parser->program->outputs = output;
	return output;
}

/* Checks that only a separator, a } or a comment follows a command. */
static void
end_command(
	struct parser *parser)
{
	char value;

	/* Blanks, then a separator (taken) or a } or # (left). */
	skip_blanks(parser);
	value = peek(parser);
	if (value == ';' || value == '\n') {
		parser->position++;
		return;
	}

	/* A } or a comment ends the command too, and is left for later. */
	if (value == '}' || value == '#' || value == '\0')
		return;

	/* Anything else. */
	compile_error(parser, "extra characters after command");
}

/* Refuses a command that runs a program or touches a file, with --sandbox. */
static void
check_sandbox(
	const struct parser *parser)
{
	/* Anything goes outside the sandbox. */
	if (!parser->program->sandbox)
		return;

	/* The refusal ends sed. */
	compile_error(parser, "e/r/w commands disabled in sandbox mode");
}

/* Skips spaces and tabs. */
static void
skip_blanks(
	struct parser *parser)
{
	char value;

	/* Each blank. */
	for (;;) {
		value = peek(parser);
		if (value != ' ' && value != '\t')
			break;
		parser->position++;
	}
}

/* Returns the character at the position. */
static char
peek(
	const struct parser *parser)
{
	/* The character, or the terminating NUL. */
	return parser->text[parser->position];
}

/*
 * Resolves each b, t and T to the index of its label, or to the end of the
 * script when it has none.
 */
static void
resolve_labels(
	struct parser *parser)
{
	struct sed_program *program;
	struct sed_command *command;
	size_t index;
	size_t label;
	int compare;

	/* Each b, t and T, to the : of its label. */
	program = parser->program;
	for (index = 0; index < program->count; index++) {
		command = &program->commands[index];
		if (command->name != 'b' && command->name != 't' &&
		    command->name != 'T')
			continue;

		/* No label: the end of the script. */
		command->jump = program->count;
		if (command->text == NULL)
			continue;

		/* The : with the same label. */
		for (label = 0; label < program->count; label++) {
			if (program->commands[label].name != ':')
				continue;
			compare = strcmp(program->commands[label].text,
					 command->text);
			if (compare == 0)
				break;
		}

		/* A label that no : defines is an error. */
		if (label == program->count)
			sed_fatal("can't find label for jump to", command->text);
		command->jump = label;
	}
}

/* Reports an error in the script, with where it is, and ends sed. */
static void
compile_error(
	const struct parser *parser,
	const char *message)
{
	char where[64];

	/* The character the error was found at. */
	(void)snprintf(where, sizeof(where), "-e expression #1, char %zu",
		       parser->position);
	sed_fatal(where, message);
}

/* Adds a character to a string being built. */
static void
buffer_add(
	struct text_buffer *buffer,
	char value)
{
	/* Room for it and a terminating NUL. */
	if (buffer->length + 2U > buffer->capacity) {
		buffer->capacity = buffer->capacity * 2U + 32U;
		buffer->data = sed_realloc(buffer->data, buffer->capacity);
	}

	/* The character. */
	buffer->data[buffer->length] = value;
	buffer->length++;
}

/* Terminates a string being built and returns it. */
static char *
buffer_finish(
	struct text_buffer *buffer)
{
	/* An empty string has storage too. */
	if (buffer->data == NULL) {
		buffer->capacity = 1;
		buffer->data = sed_malloc(1);
	}

	/* Succeeded. */
	buffer->data[buffer->length] = '\0';
	return buffer->data;
}

/* Reports whether a character means something in a regex. */
static int
is_regex_special(
	char value)
{
	const char *found;

	/* The characters of BRE and ERE syntax. */
	if (value == '\0')
		return 0;
	found = strchr(".[]*^$\\+?(){}|", value);
	if (found == NULL)
		return 0;
	return 1;
}

/* Reports whether a character is a decimal digit. */
static int
is_digit(
	char value)
{
	/* 0 to 9. */
	if (value >= '0' && value <= '9')
		return 1;
	return 0;
}
