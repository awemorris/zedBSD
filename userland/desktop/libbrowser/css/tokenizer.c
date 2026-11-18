/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CSS tokenizer of CSS Syntax 3: turns a style sheet's text into a
 * list of tokens.
 *
 * The whole text is tokenized at once into an array; the token texts
 * (unescaped names and strings) live in the caller's arena.
 */

#include "css/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The end of the input, as a code point. */
#define LEX_EOF		(-1)

/* The longest number the tokenizer converts, in characters. */
#define LEX_NUMBER_MAX	64U

/*
 * The state of one tokenization: the text, where the reading stands, and
 * the tokens made so far.
 */
struct css_lexer {
	const uint16_t *units;
	size_t length;
	size_t position;
	/* The current token begins after skipped comments and before any token consumption. */
	size_t token_start;
	struct wb_arena *arena;
	struct wb_vector tokens;
	struct wb_units scratch;
	int failed;
};

static int32_t lex_peek(const struct css_lexer *lexer, size_t ahead);
static int32_t lex_next(struct css_lexer *lexer);
static int lex_is_name_start(int32_t c);
static int lex_is_name(int32_t c);
static int lex_is_digit(int32_t c);
static int lex_is_hex(int32_t c);
static int lex_is_whitespace(int32_t c);
static int lex_valid_escape(int32_t first, int32_t second);
static int lex_escape_ahead(const struct css_lexer *lexer, size_t ahead);
static int lex_starts_identifier(const struct css_lexer *lexer, size_t ahead);
static int lex_starts_number(const struct css_lexer *lexer, size_t ahead);
static void lex_token(struct css_lexer *lexer);
static int lex_punctuation(struct css_lexer *lexer, int32_t c);
static void lex_hash(struct css_lexer *lexer);
static void lex_hyphen(struct css_lexer *lexer);
static void lex_whitespace(struct css_lexer *lexer);
static void lex_emit(struct css_lexer *lexer, int type, const struct wb_units *text, double number, int integer, uint32_t delim);
static void lex_comments(struct css_lexer *lexer);
static void lex_string(struct css_lexer *lexer, int32_t quote);
static void lex_numeric(struct css_lexer *lexer);
static size_t lex_digits(struct css_lexer *lexer, char *text, size_t used);
static void lex_ident_like(struct css_lexer *lexer);
static void lex_url(struct css_lexer *lexer);
static void lex_bad_url(struct css_lexer *lexer);
static void lex_name(struct css_lexer *lexer, struct wb_units *out);
static uint32_t lex_escape(struct css_lexer *lexer);
static void lex_append(struct css_lexer *lexer, struct wb_units *units, uint32_t code_point);

/*
 * Tokenizes a style sheet's text.
 *
 * On success tokens points at an array (in the arena) of count tokens,
 * the last of which is CSS_TOKEN_EOF.
 */
int
css_tokenize(
	struct wb_arena *arena,
	const uint16_t *units,
	size_t length,
	struct css_token **tokens,
	size_t *count)
{
	struct css_lexer lexer;
	struct css_token *copy;
	struct css_token *last;
	size_t size;

	/* Starts at the beginning with no tokens. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.units = units;
	lexer.length = length;
	lexer.arena = arena;
	wb_vector_init(&lexer.tokens, sizeof(struct css_token));
	wb_units_init(&lexer.scratch);

	/* Makes tokens until the end-of-file token. */
	for (;;) {
		lex_token(&lexer);
		if (lexer.failed)
			break;

		/* The end-of-file token is the last. */
		last = wb_vector_at(&lexer.tokens, lexer.tokens.count - 1U);
		if (last->type == CSS_TOKEN_EOF)
			break;
	}

	/* Moves the tokens into the arena. */
	size = lexer.tokens.count * sizeof(struct css_token);
	copy = NULL;
	if (!lexer.failed)
		copy = wb_arena_alloc(arena, size);
	if (copy != NULL)
		memcpy(copy, lexer.tokens.items, size);
	wb_vector_release(&lexer.tokens);
	wb_units_release(&lexer.scratch);
	if (copy == NULL)
		return ENOMEM;

	/* Succeeded: the tokens are in the arena. */
	*tokens = copy;
	*count = size / sizeof(struct css_token);
	return 0;
}

/* Reads the code point ahead of the position without consuming it (CR and FF read as LF, NUL as U+FFFD). */
static int32_t
lex_peek(
	const struct css_lexer *lexer,
	size_t ahead)
{
	uint16_t unit;

	/* Past the end is end of file. */
	if (ahead >= lexer->length - lexer->position)
		return LEX_EOF;

	/* The preprocessing: carriage returns and form feeds are line feeds, NULs are replaced. */
	unit = lexer->units[lexer->position + ahead];
	if (unit == 0x0dU || unit == 0x0cU)
		return 0x0a;
	if (unit == 0)
		return (int32_t)WB_REPLACEMENT;

	/* Surrogates are read as they are; CSS names treat them as name characters. */
	return unit;
}

/* Consumes one code point (a CR LF pair counts as one line feed). */
static int32_t
lex_next(
	struct css_lexer *lexer)
{
	int32_t c;

	/* Reads it. */
	c = lex_peek(lexer, 0);
	if (c == LEX_EOF)
		return LEX_EOF;

	/* Moves past it, and past the LF of a CR LF pair. */
	if (lexer->units[lexer->position] == 0x0dU &&
	    lexer->position + 1U < lexer->length &&
	    lexer->units[lexer->position + 1U] == 0x0aU)
		lexer->position++;
	lexer->position++;

	/* Reports the code point. */
	return c;
}

/* Tells whether a code point starts a name: a letter, underscore or non-ASCII. */
static int
lex_is_name_start(
	int32_t c)
{
	/* Letters and the underscore. */
	if (c >= 'a' && c <= 'z')
		return 1;
	if (c >= 'A' && c <= 'Z')
		return 1;
	if (c == '_')
		return 1;

	/* Everything past ASCII. */
	if (c >= 0x80)
		return 1;

	/* Anything else does not. */
	return 0;
}

/* Tells whether a code point continues a name: a name start, a digit or '-'. */
static int
lex_is_name(
	int32_t c)
{
	int start;

	/* The name starts. */
	start = lex_is_name_start(c);
	if (start)
		return 1;

	/* Digits and the hyphen. */
	if (c >= '0' && c <= '9')
		return 1;
	if (c == '-')
		return 1;

	/* Anything else ends a name. */
	return 0;
}

/* Tells whether a code point is an ASCII digit. */
static int
lex_is_digit(
	int32_t c)
{
	/* The digit range. */
	if (c >= '0' && c <= '9')
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether a code point is a hexadecimal digit. */
static int
lex_is_hex(
	int32_t c)
{
	/* The digits and a to f in either case. */
	if (c >= '0' && c <= '9')
		return 1;
	if (c >= 'a' && c <= 'f')
		return 1;
	if (c >= 'A' && c <= 'F')
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether a code point is CSS whitespace (after preprocessing): LF, tab or space. */
static int
lex_is_whitespace(
	int32_t c)
{
	/* The three whitespace characters left after preprocessing. */
	if (c == 0x0a || c == 0x09 || c == 0x20)
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether two code points start a valid escape (a backslash not followed by a newline). */
static int
lex_valid_escape(
	int32_t first,
	int32_t second)
{
	/* A backslash followed by anything but a line feed or the end. */
	if (first == '\\' && second != 0x0a && second != LEX_EOF)
		return 1;

	/* Anything else is not an escape. */
	return 0;
}

/* Tells whether the code points at an offset start a valid escape. */
static int
lex_escape_ahead(
	const struct css_lexer *lexer,
	size_t ahead)
{
	int32_t first;
	int32_t second;
	int escape;

	/* Reads the two code points and asks. */
	first = lex_peek(lexer, ahead);
	second = lex_peek(lexer, ahead + 1U);
	escape = lex_valid_escape(first, second);

	/* Reports the answer. */
	return escape;
}

/* Tells whether the three code points ahead start an identifier. */
static int
lex_starts_identifier(
	const struct css_lexer *lexer,
	size_t ahead)
{
	int32_t first;
	int32_t second;
	int start;
	int escape;

	/* A hyphen needs a name start, another hyphen or an escape after it. */
	first = lex_peek(lexer, ahead);
	second = lex_peek(lexer, ahead + 1U);
	if (first == '-') {
		start = lex_is_name_start(second);
		if (start || second == '-')
			return 1;
		escape = lex_escape_ahead(lexer, ahead + 1U);
		return escape;
	}

	/* A name start, or an escape. */
	start = lex_is_name_start(first);
	if (start)
		return 1;
	escape = lex_valid_escape(first, second);

	/* Reports whether an escape starts it. */
	return escape;
}

/* Tells whether the code points ahead start a number. */
static int
lex_starts_number(
	const struct css_lexer *lexer,
	size_t ahead)
{
	int32_t first;
	int32_t second;
	int32_t third;
	int digit;

	/* A sign needs a digit, or a point and a digit, after it. */
	first = lex_peek(lexer, ahead);
	second = lex_peek(lexer, ahead + 1U);
	third = lex_peek(lexer, ahead + 2U);
	if (first == '+' || first == '-') {
		digit = lex_is_digit(second);
		if (digit)
			return 1;
		if (second != '.')
			return 0;
		digit = lex_is_digit(third);
		return digit;
	}

	/* A decimal point needs a digit after it. */
	if (first == '.') {
		digit = lex_is_digit(second);
		return digit;
	}

	/* Otherwise a digit starts it. */
	digit = lex_is_digit(first);
	return digit;
}

/* Consumes one token. */
static void
lex_token(
	struct css_lexer *lexer)
{
	int32_t c;
	int handled;
	int digit;
	int start;

	/* Comments go without a token. */
	lex_comments(lexer);
	lexer->token_start = lexer->position;

	/* The end of the input. */
	c = lex_peek(lexer, 0);
	if (c == LEX_EOF) {
		lex_emit(lexer, CSS_TOKEN_EOF, NULL, 0, 0, 0);
		return;
	}

	/* Whitespace runs. */
	handled = lex_is_whitespace(c);
	if (handled) {
		lex_whitespace(lexer);
		return;
	}

	/* The punctuation and the characters with rules of their own. */
	handled = lex_punctuation(lexer, c);
	if (handled)
		return;

	/* Digits start numbers; name starts start identifiers. */
	digit = lex_is_digit(c);
	if (digit) {
		lex_numeric(lexer);
		return;
	}

	/* Name starts start identifiers. */
	start = lex_is_name_start(c);
	if (start) {
		lex_ident_like(lexer);
		return;
	}

	/* Anything else is a delimiter. */
	lex_next(lexer);
	lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, (uint32_t)c);
}

/* Consumes a token that starts with punctuation; returns 0 when c is not such a character. */
static int
lex_punctuation(
	struct css_lexer *lexer,
	int32_t c)
{
	int32_t first;
	int32_t second;
	int32_t third;
	int starts;
	int escape;

	/* The character's rule. */
	switch (c) {
	case '"':
	case '\'':
		lex_next(lexer);
		lex_string(lexer, c);
		return 1;
	case '#':
		lex_hash(lexer);
		return 1;
	case '(':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_OPEN_PAREN, NULL, 0, 0, 0);
		return 1;
	case ')':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_CLOSE_PAREN, NULL, 0, 0, 0);
		return 1;
	case '[':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_OPEN_SQUARE, NULL, 0, 0, 0);
		return 1;
	case ']':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_CLOSE_SQUARE, NULL, 0, 0, 0);
		return 1;
	case '{':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_OPEN_CURLY, NULL, 0, 0, 0);
		return 1;
	case '}':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_CLOSE_CURLY, NULL, 0, 0, 0);
		return 1;
	case ',':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_COMMA, NULL, 0, 0, 0);
		return 1;
	case ':':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_COLON, NULL, 0, 0, 0);
		return 1;
	case ';':
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_SEMICOLON, NULL, 0, 0, 0);
		return 1;
	case '+':
	case '.':
		/* A sign or a point that starts a number, else a delimiter. */
		starts = lex_starts_number(lexer, 0);
		if (starts) {
			lex_numeric(lexer);
			return 1;
		}

		/* Otherwise the sign or point is a delimiter. */
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, (uint32_t)c);
		return 1;
	case '-':
		lex_hyphen(lexer);
		return 1;
	case '<':
		/* A CDO, or a delimiter. */
		lex_next(lexer);
		first = lex_peek(lexer, 0);
		second = lex_peek(lexer, 1);
		third = lex_peek(lexer, 2);
		if (first == '!' && second == '-' && third == '-') {
			lexer->position += 3U;
			lex_emit(lexer, CSS_TOKEN_CDO, NULL, 0, 0, 0);
			return 1;
		}

		/* A lone less-than sign is a delimiter. */
		lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, '<');
		return 1;
	case '@':
		/* An at-keyword, or a delimiter. */
		lex_next(lexer);
		starts = lex_starts_identifier(lexer, 0);
		if (starts) {
			lex_name(lexer, &lexer->scratch);
			lex_emit(lexer, CSS_TOKEN_AT_KEYWORD, &lexer->scratch, 0, 0, 0);
			return 1;
		}

		/* A lone at sign is a delimiter. */
		lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, '@');
		return 1;
	case '\\':
		/* An escape starts an identifier; a lone backslash is a delimiter. */
		escape = lex_escape_ahead(lexer, 0);
		if (escape) {
			lex_ident_like(lexer);
			return 1;
		}

		/* A lone backslash is a delimiter. */
		lex_next(lexer);
		lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, '\\');
		return 1;
	default:
		return 0;
	}
}

/* Consumes a hash token (a # and a name), or a # delimiter. */
static void
lex_hash(
	struct css_lexer *lexer)
{
	struct css_token *token;
	int name;
	int escape;
	int starts;

	/* Moves past the #; a name or an escape must follow for a hash. */
	lex_next(lexer);
	name = lex_is_name(lex_peek(lexer, 0));
	escape = lex_escape_ahead(lexer, 0);
	if (!name && !escape) {
		lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, '#');
		return;
	}

	/* The hash is flagged "id" when its name is an identifier. */
	starts = lex_starts_identifier(lexer, 0);
	lex_name(lexer, &lexer->scratch);
	lex_emit(lexer, CSS_TOKEN_HASH, &lexer->scratch, 0, 0, 0);
	if (lexer->failed)
		return;
	token = wb_vector_at(&lexer->tokens, lexer->tokens.count - 1U);
	token->hash_is_id = starts;
}

/* Consumes what a hyphen starts: a number, a CDC, an identifier or a delimiter. */
static void
lex_hyphen(
	struct css_lexer *lexer)
{
	int32_t second;
	int32_t third;
	int starts;

	/* A negative number. */
	starts = lex_starts_number(lexer, 0);
	if (starts) {
		lex_numeric(lexer);
		return;
	}

	/* A CDC. */
	second = lex_peek(lexer, 1);
	third = lex_peek(lexer, 2);
	if (second == '-' && third == '>') {
		lexer->position += 3U;
		lex_emit(lexer, CSS_TOKEN_CDC, NULL, 0, 0, 0);
		return;
	}

	/* An identifier. */
	starts = lex_starts_identifier(lexer, 0);
	if (starts) {
		lex_ident_like(lexer);
		return;
	}

	/* Otherwise a delimiter. */
	lex_next(lexer);
	lex_emit(lexer, CSS_TOKEN_DELIM, NULL, 0, 0, '-');
}

/* Consumes a run of whitespace as one token. */
static void
lex_whitespace(
	struct css_lexer *lexer)
{
	int space;

	/* Takes every whitespace character. */
	for (;;) {
		space = lex_is_whitespace(lex_peek(lexer, 0));
		if (!space)
			break;
		lex_next(lexer);
	}

	/* One token stands for the run. */
	lex_emit(lexer, CSS_TOKEN_WHITESPACE, NULL, 0, 0, 0);
}

/* Appends a token with a copy of its text in the arena. */
static void
lex_emit(
	struct css_lexer *lexer,
	int type,
	const struct wb_units *text,
	double number,
	int integer,
	uint32_t delim)
{
	struct css_token token;
	uint16_t *copy;
	int error;

	/* Fills the token. */
	memset(&token, 0, sizeof(token));
	token.type = type;
	token.source_start = lexer->token_start;
	token.source_end = lexer->position;
	token.number = number;
	token.integer = integer;
	token.delim = delim;

	/* Copies its text into the arena. */
	if (text != NULL && text->length != 0) {
		copy = wb_arena_alloc(lexer->arena, text->length * sizeof(uint16_t));
		if (copy == NULL) {
			lexer->failed = 1;
			return;
		}

		/* Copies the text. */
		memcpy(copy, text->data, text->length * sizeof(uint16_t));
		token.text = copy;
		token.length = text->length;
	}

	/* Appends it. */
	error = wb_vector_push(&lexer->tokens, &token);
	if (error != 0)
		lexer->failed = 1;
}

/* Consumes the comments at the position. */
static void
lex_comments(
	struct css_lexer *lexer)
{
	int32_t c;
	int32_t next;

	/* Each comment runs from slash-star to star-slash or the end. */
	for (;;) {
		c = lex_peek(lexer, 0);
		next = lex_peek(lexer, 1);
		if (c != '/' || next != '*')
			return;

		/* Inside the comment up to its end. */
		lexer->position += 2U;
		for (;;) {
			/* The end of the input ends the comment. */
			c = lex_peek(lexer, 0);
			if (c == LEX_EOF)
				return;

			/* Star-slash ends it. */
			next = lex_peek(lexer, 1);
			if (c == '*' && next == '/') {
				lexer->position += 2U;
				break;
			}

			/* Anything else is inside it. */
			lex_next(lexer);
		}
	}
}

/* Consumes a string token after its opening quote, up to the closing one. */
static void
lex_string(
	struct css_lexer *lexer,
	int32_t quote)
{
	size_t before;
	int32_t c;

	/* Gathers the characters. */
	wb_units_clear(&lexer->scratch);
	for (;;) {
		before = lexer->position;
		c = lex_next(lexer);

		/* The closing quote or the end ends the string. */
		if (c == quote || c == LEX_EOF)
			break;

		/* A newline makes it a bad string (the newline is not consumed). */
		if (c == 0x0a) {
			lexer->position = before;
			lex_emit(lexer, CSS_TOKEN_BAD_STRING, NULL, 0, 0, 0);
			return;
		}

		/* An escape: a backslash newline continues the line; others stand for their code point. */
		if (c == '\\') {
			c = lex_peek(lexer, 0);
			if (c == 0x0a)
				lex_next(lexer);
			if (c == 0x0a || c == LEX_EOF)
				continue;
			lex_append(lexer, &lexer->scratch, lex_escape(lexer));
			continue;
		}

		/* Anything else is the string's. */
		lex_append(lexer, &lexer->scratch, (uint32_t)c);
	}

	/* Emits the string. */
	lex_emit(lexer, CSS_TOKEN_STRING, &lexer->scratch, 0, 0, 0);
}

/* Consumes a number, percentage or dimension token. */
static void
lex_numeric(
	struct css_lexer *lexer)
{
	char text[LEX_NUMBER_MAX + 8U];
	size_t used;
	int integer;
	int32_t c;
	int32_t next;
	double value;
	int starts;
	int digit;

	/* The sign and the integer digits. */
	used = 0;
	integer = 1;
	c = lex_peek(lexer, 0);
	if (c == '+' || c == '-') {
		text[used++] = (char)c;
		lexer->position++;
	}

	/* The digits after the sign. */
	used = lex_digits(lexer, text, used);

	/* The fraction. */
	c = lex_peek(lexer, 0);
	digit = lex_is_digit(lex_peek(lexer, 1));
	if (c == '.' && digit) {
		integer = 0;
		text[used++] = (char)lex_next(lexer);
		used = lex_digits(lexer, text, used);
	}

	/* The exponent: e, an optional sign, digits. */
	c = lex_peek(lexer, 0);
	next = lex_peek(lexer, 1);
	digit = lex_is_digit(next);
	if (next == '+' || next == '-')
		digit = lex_is_digit(lex_peek(lexer, 2));
	if ((c == 'e' || c == 'E') && digit) {
		integer = 0;
		text[used++] = (char)lex_next(lexer);
		if (next == '+' || next == '-')
			text[used++] = (char)lex_next(lexer);
		used = lex_digits(lexer, text, used);
	}

	/* Converts the number. */
	text[used] = '\0';
	value = strtod(text, NULL);

	/* An identifier after it makes a dimension. */
	starts = lex_starts_identifier(lexer, 0);
	if (starts) {
		lex_name(lexer, &lexer->scratch);
		lex_emit(lexer, CSS_TOKEN_DIMENSION, &lexer->scratch, value, integer, 0);
		return;
	}

	/* A percent sign makes a percentage. */
	c = lex_peek(lexer, 0);
	if (c == '%') {
		lexer->position++;
		lex_emit(lexer, CSS_TOKEN_PERCENTAGE, NULL, value, 0, 0);
		return;
	}

	/* A plain number. */
	lex_emit(lexer, CSS_TOKEN_NUMBER, NULL, value, integer, 0);
}

/* Copies the digits at the position into text (as far as there is room); returns the new length. */
static size_t
lex_digits(
	struct css_lexer *lexer,
	char *text,
	size_t used)
{
	int digit;

	/* Takes digits while they come. */
	for (;;) {
		digit = lex_is_digit(lex_peek(lexer, 0));
		if (!digit)
			break;

		/* Digits past the room are consumed and dropped. */
		if (used < LEX_NUMBER_MAX) {
			text[used] = (char)lex_next(lexer);
			used++;
		} else {
			lex_next(lexer);
		}
	}

	/* Reports the length. */
	return used;
}

/* Consumes an identifier, a function or a URL. */
static void
lex_ident_like(
	struct css_lexer *lexer)
{
	int32_t first;
	int32_t second;
	int first_space;
	int second_space;
	int is_url;
	int quoted;

	/* Reads the name. */
	lex_name(lexer, &lexer->scratch);
	is_url = css_units_equal_ascii(lexer->scratch.data, lexer->scratch.length, "url");

	/* url( with an unquoted argument is a URL token; with a quoted one, a function. */
	first = lex_peek(lexer, 0);
	if (is_url && first == '(') {
		lexer->position++;
		for (;;) {
			/* Whitespace is skipped while more whitespace follows it. */
			first = lex_peek(lexer, 0);
			second = lex_peek(lexer, 1);
			first_space = lex_is_whitespace(first);
			second_space = lex_is_whitespace(second);
			if (!first_space || !second_space)
				break;

			/* The space is skipped. */
			lexer->position++;
		}

		/* A quote next (after at most one space) makes it a function. */
		quoted = 0;
		if (first == '"' || first == '\'')
			quoted = 1;
		if (first_space && (second == '"' || second == '\''))
			quoted = 1;
		if (quoted) {
			lex_emit(lexer, CSS_TOKEN_FUNCTION, &lexer->scratch, 0, 0, 0);
			return;
		}

		/* Otherwise the URL itself. */
		lex_url(lexer);
		return;
	}

	/* A name followed by a parenthesis is a function. */
	if (first == '(') {
		lexer->position++;
		lex_emit(lexer, CSS_TOKEN_FUNCTION, &lexer->scratch, 0, 0, 0);
		return;
	}

	/* Otherwise an identifier. */
	lex_emit(lexer, CSS_TOKEN_IDENT, &lexer->scratch, 0, 0, 0);
}

/* Consumes an unquoted URL up to its closing parenthesis. */
static void
lex_url(
	struct css_lexer *lexer)
{
	int32_t c;
	int escape;
	int space;

	/* Skips the leading whitespace. */
	for (;;) {
		space = lex_is_whitespace(lex_peek(lexer, 0));
		if (!space)
			break;
		lexer->position++;
	}

	/* Gathers the characters up to the parenthesis. */
	wb_units_clear(&lexer->scratch);
	for (;;) {
		c = lex_next(lexer);
		if (c == ')' || c == LEX_EOF)
			break;

		/* Whitespace must be followed only by the closing parenthesis. */
		space = lex_is_whitespace(c);
		if (space) {
			for (;;) {
				c = lex_peek(lexer, 0);
				space = lex_is_whitespace(c);
				if (!space)
					break;
				lexer->position++;
			}

			/* Only the parenthesis may follow the whitespace. */
			if (c == ')' || c == LEX_EOF) {
				lex_next(lexer);
				break;
			}

			/* Anything else spoils the URL. */
			lex_bad_url(lexer);
			return;
		}

		/* Quotes and parentheses are not allowed unescaped. */
		if (c == '"' || c == '\'' || c == '(') {
			lex_bad_url(lexer);
			return;
		}

		/* An escape stands for its code point; a bad one spoils the URL. */
		if (c == '\\') {
			escape = lex_valid_escape(c, lex_peek(lexer, 0));
			if (!escape) {
				lex_bad_url(lexer);
				return;
			}

			/* The escape stands for its code point. */
			lex_append(lexer, &lexer->scratch, lex_escape(lexer));
			continue;
		}

		/* Anything else is the URL's. */
		lex_append(lexer, &lexer->scratch, (uint32_t)c);
	}

	/* Emits the URL. */
	lex_emit(lexer, CSS_TOKEN_URL, &lexer->scratch, 0, 0, 0);
}

/* Consumes the rest of a bad URL up to its closing parenthesis. */
static void
lex_bad_url(
	struct css_lexer *lexer)
{
	int32_t c;
	int escape;

	/* Skips to the parenthesis, passing over escapes. */
	for (;;) {
		c = lex_next(lexer);
		if (c == ')' || c == LEX_EOF)
			break;

		/* An escaped character does not end the URL. */
		escape = lex_valid_escape(c, lex_peek(lexer, 0));
		if (escape)
			lex_escape(lexer);
	}

	/* One bad URL token stands for it. */
	lex_emit(lexer, CSS_TOKEN_BAD_URL, NULL, 0, 0, 0);
}

/* Consumes a name (with escapes) into out. */
static void
lex_name(
	struct css_lexer *lexer,
	struct wb_units *out)
{
	int32_t c;
	int name;
	int escape;

	/* Takes name characters and escapes. */
	wb_units_clear(out);
	for (;;) {
		c = lex_peek(lexer, 0);

		/* A name character is taken as it is. */
		name = lex_is_name(c);
		if (name) {
			lex_next(lexer);
			lex_append(lexer, out, (uint32_t)c);
			continue;
		}

		/* An escape stands for its code point. */
		escape = lex_escape_ahead(lexer, 0);
		if (escape) {
			lex_next(lexer);
			lex_append(lexer, out, lex_escape(lexer));
			continue;
		}

		/* Anything else ends the name. */
		return;
	}
}

/* Consumes an escape after its backslash and reports the code point it stands for. */
static uint32_t
lex_escape(
	struct css_lexer *lexer)
{
	uint32_t value;
	int32_t c;
	int digits;
	int hex;
	int space;

	/* Up to six hex digits, then one optional whitespace. */
	c = lex_peek(lexer, 0);
	hex = lex_is_hex(c);
	if (hex) {
		value = 0;
		for (digits = 0; digits < 6; digits++) {
			/* The value grows by each hex digit. */
			c = lex_peek(lexer, 0);
			hex = lex_is_hex(c);
			if (!hex)
				break;
			lex_next(lexer);
			value *= 16U;
			if (c >= '0' && c <= '9') {
				value += (uint32_t)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				value += (uint32_t)(c - 'a' + 10);
			} else {
				value += (uint32_t)(c - 'A' + 10);
			}
		}

		/* One whitespace after the digits belongs to the escape. */
		space = lex_is_whitespace(lex_peek(lexer, 0));
		if (space)
			lex_next(lexer);

		/* Zero, surrogates and values past the last code point stand for U+FFFD. */
		if (value == 0 || value > WB_CODE_POINT_MAX)
			return WB_REPLACEMENT;
		if (value >= 0xd800U && value <= 0xdfffU)
			return WB_REPLACEMENT;
		return value;
	}

	/* The end of the input escapes to U+FFFD. */
	if (c == LEX_EOF)
		return WB_REPLACEMENT;

	/* Anything else stands for itself. */
	lex_next(lexer);
	return (uint32_t)c;
}

/* Appends a code point to a buffer; a failure stops the tokenizer. */
static void
lex_append(
	struct css_lexer *lexer,
	struct wb_units *units,
	uint32_t code_point)
{
	int error;

	/* Appends it as UTF-16. */
	error = wb_units_append_code_point(units, code_point);
	if (error != 0)
		lexer->failed = 1;
}
