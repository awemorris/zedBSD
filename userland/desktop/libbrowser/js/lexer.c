/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JavaScript lexer (ES2024 §12): white space, line terminators and
 * comments (with the HTML-like comments of scripts), identifiers (with
 * \u escapes), private names, numeric literals (with separators, the
 * hexadecimal, octal and binary forms, the legacy octal forms and BigInt
 * suffixes), string literals, template parts and regular expression
 * literals, and the punctuators.
 *
 * Whether a / starts a regular expression depends on the grammar, so the
 * parser says so with each call.  A template's parts after the first are
 * read when the parser reaches the } that ends a substitution.
 *
 * The first pass takes any character from U+0080 on that is not white
 * space or a line terminator as an identifier character; the exact
 * ID_Start and ID_Continue sets arrive with the Unicode tables.
 */

#include "js/internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The line terminators beyond LF and CR. */
#define LEXER_LS		0x2028U
#define LEXER_PS		0x2029U

/* The zero-width joiners that may continue an identifier. */
#define LEXER_ZWNJ		0x200CU
#define LEXER_ZWJ		0x200DU

/* The longest numeral read into a buffer for strtod. */
#define LEXER_NUMERAL_MAX	512U

/*
 * One punctuator's spelling and value, for the longest-match search.
 */
struct lexer_punctuator {
	const char *text;
	int punctuator;
};

/* The punctuators, longest first within each first character. */
static const struct lexer_punctuator lexer_punctuators[] = {
	{ ">>>=", JS_P_SHR_ASSIGN }, { "...", JS_P_ELLIPSIS }, { "===", JS_P_STRICT_EQ }, { "!==", JS_P_STRICT_NE },
	{ "**=", JS_P_POWER_ASSIGN }, { "<<=", JS_P_SHL_ASSIGN }, { ">>=", JS_P_SAR_ASSIGN }, { ">>>", JS_P_SHR },
	{ "&&=", JS_P_AND_ASSIGN }, { "||=", JS_P_OR_ASSIGN }, { "?\?=", JS_P_NULLISH_ASSIGN }, { "?.", JS_P_OPTIONAL },
	{ "=>", JS_P_ARROW }, { "==", JS_P_EQ }, { "!=", JS_P_NE }, { "<=", JS_P_LE }, { ">=", JS_P_GE },
	{ "**", JS_P_POWER }, { "++", JS_P_INCREMENT }, { "--", JS_P_DECREMENT }, { "<<", JS_P_SHL }, { ">>", JS_P_SAR },
	{ "&&", JS_P_AND }, { "||", JS_P_OR }, { "??", JS_P_NULLISH }, { "+=", JS_P_PLUS_ASSIGN }, { "-=", JS_P_MINUS_ASSIGN },
	{ "*=", JS_P_STAR_ASSIGN }, { "/=", JS_P_SLASH_ASSIGN }, { "%=", JS_P_PERCENT_ASSIGN }, { "&=", JS_P_AMP_ASSIGN },
	{ "|=", JS_P_BAR_ASSIGN }, { "^=", JS_P_CARET_ASSIGN },
	{ "{", JS_P_LBRACE }, { "}", JS_P_RBRACE }, { "(", JS_P_LPAREN }, { ")", JS_P_RPAREN }, { "[", JS_P_LBRACKET },
	{ "]", JS_P_RBRACKET }, { ".", JS_P_DOT }, { ";", JS_P_SEMICOLON }, { ",", JS_P_COMMA }, { "<", JS_P_LT },
	{ ">", JS_P_GT }, { "+", JS_P_PLUS }, { "-", JS_P_MINUS }, { "*", JS_P_STAR }, { "/", JS_P_SLASH },
	{ "%", JS_P_PERCENT }, { "&", JS_P_AMP }, { "|", JS_P_BAR }, { "^", JS_P_CARET }, { "!", JS_P_NOT },
	{ "~", JS_P_TILDE }, { "?", JS_P_QUESTION }, { ":", JS_P_COLON }, { "=", JS_P_ASSIGN },
	{ NULL, JS_P_NONE }
};

/* The spellings of the words, in the order of enum js_word. */
static const char *const lexer_words[JS_WORDS] = {
	NULL,
	"as", "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
	"default", "delete", "do", "else", "export", "extends", "false", "finally", "for", "from",
	"function", "get", "if", "import", "in", "instanceof", "let", "meta", "new", "null", "of", "return",
	"set", "static", "super", "switch", "target", "this", "throw", "true", "try", "typeof", "var",
	"void", "while", "with", "yield", "eval", "arguments", "constructor", "prototype", "__proto__",
};

/* The words reserved everywhere (await and yield depend on the context, which the parser knows). */
static const char *const lexer_reserved[] = {
	"break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else",
	"enum", "export", "extends", "false", "finally", "for", "function", "if", "import", "in", "instanceof", "new",
	"null", "return", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with",
	NULL
};

/* The words reserved in strict code only. */
static const char *const lexer_strict_reserved[] = {
	"implements", "interface", "let", "package", "private", "protected", "public", "static", "yield", NULL
};

static uint32_t lexer_peek(const struct js_lexer *lexer, uint32_t ahead);
static uint32_t lexer_code_point(const struct js_lexer *lexer, uint32_t at, uint32_t *size);
static int lexer_is_line_terminator(uint32_t unit);
static int lexer_is_space(uint32_t code_point);
static int lexer_is_identifier_start(uint32_t code_point);
static int lexer_is_identifier_part(uint32_t code_point);
static void lexer_newline(struct js_lexer *lexer, uint32_t at);
static void lexer_skip(struct js_lexer *lexer);
static void lexer_skip_line(struct js_lexer *lexer);
static void lexer_skip_block(struct js_lexer *lexer);
static void lexer_identifier(struct js_lexer *lexer, struct js_token *token);
static uint32_t lexer_unicode_escape(struct js_lexer *lexer, int *valid);
static void lexer_number(struct js_lexer *lexer, struct js_token *token);
static void lexer_digits(struct js_lexer *lexer, int base, char *numeral, size_t *count, int separators);
static void lexer_string(struct js_lexer *lexer, struct js_token *token);
static void lexer_template(struct js_lexer *lexer, struct js_token *token);
static int lexer_escape(struct js_lexer *lexer, struct wb_units *cooked, int template_part, struct js_token *token);
static void lexer_regexp(struct js_lexer *lexer, struct js_token *token);
static void lexer_punctuator(struct js_lexer *lexer, struct js_token *token);
static void lexer_finish(struct js_lexer *lexer, struct js_token *token, struct wb_units *units, const uint16_t *plain, size_t plain_length);
static int lexer_hex(uint32_t unit);
static void lexer_fail(struct js_lexer *lexer, const char *message);
static int lexer_line_ends(const struct js_lexer *lexer);
static int lexer_has_char(const char *text, size_t length, char wanted);

/*
 * Starts a lexer at the beginning of a source; a hashbang comment on the
 * first line is skipped.
 */
void
js_lexer_init(
	struct js_lexer *lexer,
	struct js_parser *parser,
	const uint16_t *source,
	size_t length,
	int module)
{
	/* The source, from its first character on line 1, and empty buffers. */
	memset(lexer, 0, sizeof(*lexer));
	wb_units_init(&lexer->cooked);
	wb_units_init(&lexer->raw);
	lexer->source = source;
	lexer->length = (uint32_t)length;
	lexer->line = 1;
	lexer->module = module;
	lexer->parser = parser;

	/* A hashbang (#!) at the very start is a comment to the end of its line. */
	if (length >= 2U && source[0] == '#' && source[1] == '!')
		lexer_skip_line(lexer);
}

/*
 * Reads the next token; regexp_allowed says whether a / there starts a
 * regular expression (it does where an expression can start).
 */
void
js_lexer_next(
	struct js_lexer *lexer,
	int regexp_allowed)
{
	struct js_token *token;
	uint32_t unit;
	uint32_t second;
	uint32_t size;
	uint32_t code_point;
	int starts_identifier;
	int numeral;

	/* The blanks and comments before the token, noting a line terminator among them. */
	token = &lexer->token;
	memset(token, 0, sizeof(*token));
	lexer_skip(lexer);
	token->newline_before = lexer->token.newline_before;

	/* Where the token starts. */
	token->start = lexer->position;
	token->line = lexer->line;
	token->column = lexer->position - lexer->line_start + 1U;
	lexer->token_on_line = 1;

	/* The end of the source. */
	if (lexer->position >= lexer->length) {
		token->kind = JS_TOKEN_END;
		token->end = lexer->position;
		return;
	}

	/* An identifier or a word. */
	unit = lexer->source[lexer->position];
	code_point = lexer_code_point(lexer, lexer->position, &size);
	starts_identifier = lexer_is_identifier_start(code_point);
	if (starts_identifier || unit == '\\') {
		lexer_identifier(lexer, token);
		return;
	}

	/* A private name: # and an identifier. */
	if (unit == '#') {
		lexer->position++;
		code_point = lexer_code_point(lexer, lexer->position, &size);
		starts_identifier = lexer_is_identifier_start(code_point);
		unit = lexer_peek(lexer, 0);
		if (!starts_identifier && unit != '\\')
			lexer_fail(lexer, "# must start a private name");
		lexer_identifier(lexer, token);
		token->kind = JS_TOKEN_PRIVATE_NAME;
		return;
	}

	/* A numeral: a digit, or . and a digit. */
	second = lexer_peek(lexer, 1);
	numeral = 0;
	if (unit >= '0' && unit <= '9')
		numeral = 1;
	if (unit == '.' && second >= '0' && second <= '9')
		numeral = 1;
	if (numeral) {
		lexer_number(lexer, token);
		return;
	}

	/* A string. */
	if (unit == '"' || unit == '\'') {
		lexer_string(lexer, token);
		return;
	}

	/* A template's first part. */
	if (unit == '`') {
		lexer->position++;
		lexer_template(lexer, token);
		return;
	}

	/* A regular expression where one may start. */
	if (unit == '/' && regexp_allowed) {
		lexer_regexp(lexer, token);
		return;
	}

	/* Otherwise a punctuator. */
	lexer_punctuator(lexer, token);
}

/*
 * Reads a template's next part after the } that ends a substitution (the
 * current token).
 */
void
js_lexer_template_continue(
	struct js_lexer *lexer)
{
	struct js_token *token;
	uint32_t start;
	uint32_t line;
	uint32_t column;

	/* The part starts right after the }. */
	token = &lexer->token;
	start = token->start;
	line = token->line;
	column = token->column;
	lexer->position = token->end;
	memset(token, 0, sizeof(*token));
	token->start = start;
	token->line = line;
	token->column = column;

	/* Reads it. */
	lexer_template(lexer, token);
}

/*
 * Copies a lexer's state, to go back to it after looking ahead.
 */
void
js_lexer_save(
	const struct js_lexer *lexer,
	struct js_lexer *saved)
{
	/* The whole state, token included. */
	*saved = *lexer;
}

/*
 * Goes back to a lexer's saved state (the buffers are the lexer's own,
 * which may have grown since, and stay).
 */
void
js_lexer_restore(
	struct js_lexer *lexer,
	const struct js_lexer *saved)
{
	struct wb_units cooked;
	struct wb_units raw;

	/* The state, token included, but the live buffers. */
	cooked = lexer->cooked;
	raw = lexer->raw;
	*lexer = *saved;
	lexer->cooked = cooked;
	lexer->raw = raw;
}

/*
 * Frees the lexer's buffers.
 */
void
js_lexer_release(
	struct js_lexer *lexer)
{
	/* The two buffers. */
	wb_units_release(&lexer->cooked);
	wb_units_release(&lexer->raw);
}

/*
 * Tells whether a token is an identifier spelled as a word without
 * escapes (a keyword written with an escape is not the keyword).
 */
int
js_token_is(
	const struct js_token *token,
	const char *word)
{
	int same;

	/* Only identifiers are words, and an escaped one never matches. */
	if (token->kind != JS_TOKEN_IDENTIFIER || token->escaped)
		return 0;

	/* The characters. */
	same = js_text_is(token->text, token->text_length, word);

	/* Reports whether they match. */
	return same;
}

/*
 * Tells whether an identifier token (escapes decoded) is a reserved word,
 * counting the words reserved in strict code when strict is set.
 */
int
js_token_is_reserved(
	const struct js_token *token,
	int strict)
{
	int reserved;

	/* The token's decoded characters. */
	reserved = js_word_is_reserved(token->text, token->text_length, strict);

	/* Reports it. */
	return reserved;
}

/*
 * Reports the js_word a text spells, or JS_W_NONE.
 */
int
js_word_of(
	const uint16_t *text,
	size_t length)
{
	int word;
	int same;

	/* Only short ASCII words are in the table. */
	if (length == 0 || length > 11U || text[0] >= 0x80U)
		return JS_W_NONE;

	/* Each word in turn. */
	for (word = JS_W_NONE + 1; word < JS_WORDS; word++) {
		same = js_text_is(text, length, lexer_words[word]);
		if (same)
			return word;
	}

	/* No word of the table. */
	return JS_W_NONE;
}

/*
 * Tells whether a word (escapes decoded) is reserved, counting the words
 * reserved in strict code when strict is set.
 */
int
js_word_is_reserved(
	const uint16_t *text,
	size_t length,
	int strict)
{
	size_t index;
	int same;

	/* The words reserved everywhere (await and yield are decided by the parser's context). */
	for (index = 0; lexer_reserved[index] != NULL; index++) {
		same = js_text_is(text, length, lexer_reserved[index]);
		if (same)
			return 1;
	}

	/* Sloppy code reserves no more. */
	if (!strict)
		return 0;

	/* The words strict code reserves. */
	for (index = 0; lexer_strict_reserved[index] != NULL; index++) {
		same = js_text_is(text, length, lexer_strict_reserved[index]);
		if (same)
			return 1;
	}

	/* Not reserved. */
	return 0;
}

/* Reports the code unit some units ahead, or 0 past the end. */
static uint32_t
lexer_peek(
	const struct js_lexer *lexer,
	uint32_t ahead)
{
	/* Past the end is nothing. */
	if (lexer->position + ahead >= lexer->length)
		return 0;

	/* The unit. */
	return lexer->source[lexer->position + ahead];
}

/* Reads the code point at a place (a surrogate pair is one) and how many units it takes. */
static uint32_t
lexer_code_point(
	const struct js_lexer *lexer,
	uint32_t at,
	uint32_t *size)
{
	uint32_t high;
	uint32_t low;

	/* Past the end is nothing. */
	*size = 1;
	if (at >= lexer->length)
		return 0;

	/* A high surrogate followed by a low one is a pair. */
	high = lexer->source[at];
	if (high >= 0xD800U && high <= 0xDBFFU && at + 1U < lexer->length) {
		low = lexer->source[at + 1U];
		if (low >= 0xDC00U && low <= 0xDFFFU) {
			*size = 2;
			return 0x10000U + ((high - 0xD800U) << 10) + (low - 0xDC00U);
		}
	}

	/* Any other unit is itself. */
	return high;
}

/* Tells whether a unit is a line terminator. */
static int
lexer_is_line_terminator(
	uint32_t unit)
{
	/* LF, CR, LS and PS. */
	if (unit == 0x0AU || unit == 0x0DU)
		return 1;
	if (unit == LEXER_LS || unit == LEXER_PS)
		return 1;

	/* Nothing else. */
	return 0;
}

/* Tells whether a code point is white space (not a line terminator). */
static int
lexer_is_space(
	uint32_t code_point)
{
	/* Tab, vertical tab, form feed, space, no-break space and the byte order mark. */
	if (code_point == 0x09U || code_point == 0x0BU || code_point == 0x0CU || code_point == 0x20U)
		return 1;
	if (code_point == 0xA0U || code_point == 0xFEFFU)
		return 1;

	/* The other space separators (Zs). */
	if (code_point == 0x1680U || (code_point >= 0x2000U && code_point <= 0x200AU))
		return 1;
	if (code_point == 0x202FU || code_point == 0x205FU || code_point == 0x3000U)
		return 1;

	/* Nothing else. */
	return 0;
}

/* Tells whether a code point may start an identifier. */
static int
lexer_is_identifier_start(
	uint32_t code_point)
{
	int space;
	int terminator;

	/* The ASCII letters, $ and _. */
	if ((code_point >= 'a' && code_point <= 'z') || (code_point >= 'A' && code_point <= 'Z'))
		return 1;
	if (code_point == '$' || code_point == '_')
		return 1;

	/* ASCII's other characters never start one. */
	if (code_point < 0x80U)
		return 0;

	/* Beyond ASCII, anything that is not a blank or a line terminator (until the Unicode tables). */
	space = lexer_is_space(code_point);
	terminator = lexer_is_line_terminator(code_point);
	if (space || terminator)
		return 0;
	if (code_point == LEXER_ZWNJ || code_point == LEXER_ZWJ)
		return 0;

	/* A letter, as far as this pass can tell. */
	return 1;
}

/* Tells whether a code point may continue an identifier. */
static int
lexer_is_identifier_part(
	uint32_t code_point)
{
	int start;

	/* What may start one, and the digits and the joiners. */
	start = lexer_is_identifier_start(code_point);
	if (start)
		return 1;
	if (code_point >= '0' && code_point <= '9')
		return 1;
	if (code_point == LEXER_ZWNJ || code_point == LEXER_ZWJ)
		return 1;

	/* Nothing else. */
	return 0;
}

/* Counts a line terminator at a place: the next line starts after it (a CR LF pair is one). */
static void
lexer_newline(
	struct js_lexer *lexer,
	uint32_t at)
{
	/* CR LF is one line terminator. */
	if (lexer->source[at] == 0x0DU && at + 1U < lexer->length && lexer->source[at + 1U] == 0x0AU)
		at++;

	/* The next line (the caller says whether it comes before a token). */
	lexer->line++;
	lexer->line_start = at + 1U;
	lexer->position = at + 1U;
	lexer->token_on_line = 0;
}

/* Skips white space, line terminators and comments before a token. */
static void
lexer_skip(
	struct js_lexer *lexer)
{
	uint32_t unit;
	uint32_t second;
	uint32_t third;
	uint32_t fourth;
	uint32_t size;
	uint32_t code_point;
	int space;
	int terminator;
	int html_open;
	int html_close;

	/* Until something that is neither. */
	while (lexer->position < lexer->length) {
		unit = lexer->source[lexer->position];
		second = lexer_peek(lexer, 1);
		third = lexer_peek(lexer, 2);
		fourth = lexer_peek(lexer, 3);

		/* A line terminator comes before the token. */
		terminator = lexer_is_line_terminator(unit);
		if (terminator) {
			lexer_newline(lexer, lexer->position);
			lexer->token.newline_before = 1;
			continue;
		}

		/* White space. */
		code_point = lexer_code_point(lexer, lexer->position, &size);
		space = lexer_is_space(code_point);
		if (space) {
			lexer->position += size;
			continue;
		}

		/* A comment to the end of the line. */
		if (unit == '/' && second == '/') {
			lexer_skip_line(lexer);
			continue;
		}

		/* A block comment. */
		if (unit == '/' && second == '*') {
			lexer_skip_block(lexer);
			continue;
		}

		/* A script's HTML-like comments: <!-- anywhere, --> at the start of a line. */
		html_open = 0;
		if (unit == '<' && second == '!' && third == '-' && fourth == '-')
			html_open = 1;
		html_close = 0;
		if (!lexer->token_on_line && unit == '-' && second == '-' && third == '>')
			html_close = 1;
		if (!lexer->module && (html_open || html_close)) {
			lexer_skip_line(lexer);
			continue;
		}

		/* A token starts here. */
		return;
	}
}

/* Skips to the end of the line (the terminator itself is left for the caller). */
static void
lexer_skip_line(
	struct js_lexer *lexer)
{
	int terminator;

	/* Every unit up to a line terminator. */
	while (lexer->position < lexer->length) {
		terminator = lexer_is_line_terminator(lexer->source[lexer->position]);
		if (terminator)
			break;
		lexer->position++;
	}
}

/* Skips a block comment, counting its line terminators; an unclosed one is an error. */
static void
lexer_skip_block(
	struct js_lexer *lexer)
{
	uint32_t unit;
	uint32_t second;
	int terminator;

	/* Past the opening. */
	lexer->position += 2U;
	while (lexer->position < lexer->length) {
		/* The closing ends it. */
		unit = lexer->source[lexer->position];
		second = lexer_peek(lexer, 1);
		if (unit == '*' && second == '/') {
			lexer->position += 2U;
			return;
		}

		/* A line terminator inside counts as one before the next token. */
		terminator = lexer_is_line_terminator(unit);
		if (terminator) {
			lexer_newline(lexer, lexer->position);
			lexer->token.newline_before = 1;
			continue;
		}

		/* Anything else is skipped. */
		lexer->position++;
	}

	/* The source ended inside the comment. */
	lexer_fail(lexer, "unterminated comment");
}

/* Reads an identifier (escapes decoded into the arena, else the text points into the source). */
static void
lexer_identifier(
	struct js_lexer *lexer,
	struct js_token *token)
{
	struct wb_units *units;
	uint32_t start;
	uint32_t size;
	uint32_t code_point;
	uint32_t unit;
	uint32_t second;
	int first;
	int allowed;
	int valid;

	/* Each character, the first by the start rule and the rest by the part rule. */
	units = &lexer->cooked;
	wb_units_clear(units);
	start = lexer->position;
	first = 1;
	for (;;) {
		/* An escape stands for the character it names, which must fit where it is. */
		unit = lexer_peek(lexer, 0);
		second = lexer_peek(lexer, 1);
		if (unit == '\\') {
			if (second != 'u')
				lexer_fail(lexer, "invalid escape in an identifier");
			lexer->position += 2U;
			code_point = lexer_unicode_escape(lexer, &valid);
			if (!valid)
				lexer_fail(lexer, "invalid escape in an identifier");
			allowed = lexer_is_identifier_part(code_point);
			if (first)
				allowed = lexer_is_identifier_start(code_point);
			if (!allowed)
				lexer_fail(lexer, "an escape in an identifier names a character it cannot hold");
			token->escaped = 1;
			wb_units_append_code_point(units, code_point);
			first = 0;
			continue;
		}

		/* A plain character that continues the identifier. */
		code_point = lexer_code_point(lexer, lexer->position, &size);
		allowed = lexer_is_identifier_part(code_point);
		if (first)
			allowed = lexer_is_identifier_start(code_point);
		if (lexer->position >= lexer->length || !allowed)
			break;
		wb_units_append(units, &lexer->source[lexer->position], size);
		lexer->position += size;
		first = 0;
	}

	/* The token: the source's characters, or the decoded ones when there was an escape. */
	token->kind = JS_TOKEN_IDENTIFIER;
	if (token->escaped) {
		lexer_finish(lexer, token, units, NULL, 0);
	} else {
		lexer_finish(lexer, token, units, &lexer->source[start], lexer->position - start);
	}

	/* The word it spells; a keyword only without escapes. */
	token->word = js_word_of(token->text, token->text_length);
	token->keyword = JS_W_NONE;
	if (!token->escaped)
		token->keyword = token->word;
}

/* Reads the digits of a \u escape after the u: four hexadecimal digits or {code point}. */
static uint32_t
lexer_unicode_escape(
	struct js_lexer *lexer,
	int *valid)
{
	uint32_t code_point;
	uint32_t digits;
	uint32_t unit;
	int value;

	/* The braced form: one or more digits up to U+10FFFF. */
	*valid = 0;
	code_point = 0;
	unit = lexer_peek(lexer, 0);
	if (unit == '{') {
		lexer->position++;
		digits = 0;
		for (;;) {
			value = lexer_hex(lexer_peek(lexer, 0));
			if (value < 0)
				break;
			code_point = code_point * 16U + (uint32_t)value;
			if (code_point > 0x10FFFFU)
				return 0;
			lexer->position++;
			digits++;
		}

		/* The brace must close a code point of at least one digit. */
		unit = lexer_peek(lexer, 0);
		if (digits == 0 || unit != '}')
			return 0;
		lexer->position++;
		*valid = 1;
		return code_point;
	}

	/* Four digits. */
	for (digits = 0; digits < 4U; digits++) {
		value = lexer_hex(lexer_peek(lexer, 0));
		if (value < 0)
			return 0;
		code_point = code_point * 16U + (uint32_t)value;
		lexer->position++;
	}

	/* Succeeded: the code point. */
	*valid = 1;
	return code_point;
}

/* Reads a numeric literal. */
static void
lexer_number(
	struct js_lexer *lexer,
	struct js_token *token)
{
	char numeral[LEXER_NUMERAL_MAX + 1U];
	size_t count;
	uint32_t start;
	uint32_t size;
	uint32_t next;
	uint32_t unit;
	uint32_t second;
	uint32_t prefix;
	uint32_t index;
	int base;
	int legacy;
	int decimal;
	int bigint;
	int all_octal;
	int fraction;
	int exponent;
	int follows;
	double value;

	/* The prefixed forms: 0x, 0o and 0b. */
	start = lexer->position;
	count = 0;
	base = 10;
	legacy = 0;
	decimal = 1;
	unit = lexer_peek(lexer, 0);
	second = lexer_peek(lexer, 1);
	prefix = second | 0x20U;
	if (unit == '0' && (prefix == 'x' || prefix == 'o' || prefix == 'b')) {
		if (prefix == 'x')
			base = 16;
		if (prefix == 'o')
			base = 8;
		if (prefix == 'b')
			base = 2;
		lexer->position += 2U;
		lexer_digits(lexer, base, numeral, &count, 1);
		if (count == 0)
			lexer_fail(lexer, "a numeral's prefix without digits");
		decimal = 0;
	} else if (unit == '0' && second >= '0' && second <= '9') {
		/* A leading zero followed by digits: legacy octal, or decimal when an 8 or a 9 is among them. */
		legacy = 1;
		lexer_digits(lexer, 10, numeral, &count, 0);
		all_octal = 1;
		for (index = 0; index < count; index++) {
			if (numeral[index] > '7')
				all_octal = 0;
		}

		/* All octal digits make an octal numeral. */
		if (all_octal) {
			base = 8;
			decimal = 0;
		}
	} else {
		/* A decimal integer part (possibly empty before a .); a separator never follows a leading 0. */
		if (unit == '0' && second == '_')
			lexer_fail(lexer, "a numeric separator after a leading 0");
		lexer_digits(lexer, 10, numeral, &count, 1);
	}

	/* A decimal numeral may have a fraction and an exponent. */
	bigint = 0;
	if (decimal) {
		unit = lexer_peek(lexer, 0);
		if (unit == '.') {
			numeral[count++] = '.';
			lexer->position++;
			lexer_digits(lexer, 10, numeral, &count, 1);
		}

		/* The exponent. */
		unit = lexer_peek(lexer, 0);
		if ((unit | 0x20U) == 'e') {
			numeral[count++] = 'e';
			lexer->position++;
			unit = lexer_peek(lexer, 0);
			if (unit == '+' || unit == '-') {
				numeral[count++] = (char)unit;
				lexer->position++;
			}

			/* At least one digit. */
			next = lexer_peek(lexer, 0);
			if (next < '0' || next > '9')
				lexer_fail(lexer, "an exponent without digits");
			lexer_digits(lexer, 10, numeral, &count, 1);
		}
	}

	/* A BigInt's n follows an integer without a fraction or an exponent, never a legacy one. */
	unit = lexer_peek(lexer, 0);
	if (unit == 'n') {
		fraction = lexer_has_char(numeral, count, '.');
		exponent = 0;
		if (decimal)
			exponent = lexer_has_char(numeral, count, 'e');
		if (legacy || fraction || exponent)
			lexer_fail(lexer, "invalid BigInt literal");
		bigint = 1;
		lexer->position++;
	}

	/* The numeral must not run into an identifier or a digit. */
	next = lexer_code_point(lexer, lexer->position, &size);
	follows = lexer_is_identifier_start(next);
	if (next == '\\' || (next >= '0' && next <= '9'))
		follows = 1;
	if (follows)
		lexer_fail(lexer, "an identifier starts right after a numeral");

	/* The value, read exactly and rounded once: a decimal numeral, or the digits of another base. */
	numeral[count] = '\0';
	if (decimal && base == 10) {
		value = vm_number_parse(numeral, count);
	} else {
		value = vm_number_parse_radix(numeral, count, (int)base);
	}

	/* The token. */
	token->kind = JS_TOKEN_NUMBER;
	if (bigint)
		token->kind = JS_TOKEN_BIGINT;
	token->number = value;
	token->legacy_octal = legacy;
	token->text = &lexer->source[start];
	token->text_length = lexer->position - start;
	token->end = lexer->position;
}

/* Reads digits of a base into a numeral (separators _ between digits allowed when asked). */
static void
lexer_digits(
	struct js_lexer *lexer,
	int base,
	char *numeral,
	size_t *count,
	int separators)
{
	uint32_t unit;
	int value;
	int after_digit;

	/* Each digit, or a separator between two. */
	after_digit = 0;
	for (;;) {
		unit = lexer_peek(lexer, 0);

		/* A separator must sit between two digits. */
		if (unit == '_' && separators) {
			value = lexer_hex(lexer_peek(lexer, 1));
			if (!after_digit || value < 0 || value >= base)
				lexer_fail(lexer, "a numeric separator must sit between digits");
			lexer->position++;
			after_digit = 0;
			continue;
		}

		/* A digit of the base; past the buffer's room the rest only moves on (their precision is lost anyway). */
		value = lexer_hex(unit);
		if (value < 0 || value >= base)
			break;
		if (*count < LEXER_NUMERAL_MAX - 32U)
			numeral[(*count)++] = (char)unit;
		lexer->position++;
		after_digit = 1;
	}
}

/* Reads a string literal. */
static void
lexer_string(
	struct js_lexer *lexer,
	struct js_token *token)
{
	struct wb_units *units;
	uint32_t quote;
	uint32_t unit;
	uint32_t start;
	int escaped;

	/* After the opening quote, up to the same quote. */
	units = &lexer->cooked;
	wb_units_clear(units);
	quote = lexer->source[lexer->position];
	lexer->position++;
	start = lexer->position;
	escaped = 0;
	for (;;) {
		/* The source must not end inside. */
		if (lexer->position >= lexer->length)
			lexer_fail(lexer, "unterminated string");

		/* The closing quote. */
		unit = lexer->source[lexer->position];
		if (unit == quote)
			break;

		/* LF and CR cannot be in a string (LS and PS can). */
		if (unit == 0x0AU || unit == 0x0DU)
			lexer_fail(lexer, "unterminated string");

		/* An escape. */
		if (unit == '\\') {
			escaped = 1;
			lexer_escape(lexer, units, 0, token);
			continue;
		}

		/* A plain character. */
		wb_units_append(units, &lexer->source[lexer->position], 1);
		lexer->position++;
	}

	/* The token: the cooked text, or the source's when there was no escape (a directive must have none). */
	token->kind = JS_TOKEN_STRING;
	token->escaped = escaped;
	if (escaped) {
		lexer->position++;
		lexer_finish(lexer, token, units, NULL, 0);
	} else {
		lexer_finish(lexer, token, units, &lexer->source[start], lexer->position - start);
		lexer->position++;
	}

	/* The token ends after the quote. */
	token->end = lexer->position;

	/* The word it spells (a string key may be constructor or __proto__). */
	token->word = js_word_of(token->text, token->text_length);
}

/*
 * Reads a template part after its ` or }: up to ` (the last part) or ${
 * (a substitution follows), cooking the escapes and normalizing the raw
 * line terminators.
 */
static void
lexer_template(
	struct js_lexer *lexer,
	struct js_token *token)
{
	struct wb_units *cooked;
	struct wb_units *raw;
	uint32_t unit;
	uint32_t second;
	uint32_t before;
	uint16_t newline;
	int valid;
	int terminator;

	/* Each character up to the end of the part. */
	cooked = &lexer->cooked;
	raw = &lexer->raw;
	wb_units_clear(cooked);
	wb_units_clear(raw);
	newline = 0x0AU;
	token->kind = JS_TOKEN_TEMPLATE;
	for (;;) {
		/* The source must not end inside. */
		if (lexer->position >= lexer->length)
			lexer_fail(lexer, "unterminated template");

		/* ` ends the template. */
		unit = lexer->source[lexer->position];
		if (unit == '`') {
			lexer->position++;
			token->template_tail = 1;
			break;
		}

		/* ${ starts a substitution. */
		second = lexer_peek(lexer, 1);
		if (unit == '$' && second == '{') {
			lexer->position += 2U;
			break;
		}

		/* An escape: its raw characters, and its cooked value unless it is invalid. */
		if (unit == '\\') {
			before = lexer->position;
			valid = lexer_escape(lexer, cooked, 1, token);
			if (!valid)
				token->invalid_cooked = 1;
			wb_units_append(raw, &lexer->source[before], lexer->position - before);
			continue;
		}

		/* A line terminator is counted; CR and CR LF become LF. */
		terminator = lexer_is_line_terminator(unit);
		if (terminator) {
			if (unit == 0x0DU) {
				wb_units_append(cooked, &newline, 1);
				wb_units_append(raw, &newline, 1);
			} else {
				wb_units_append(cooked, &lexer->source[lexer->position], 1);
				wb_units_append(raw, &lexer->source[lexer->position], 1);
			}

			/* The next line. */
			lexer_newline(lexer, lexer->position);
			continue;
		}

		/* A plain character. */
		wb_units_append(cooked, &lexer->source[lexer->position], 1);
		wb_units_append(raw, &lexer->source[lexer->position], 1);
		lexer->position++;
	}

	/* The token's cooked and raw texts. */
	lexer_finish(lexer, token, cooked, NULL, 0);
	token->raw = js_arena_copy(lexer->parser, raw->data, raw->length * sizeof(uint16_t));
	token->raw_length = raw->length;
	token->end = lexer->position;
}

/*
 * Reads an escape (at its \) into the cooked text; reports whether it was
 * valid.  An invalid escape is a syntax error in a string and makes the
 * cooked value undefined in a template.  Legacy octal escapes (and \8,
 * \9) are marked on the token, since strict code forbids them.
 */
static int
lexer_escape(
	struct js_lexer *lexer,
	struct wb_units *cooked,
	int template_part,
	struct js_token *token)
{
	uint32_t unit;
	uint32_t next;
	uint32_t code_point;
	uint32_t value;
	uint16_t single;
	int high;
	int low;
	int valid;
	int terminator;

	/* The character after the backslash. */
	lexer->position++;
	if (lexer->position >= lexer->length)
		lexer_fail(lexer, "unterminated escape");
	unit = lexer->source[lexer->position];

	/* A line continuation adds nothing. */
	terminator = lexer_is_line_terminator(unit);
	if (terminator) {
		lexer_newline(lexer, lexer->position);
		return 1;
	}

	/* \x: two hexadecimal digits. */
	if (unit == 'x') {
		high = lexer_hex(lexer_peek(lexer, 1));
		low = lexer_hex(lexer_peek(lexer, 2));
		if (high < 0 || low < 0) {
			if (!template_part)
				lexer_fail(lexer, "invalid \\x escape");
			lexer->position++;
			return 0;
		}

		/* The byte they name. */
		single = (uint16_t)(high * 16 + low);
		wb_units_append(cooked, &single, 1);
		lexer->position += 3U;
		return 1;
	}

	/* \u: four digits or a braced code point. */
	if (unit == 'u') {
		lexer->position++;
		code_point = lexer_unicode_escape(lexer, &valid);
		if (!valid) {
			if (!template_part)
				lexer_fail(lexer, "invalid \\u escape");
			return 0;
		}

		/* The code point they name. */
		wb_units_append_code_point(cooked, code_point);
		return 1;
	}

	/* \0 not followed by a digit is NUL. */
	next = lexer_peek(lexer, 1);
	if (unit == '0' && (next < '0' || next > '9')) {
		single = 0;
		wb_units_append(cooked, &single, 1);
		lexer->position++;
		return 1;
	}

	/* The legacy octal escapes (up to \377), and \8 and \9: never in a template, marked in a string. */
	if (unit >= '0' && unit <= '9') {
		if (template_part) {
			lexer->position++;
			return 0;
		}

		/* \8 and \9 stand for themselves. */
		token->legacy_octal = 1;
		if (unit >= '8') {
			single = (uint16_t)unit;
			wb_units_append(cooked, &single, 1);
			lexer->position++;
			return 1;
		}

		/* One to three octal digits (three only from \0 to \3). */
		value = unit - '0';
		lexer->position++;
		next = lexer_peek(lexer, 0);
		if (next >= '0' && next <= '7') {
			value = value * 8U + (next - '0');
			lexer->position++;
			next = lexer_peek(lexer, 0);
			if (unit <= '3' && next >= '0' && next <= '7') {
				value = value * 8U + (next - '0');
				lexer->position++;
			}
		}

		/* The character they name. */
		single = (uint16_t)value;
		wb_units_append(cooked, &single, 1);
		return 1;
	}

	/* The single-character escapes. */
	switch (unit) {
	case 'b':
		single = 0x08U;
		break;
	case 't':
		single = 0x09U;
		break;
	case 'n':
		single = 0x0AU;
		break;
	case 'v':
		single = 0x0BU;
		break;
	case 'f':
		single = 0x0CU;
		break;
	case 'r':
		single = 0x0DU;
		break;
	default:
		/* Any other character stands for itself. */
		single = (uint16_t)unit;
		break;
	}

	/* The character. */
	wb_units_append(cooked, &single, 1);
	lexer->position++;
	return 1;
}

/* Reads a regular expression literal: its body (not checked until the RegExp engine arrives) and flags. */
static void
lexer_regexp(
	struct js_lexer *lexer,
	struct js_token *token)
{
	uint32_t start;
	uint32_t end;
	uint32_t unit;
	uint32_t size;
	uint32_t code_point;
	int in_class;
	int part;
	int ended;

	/* The body, up to a / outside a class. */
	lexer->position++;
	start = lexer->position;
	in_class = 0;
	for (;;) {
		/* The body must end on its line. */
		ended = lexer_line_ends(lexer);
		if (ended)
			lexer_fail(lexer, "unterminated regular expression");
		unit = lexer->source[lexer->position];

		/* An escape takes the next character, which must not end the line. */
		if (unit == '\\') {
			lexer->position++;
			ended = lexer_line_ends(lexer);
			if (ended)
				lexer_fail(lexer, "unterminated regular expression");
			lexer->position++;
			continue;
		}

		/* A class holds / as a plain character. */
		if (unit == '[')
			in_class = 1;
		if (unit == ']')
			in_class = 0;

		/* The closing /. */
		if (unit == '/' && !in_class)
			break;
		lexer->position++;
	}

	/* The body ends before the closing /. */
	end = lexer->position;
	lexer->position++;

	/* The flags: identifier characters. */
	token->raw = &lexer->source[lexer->position];
	for (;;) {
		code_point = lexer_code_point(lexer, lexer->position, &size);
		part = lexer_is_identifier_part(code_point);
		if (lexer->position >= lexer->length || !part)
			break;
		lexer->position += size;
	}

	/* An escape cannot spell a flag. */
	unit = lexer_peek(lexer, 0);
	if (unit == '\\')
		lexer_fail(lexer, "an escape in a regular expression's flags");

	/* The token. */
	token->kind = JS_TOKEN_REGEXP;
	token->raw_length = (size_t)(&lexer->source[lexer->position] - token->raw);
	token->text = &lexer->source[start];
	token->text_length = end - start;
	token->end = lexer->position;
}

/* Reads a punctuator by the longest spelling that matches. */
static void
lexer_punctuator(
	struct js_lexer *lexer,
	struct js_token *token)
{
	const struct lexer_punctuator *candidate;
	uint32_t unit;
	uint32_t third;
	size_t length;
	size_t index;
	int matches;

	/* The first spelling that matches (the table is longest first). */
	for (candidate = lexer_punctuators; candidate->text != NULL; candidate++) {
		length = strlen(candidate->text);
		matches = 1;
		for (index = 0; index < length; index++) {
			unit = lexer_peek(lexer, (uint32_t)index);
			if (unit != (uint32_t)(unsigned char)candidate->text[index])
				matches = 0;
		}

		/* Another spelling. */
		if (!matches)
			continue;

		/* ?. followed by a digit is ? and a numeral (a ? b ?.5 : c). */
		third = lexer_peek(lexer, 2);
		if (candidate->punctuator == JS_P_OPTIONAL && third >= '0' && third <= '9')
			continue;

		/* The token. */
		token->kind = JS_TOKEN_PUNCTUATOR;
		token->punctuator = candidate->punctuator;
		lexer->position += (uint32_t)length;
		token->end = lexer->position;
		return;
	}

	/* Nothing matches. */
	lexer_fail(lexer, "unexpected character");
}

/* Gives a token its text: the plain source when it had no escape, else the cooked units copied to the arena. */
static void
lexer_finish(
	struct js_lexer *lexer,
	struct js_token *token,
	struct wb_units *units,
	const uint16_t *plain,
	size_t plain_length)
{
	/* The source's own characters. */
	token->end = lexer->position;
	if (plain != NULL) {
		token->text = plain;
		token->text_length = plain_length;
		return;
	}

	/* A copy of the cooked characters. */
	token->text = js_arena_copy(lexer->parser, units->data, units->length * sizeof(uint16_t));
	token->text_length = units->length;
}

/* Reports a hexadecimal digit's value, or -1. */
static int
lexer_hex(
	uint32_t unit)
{
	/* The digits and the letters in either case. */
	if (unit >= '0' && unit <= '9')
		return (int)(unit - '0');
	if (unit >= 'a' && unit <= 'f')
		return (int)(unit - 'a' + 10U);
	if (unit >= 'A' && unit <= 'F')
		return (int)(unit - 'A' + 10U);

	/* Not a digit. */
	return -1;
}

/* Tells whether a numeral's text has a character. */
static int
lexer_has_char(
	const char *text,
	size_t length,
	char wanted)
{
	const char *found;

	/* The first one, if any. */
	found = memchr(text, wanted, length);
	if (found == NULL)
		return 0;

	/* It has. */
	return 1;
}

/* Tells whether the source or the line ends at the lexer's place. */
static int
lexer_line_ends(
	const struct js_lexer *lexer)
{
	int terminator;

	/* The source's end. */
	if (lexer->position >= lexer->length)
		return 1;

	/* A line terminator. */
	terminator = lexer_is_line_terminator(lexer->source[lexer->position]);

	/* Reports it. */
	return terminator;
}

/* Fails the parse at the lexer's place. */
static void
lexer_fail(
	struct js_lexer *lexer,
	const char *message)
{
	/* The line and the column of the character being read. */
	js_fail_at(lexer->parser, lexer->line, lexer->position - lexer->line_start + 1U, message);
}
