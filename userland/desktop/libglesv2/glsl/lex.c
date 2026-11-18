/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's lexer: a source becomes preprocessing tokens --
 * identifiers, numbers, punctuators and the ends of lines, which the
 * preprocessor needs to find its directives.
 *
 * Comments become white space, a backslash at the end of a line joins
 * the lines, and keywords are not told apart here: the parser classifies
 * identifiers by the version #version chose.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* The spellings of the punctuators, longest first so that the first match is the token. */
struct lex_punct {
	const char *text;
	unsigned punct;
};

/*
 * The punctuators by spelling.  Three-character ones come first, then
 * two, then one, so a scan in order finds the longest.
 */
static const struct lex_punct lex_puncts[] = {
	{ "<<=", GLSL_P_SHL_ASSIGN },
	{ ">>=", GLSL_P_SHR_ASSIGN },
	{ "<<", GLSL_P_SHL },
	{ ">>", GLSL_P_SHR },
	{ "<=", GLSL_P_LE },
	{ ">=", GLSL_P_GE },
	{ "==", GLSL_P_EQ },
	{ "!=", GLSL_P_NE },
	{ "&&", GLSL_P_AND_AND },
	{ "||", GLSL_P_OR_OR },
	{ "^^", GLSL_P_XOR_XOR },
	{ "+=", GLSL_P_ADD_ASSIGN },
	{ "-=", GLSL_P_SUB_ASSIGN },
	{ "*=", GLSL_P_MUL_ASSIGN },
	{ "/=", GLSL_P_DIV_ASSIGN },
	{ "%=", GLSL_P_MOD_ASSIGN },
	{ "&=", GLSL_P_AND_ASSIGN },
	{ "|=", GLSL_P_OR_ASSIGN },
	{ "^=", GLSL_P_XOR_ASSIGN },
	{ "++", GLSL_P_INC },
	{ "--", GLSL_P_DEC },
	{ "##", GLSL_P_HASH_HASH },
	{ "(", GLSL_P_LPAREN },
	{ ")", GLSL_P_RPAREN },
	{ "[", GLSL_P_LBRACKET },
	{ "]", GLSL_P_RBRACKET },
	{ "{", GLSL_P_LBRACE },
	{ "}", GLSL_P_RBRACE },
	{ ".", GLSL_P_DOT },
	{ ",", GLSL_P_COMMA },
	{ ";", GLSL_P_SEMICOLON },
	{ ":", GLSL_P_COLON },
	{ "?", GLSL_P_QUESTION },
	{ "+", GLSL_P_PLUS },
	{ "-", GLSL_P_MINUS },
	{ "*", GLSL_P_STAR },
	{ "/", GLSL_P_SLASH },
	{ "%", GLSL_P_PERCENT },
	{ "<", GLSL_P_LT },
	{ ">", GLSL_P_GT },
	{ "!", GLSL_P_BANG },
	{ "~", GLSL_P_TILDE },
	{ "&", GLSL_P_AMP },
	{ "|", GLSL_P_BAR },
	{ "^", GLSL_P_CARET },
	{ "=", GLSL_P_ASSIGN },
	{ "#", GLSL_P_HASH }
};

/*
 * The lexer's position in a source.
 */
struct lex_state {
	struct glsl_shader *shader;
	const char *source;
	size_t length;
	size_t at;
	unsigned line;

	/* The tokens made so far, and the room for them. */
	struct glsl_token *tokens;
	unsigned count;
	unsigned capacity;
};

static struct glsl_token *lex_push(struct lex_state *state);
static int lex_skip_space(struct lex_state *state);
static int lex_is_identifier_start(int character);
static int lex_is_identifier_part(int character);
static int lex_is_digit(int character);
static void lex_scan_identifier(struct lex_state *state);
static void lex_number(struct lex_state *state, struct glsl_token *token);
static void lex_integer(struct lex_state *state, struct glsl_token *token, size_t start, size_t end);
static void lex_punct(struct lex_state *state, struct glsl_token *token);

/*
 * Splits a source into tokens, the last one GLSL_TOKEN_EOF.  Returns how
 * many there are besides the EOF, and the array in *tokens (arena).
 */
unsigned
glsl_lex(
	struct glsl_shader *shader,
	const char *source,
	size_t length,
	unsigned first_line,
	struct glsl_token **tokens)
{
	struct lex_state state;
	struct glsl_token *token;
	int space;
	int character;
	int starts;

	/* Starts at the first line with room for a few tokens. */
	memset(&state, 0, sizeof(state));
	state.shader = shader;
	state.source = source;
	state.length = length;
	state.line = first_line;

	/* Reads tokens up to the end of the source. */
	for (;;) {
		/* Skips white space and comments, noting whether any came before the token. */
		space = lex_skip_space(&state);
		if (state.at >= state.length)
			break;

		/* The token starts here, on this line. */
		token = lex_push(&state);
		token->text = state.source + state.at;
		token->line = state.line;
		token->space = (unsigned)space;
		character = (unsigned char)state.source[state.at];

		/* The end of a line (the preprocessor's directives end there). */
		if (character == '\n') {
			token->kind = GLSL_TOKEN_NEWLINE;
			token->length = 1U;
			state.at++;
			state.line++;
			continue;
		}

		/* An identifier or a keyword. */
		starts = lex_is_identifier_start(character);
		if (starts) {
			lex_scan_identifier(&state);
			token->kind = GLSL_TOKEN_IDENTIFIER;
			token->length = (unsigned)(state.source + state.at - token->text);
			continue;
		}

		/* A number: a digit, or a dot followed by a digit. */
		starts = lex_is_digit(character);
		if (!starts && character == '.' && state.at + 1U < state.length)
			starts = lex_is_digit((unsigned char)state.source[state.at + 1U]);
		if (starts) {
			lex_number(&state, token);
			continue;
		}

		/* Anything else: a punctuator, or a character GLSL does not have. */
		lex_punct(&state, token);
	}

	/* Ends the tokens with an EOF token on the last line. */
	token = lex_push(&state);
	token->kind = GLSL_TOKEN_EOF;
	token->text = state.source + state.length;
	token->line = state.line;

	/* Succeeded: the tokens and how many there are before the EOF. */
	*tokens = state.tokens;
	return state.count - 1U;
}

/*
 * Reports whether a token is spelled exactly as a text.
 */
int
glsl_token_is(
	const struct glsl_token *token,
	const char *text)
{
	size_t length;
	int differs;

	/* The lengths first, then the bytes. */
	length = strlen(text);
	if (token->length != length)
		return 0;
	differs = memcmp(token->text, text, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the same spelling. */
	return 1;
}

/* Adds a zeroed token to the end of the list, growing the list when it is full. */
static struct glsl_token *
lex_push(
	struct lex_state *state)
{
	struct glsl_token *grown;
	unsigned capacity;

	/* Grows the array (in the arena; the old one stays there unused). */
	if (state->count == state->capacity) {
		capacity = state->capacity * 2U;
		if (capacity < 256U)
			capacity = 256U;
		grown = glsl_alloc(&state->shader->arena, capacity * sizeof(*grown));
		if (state->count != 0U)
			memcpy(grown, state->tokens, state->count * sizeof(*grown));
		state->tokens = grown;
		state->capacity = capacity;
	}

	/* Succeeded: the new token (the arena zeroed it). */
	state->count++;
	return &state->tokens[state->count - 1U];
}

/* Skips spaces, comments and joined lines up to a token or a line's end; nonzero when any were skipped. */
static int
lex_skip_space(
	struct lex_state *state)
{
	const char *source;
	int skipped;
	int character;
	int next;

	/* Walks over everything that is not a token. */
	source = state->source;
	skipped = 0;
	while (state->at < state->length) {
		character = (unsigned char)source[state->at];
		next = 0;
		if (state->at + 1U < state->length)
			next = (unsigned char)source[state->at + 1U];

		/* A backslash before the end of a line joins the two lines. */
		if (character == '\\' && next == '\n') {
			state->at += 2U;
			state->line++;
			skipped = 1;
			continue;
		}

		/* A backslash before a carriage return and a line's end does too. */
		if (character == '\\' && next == '\r') {
			state->at += 2U;
			if (state->at < state->length && source[state->at] == '\n')
				state->at++;
			state->line++;
			skipped = 1;
			continue;
		}

		/* Spaces, tabs, form feeds, vertical tabs and carriage returns. */
		if (character == ' ' || character == '\t' || character == '\f' || character == '\v' || character == '\r') {
			state->at++;
			skipped = 1;
			continue;
		}

		/* A comment to the end of the line (the line's end stays a token). */
		if (character == '/' && next == '/') {
			while (state->at < state->length && source[state->at] != '\n')
				state->at++;
			skipped = 1;
			continue;
		}

		/* A block comment, whose lines are counted but whose ends of lines are not tokens. */
		if (character == '/' && next == '*') {
			state->at += 2U;
			while (state->at < state->length) {
				/* The end of the comment. */
				if (source[state->at] == '*' && state->at + 1U < state->length && source[state->at + 1U] == '/') {
					state->at += 2U;
					break;
				}

				/* A line inside it. */
				if (source[state->at] == '\n')
					state->line++;
				state->at++;
			}

			/* The comment counts as white space. */
			skipped = 1;
			continue;
		}

		/* Anything else starts a token or ends the line. */
		break;
	}

	/* Succeeded: whether anything was skipped. */
	return skipped;
}

/* Reports whether a character can start an identifier. */
static int
lex_is_identifier_start(
	int character)
{
	/* Letters and the underscore. */
	if (character >= 'a' && character <= 'z')
		return 1;
	if (character >= 'A' && character <= 'Z')
		return 1;
	if (character == '_')
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports whether a character can continue an identifier. */
static int
lex_is_identifier_part(
	int character)
{
	int start;

	/* A character that could start one, or a digit. */
	start = lex_is_identifier_start(character);
	if (start)
		return 1;
	if (character >= '0' && character <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/* Moves over the letters, digits and underscores of an identifier. */
static void
lex_scan_identifier(
	struct lex_state *state)
{
	int part;

	/* Walks to the first character that cannot continue an identifier. */
	for (;;) {
		if (state->at >= state->length)
			break;
		part = lex_is_identifier_part((unsigned char)state->source[state->at]);
		if (!part)
			break;
		state->at++;
	}
}

/* Reports whether a character is a decimal digit. */
static int
lex_is_digit(
	int character)
{
	/* The ten digits. */
	if (character >= '0' && character <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/*
 * Reads a number: an integer (decimal, octal or hexadecimal, with an
 * optional u suffix) or a float (with a fraction or an exponent, and an
 * optional f suffix, or lf for a double).
 */
static void
lex_number(
	struct lex_state *state,
	struct glsl_token *token)
{
	const char *source;
	char buffer[128];
	size_t start;
	size_t digits_end;
	size_t length;
	int is_float;
	int character;
	int identifier;

	/* Scans the digits, a fraction and an exponent. */
	source = state->source;
	start = state->at;
	is_float = 0;

	/* A hexadecimal integer has only hexadecimal digits after its prefix. */
	if (source[start] == '0' && start + 1U < state->length && (source[start + 1U] == 'x' || source[start + 1U] == 'X')) {
		state->at = start + 2U;
		while (state->at < state->length) {
			character = (unsigned char)source[state->at];
			if (!((character >= '0' && character <= '9') ||
			      (character >= 'a' && character <= 'f') ||
			      (character >= 'A' && character <= 'F')))
				break;
			state->at++;
		}
	} else {
		/* Decimal digits, a fraction after a dot, and an exponent. */
		while (state->at < state->length && source[state->at] >= '0' && source[state->at] <= '9')
			state->at++;

		/* The fraction. */
		if (state->at < state->length && source[state->at] == '.') {
			is_float = 1;
			state->at++;
			while (state->at < state->length && source[state->at] >= '0' && source[state->at] <= '9')
				state->at++;
		}

		/* The exponent, with an optional sign. */
		if (state->at < state->length && (source[state->at] == 'e' || source[state->at] == 'E')) {
			is_float = 1;
			state->at++;
			if (state->at < state->length && (source[state->at] == '+' || source[state->at] == '-'))
				state->at++;
			if (state->at >= state->length || source[state->at] < '0' || source[state->at] > '9')
				glsl_error(state->shader, state->line, "a float's exponent has no digits");
			while (state->at < state->length && source[state->at] >= '0' && source[state->at] <= '9')
				state->at++;
		}
	}

	/* The digits end here; a suffix may follow. */
	digits_end = state->at;
	token->suffix = 0U;
	if (state->at < state->length) {
		character = (unsigned char)source[state->at];
		if (is_float && (character == 'f' || character == 'F')) {
			/* A float's f. */
			token->suffix = 1U;
			state->at++;
		} else if (is_float && state->at + 1U < state->length &&
			   ((character == 'l' && source[state->at + 1U] == 'f') ||
			    (character == 'L' && source[state->at + 1U] == 'F'))) {
			/* A double's lf (or LF). */
			token->suffix = GLSL_SUFFIX_DOUBLE;
			state->at += 2U;
		} else if (!is_float && (character == 'u' || character == 'U')) {
			/* An integer's u. */
			token->suffix = 1U;
			state->at++;
		}
	}

	/* A letter or digit right after the number makes it malformed. */
	identifier = 0;
	if (state->at < state->length)
		identifier = lex_is_identifier_part((unsigned char)source[state->at]);
	if (identifier) {
		lex_scan_identifier(state);
		glsl_error(state->shader, state->line, "malformed number '%.*s'", (int)(state->at - start), source + start);
	}

	/* The token's spelling. */
	token->length = (unsigned)(state->at - start);

	/* An integer's value. */
	if (!is_float) {
		lex_integer(state, token, start, digits_end);
		return;
	}

	/* A float's value, read by the C library from a terminated copy. */
	length = digits_end - start;
	if (length >= sizeof(buffer))
		length = sizeof(buffer) - 1U;
	memcpy(buffer, source + start, length);
	buffer[length] = '\0';
	token->kind = GLSL_TOKEN_FLOAT;
	token->number = strtof(buffer, NULL);
	token->number_double = strtod(buffer, NULL);
}

/* Converts an integer's digits (decimal, octal or hexadecimal) to its 32-bit value. */
static void
lex_integer(
	struct lex_state *state,
	struct glsl_token *token,
	size_t start,
	size_t end)
{
	const char *source;
	uint64_t value;
	unsigned base;
	unsigned digit;
	size_t at;
	int character;

	/* The base from the prefix. */
	source = state->source;
	base = 10U;
	at = start;
	if (end - start > 1U && source[start] == '0' && (source[start + 1U] == 'x' || source[start + 1U] == 'X')) {
		base = 16U;
		at = start + 2U;
		if (at == end)
			glsl_error(state->shader, state->line, "a hexadecimal number has no digits");
	} else if (end - start > 1U && source[start] == '0') {
		base = 8U;
		at = start + 1U;
	}

	/* Accumulates the digits, noting a value too large for 32 bits. */
	value = 0U;
	for (; at < end; at++) {
		character = (unsigned char)source[at];
		if (character >= '0' && character <= '9') {
			digit = (unsigned)(character - '0');
		} else if (character >= 'a' && character <= 'f') {
			digit = (unsigned)(character - 'a' + 10);
		} else {
			digit = (unsigned)(character - 'A' + 10);
		}

		/* An octal number takes no 8 or 9. */
		if (digit >= base) {
			glsl_error(state->shader, state->line, "digit '%c' is not octal", character);
			digit = 0U;
		}

		/* The value so far, held below 2^33 so it cannot wrap. */
		value = value * base + digit;
		if (value > 0x1ffffffffULL)
			value = 0x1ffffffffULL;
	}

	/* An integer larger than 32 bits is an error. */
	if (value > 0xffffffffULL)
		glsl_error(state->shader, state->line, "integer constant '%.*s' does not fit in 32 bits", (int)(end - start), source + start);

	/* The token: an unsigned one with its u, else a signed one. */
	token->integer = (uint32_t)value;
	token->kind = GLSL_TOKEN_INT;
	if (token->suffix != 0U)
		token->kind = GLSL_TOKEN_UINT;
}

/* Reads a punctuator, or one character GLSL does not have as a token. */
static void
lex_punct(
	struct lex_state *state,
	struct glsl_token *token)
{
	size_t index;
	size_t length;
	int differs;

	/* The longest punctuator spelled at this point. */
	for (index = 0U; index < sizeof(lex_puncts) / sizeof(lex_puncts[0]); index++) {
		length = strlen(lex_puncts[index].text);
		if (state->at + length > state->length)
			continue;
		differs = memcmp(state->source + state->at, lex_puncts[index].text, length);
		if (differs != 0)
			continue;

		/* The punctuator. */
		token->kind = GLSL_TOKEN_PUNCT;
		token->punct = lex_puncts[index].punct;
		token->length = (unsigned)length;
		state->at += length;
		return;
	}

	/* A character that is not a token of GLSL: the parser reports it where it matters. */
	token->kind = GLSL_TOKEN_OTHER;
	token->length = 1U;
	state->at++;
}
