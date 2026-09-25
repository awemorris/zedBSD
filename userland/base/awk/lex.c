/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lexer of awk: turns the program text into tokens.
 *
 * A / is always read as division here; the parser, which knows when an
 * operand is expected, asks for the text after it to be read again as a
 * regex.  A name directly followed by ( is a function name, since a call
 * of a function of the program is written without a space.
 */

#include "userland/base/awk/awk.h"

#include <stdlib.h>
#include <string.h>

/* A word the lexer gives a meaning to, and its token. */
struct keyword {
	const char *name;
	int token;
	int builtin;
};

/*
 * The keywords and the built-in functions.  The table is constant and
 * searched in order.
 */
static const struct keyword keywords[] = {
	{ "BEGIN", TOKEN_BEGIN, 0 },
	{ "END", TOKEN_END, 0 },
	{ "function", TOKEN_FUNCTION, 0 },
	{ "getline", TOKEN_GETLINE, 0 },
	{ "print", TOKEN_PRINT, 0 },
	{ "printf", TOKEN_PRINTF, 0 },
	{ "if", TOKEN_IF, 0 },
	{ "else", TOKEN_ELSE, 0 },
	{ "while", TOKEN_WHILE, 0 },
	{ "for", TOKEN_FOR, 0 },
	{ "do", TOKEN_DO, 0 },
	{ "break", TOKEN_BREAK, 0 },
	{ "continue", TOKEN_CONTINUE, 0 },
	{ "next", TOKEN_NEXT, 0 },
	{ "nextfile", TOKEN_NEXTFILE, 0 },
	{ "exit", TOKEN_EXIT, 0 },
	{ "return", TOKEN_RETURN, 0 },
	{ "delete", TOKEN_DELETE, 0 },
	{ "in", TOKEN_IN, 0 },
	{ "length", TOKEN_BUILTIN, BUILTIN_LENGTH },
	{ "substr", TOKEN_BUILTIN, BUILTIN_SUBSTR },
	{ "index", TOKEN_BUILTIN, BUILTIN_INDEX },
	{ "split", TOKEN_BUILTIN, BUILTIN_SPLIT },
	{ "sub", TOKEN_BUILTIN, BUILTIN_SUB },
	{ "gsub", TOKEN_BUILTIN, BUILTIN_GSUB },
	{ "match", TOKEN_BUILTIN, BUILTIN_MATCH },
	{ "sprintf", TOKEN_BUILTIN, BUILTIN_SPRINTF },
	{ "sin", TOKEN_BUILTIN, BUILTIN_SIN },
	{ "cos", TOKEN_BUILTIN, BUILTIN_COS },
	{ "atan2", TOKEN_BUILTIN, BUILTIN_ATAN2 },
	{ "exp", TOKEN_BUILTIN, BUILTIN_EXP },
	{ "log", TOKEN_BUILTIN, BUILTIN_LOG },
	{ "sqrt", TOKEN_BUILTIN, BUILTIN_SQRT },
	{ "int", TOKEN_BUILTIN, BUILTIN_INT },
	{ "rand", TOKEN_BUILTIN, BUILTIN_RAND },
	{ "srand", TOKEN_BUILTIN, BUILTIN_SRAND },
	{ "tolower", TOKEN_BUILTIN, BUILTIN_TOLOWER },
	{ "toupper", TOKEN_BUILTIN, BUILTIN_TOUPPER },
	{ "system", TOKEN_BUILTIN, BUILTIN_SYSTEM },
	{ "close", TOKEN_BUILTIN, BUILTIN_CLOSE },
	{ "fflush", TOKEN_BUILTIN, BUILTIN_FFLUSH },
	{ NULL, 0, 0 }
};

/*
 * The program text being read, where the lexer is in it, and the line it
 * is on.  They are set by lex_start and live while the program is parsed.
 */
static const char *lex_source;
static size_t lex_length;
static size_t lex_position;
static int lex_current_line;

static int peek_character(size_t offset);
static void skip_blanks(void);
static void read_number(struct token *token);
static void read_name(struct token *token);
static void read_string(struct token *token);
static int read_escape(struct buffer *buffer);
static int read_operator(struct token *token);
static int is_letter(int character);
static int is_digit(int character);

/*
 * Starts reading a program.
 */
void
lex_start(
	const char *source,
	size_t length)
{
	/* The text, from its first line. */
	lex_source = source;
	lex_length = length;
	lex_position = 0;
	lex_current_line = 1;
}

/*
 * Returns the line the lexer is on, for messages.
 */
int
lex_line(
	void)
{
	/* The line counted so far. */
	return lex_current_line;
}

/*
 * Reads the next token.
 */
void
lex_next(
	struct token *token)
{
	int character;
	int known;
	int digit;
	int letter;

	/* What does not make a token. */
	skip_blanks();
	memset(token, 0, sizeof(*token));
	token->line = lex_current_line;
	token->start = lex_position;

	/* The end of the program. */
	character = peek_character(0);
	if (character < 0) {
		token->kind = TOKEN_EOF;
		return;
	}

	/* A newline ends a statement. */
	if (character == '\n') {
		lex_position++;
		lex_current_line++;
		token->kind = TOKEN_NEWLINE;
		return;
	}

	/* A number: digits, or a point and digits. */
	digit = is_digit(character);
	if (digit) {
		read_number(token);
		return;
	}

	/* A point starts a number only when a digit follows. */
	if (character == '.') {
		character = peek_character(1);
		digit = is_digit(character);
		if (digit) {
			read_number(token);
			return;
		}

		/* A point alone is not a token. */
		awk_fatal("syntax error at source line %d: unexpected .", lex_current_line);
	}

	/* A name, a keyword or a built-in function. */
	letter = is_letter(character);
	if (letter) {
		read_name(token);
		return;
	}

	/* A string. */
	if (character == '"') {
		read_string(token);
		return;
	}

	/* Succeeded: an operator or a punctuation mark. */
	known = read_operator(token);
	if (!known) {
		awk_fatal("syntax error at source line %d: unexpected character '%c'",
			  lex_current_line,
			  character);
	}
}

/*
 * Reads a regex from just after the / that starts it, and ends the token
 * after the / that ends it.  The awk escapes become the characters they
 * stand for, and \/ becomes /; a backslash inside a bracket expression is
 * taken as a character.
 */
void
lex_regex(
	struct token *token,
	size_t start)
{
	struct buffer buffer;
	int character;
	int in_bracket;
	int escaped;
	int inner;
	int closing;

	/* The token starts where the parser says. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	lex_position = start;
	memset(token, 0, sizeof(*token));
	token->kind = TOKEN_REGEX;
	token->line = lex_current_line;
	token->start = start;

	/* Every character up to the / outside a bracket expression. */
	in_bracket = 0;
	for (;;) {
		character = peek_character(0);
		if (character < 0 || character == '\n') {
			awk_fatal("syntax error at source line %d: unterminated regular expression",
				  lex_current_line);
		}

		/* The / that ends the regex. */
		if (character == '/' && !in_bracket) {
			lex_position++;
			break;
		}

		/* An escape: \/ is /, and the awk escapes are characters. */
		if (character == '\\') {
			character = peek_character(1);
			if (character == '/') {
				buffer_append_byte(&buffer, '/');
				lex_position += 2;
				continue;
			}

			/* A backslash in a bracket expression is itself. */
			if (character == '\\' && in_bracket) {
				buffer_append_byte(&buffer, '\\');
				lex_position += 2;
				continue;
			}

			/* An escaped backslash stays escaped for the ERE. */
			if (character == '\\') {
				buffer_append(&buffer, "\\\\", 2);
				lex_position += 2;
				continue;
			}

			/* The escapes of awk strings. */
			escaped = read_escape(&buffer);
			if (escaped)
				continue;

			/* Any other escape is the ERE's own. */
			buffer_append_byte(&buffer, '\\');
			lex_position++;
			character = peek_character(0);
			if (character < 0 || character == '\n')
				continue;
			buffer_append_byte(&buffer, (char)character);
			lex_position++;
			continue;
		}

		/* A bracket expression, whose first ] is a character. */
		if (character == '[' && !in_bracket) {
			in_bracket = 1;
			buffer_append_byte(&buffer, '[');
			lex_position++;
			character = peek_character(0);
			if (character == '^') {
				buffer_append_byte(&buffer, '^');
				lex_position++;
				character = peek_character(0);
			}

			/* A ] first in the brackets is a character. */
			if (character == ']') {
				buffer_append_byte(&buffer, ']');
				lex_position++;
			}

			continue;
		}

		/* A class, an equivalence class or a collating symbol in one. */
		if (character == '[' && in_bracket) {
			character = peek_character(1);
			if (character == ':' || character == '.' || character == '=') {
				buffer_append_byte(&buffer, '[');
				buffer_append_byte(&buffer, (char)character);
				lex_position += 2;
				for (;;) {
					inner = peek_character(0);
					if (inner < 0 || inner == '\n')
						break;
					buffer_append_byte(&buffer, (char)inner);
					lex_position++;

					/* The same mark and a ] close it. */
					closing = peek_character(0);
					if (inner == character && closing == ']') {
						buffer_append_byte(&buffer, ']');
						lex_position++;
						break;
					}
				}

				continue;
			}
		}

		/* The end of a bracket expression. */
		if (character == ']' && in_bracket)
			in_bracket = 0;

		/* A character as it is. */
		buffer_append_byte(&buffer, (char)character);
		lex_position++;
	}

	/* Succeeded: the regex takes the buffer. */
	token->text = buffer.data;
	token->length = buffer.length;
}

/* Returns the character at an offset from the position, or -1 past the end. */
static int
peek_character(
	size_t offset)
{
	/* Past the end of the text. */
	if (lex_position + offset >= lex_length)
		return -1;

	/* Succeeded. */
	return (unsigned char)lex_source[lex_position + offset];
}

/* Skips blanks, comments, and a backslash that continues a line. */
static void
skip_blanks(
	void)
{
	int character;
	int following;
	int after;

	/* Until something that makes a token. */
	for (;;) {
		character = peek_character(0);

		/* Blanks. */
		if (character == ' ' || character == '\t' || character == '\r') {
			lex_position++;
			continue;
		}

		/* A backslash and a newline join two lines. */
		if (character == '\\') {
			following = peek_character(1);
			if (following == '\n') {
				lex_position += 2;
				lex_current_line++;
				continue;
			}

			/* A backslash, a carriage return and a newline join them too. */
			after = peek_character(2);
			if (following == '\r' && after == '\n') {
				lex_position += 3;
				lex_current_line++;
				continue;
			}
		}

		/* A comment, up to the newline, which still ends the statement. */
		if (character == '#') {
			for (;;) {
				character = peek_character(0);
				if (character < 0 || character == '\n')
					break;
				lex_position++;
			}

			continue;
		}

		/* Something else. */
		break;
	}
}

/* Reads a decimal number: digits, a point and digits, an exponent. */
static void
read_number(
	struct token *token)
{
	size_t start;
	int character;
	int sign;
	int following;
	int digit;
	char *text;

	/* The digits before and after the point. */
	start = lex_position;
	for (;;) {
		character = peek_character(0);
		digit = is_digit(character);
		if (!digit)
			break;
		lex_position++;
	}

	/* The fraction after a point. */
	character = peek_character(0);
	if (character == '.') {
		lex_position++;
		for (;;) {
			character = peek_character(0);
			digit = is_digit(character);
			if (!digit)
				break;
			lex_position++;
		}
	}

	/* An exponent, only when digits follow the e and its sign. */
	character = peek_character(0);
	if (character == 'e' || character == 'E') {
		sign = peek_character(1);
		following = sign;
		if (sign == '+' || sign == '-')
			following = peek_character(2);
		digit = is_digit(following);
		if (digit) {
			lex_position += 2;
			if (sign == '+' || sign == '-')
				lex_position++;
			for (;;) {
				character = peek_character(0);
				digit = is_digit(character);
				if (!digit)
					break;
				lex_position++;
			}
		}
	}

	/* Succeeded: the value of the digits. */
	text = awk_copy(lex_source + start, lex_position - start);
	token->kind = TOKEN_NUMBER;
	token->number = strtod(text, NULL);
	free(text);
}

/* Reads a name, and finds whether it is a keyword or a built-in function. */
static void
read_name(
	struct token *token)
{
	const struct keyword *keyword;
	size_t start;
	size_t length;
	int character;
	int letter;
	int digit;
	int compare;

	/* Letters, digits and underscores. */
	start = lex_position;
	for (;;) {
		character = peek_character(0);
		letter = is_letter(character);
		digit = is_digit(character);
		if (!letter && !digit)
			break;
		lex_position++;
	}

	/* The text of the name. */
	length = lex_position - start;
	token->text = awk_copy(lex_source + start, length);
	token->length = length;

	/* A keyword or a built-in function. */
	for (keyword = keywords; keyword->name != NULL; keyword++) {
		compare = strcmp(keyword->name, token->text);
		if (compare != 0)
			continue;
		token->kind = keyword->token;
		token->builtin = keyword->builtin;
		return;
	}

	/* A function name is followed by ( at once. */
	character = peek_character(0);
	if (character == '(') {
		token->kind = TOKEN_FUNCTION_NAME;
		return;
	}

	/* Succeeded: a variable's name. */
	token->kind = TOKEN_NAME;
}

/* Reads a string constant with its escapes. */
static void
read_string(
	struct token *token)
{
	struct buffer buffer;
	int character;
	int escaped;

	/* The characters after the opening quote. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_append(&buffer, "", 0);
	lex_position++;
	for (;;) {
		character = peek_character(0);
		if (character < 0 || character == '\n') {
			awk_fatal("syntax error at source line %d: unterminated string",
				  lex_current_line);
		}

		/* The closing quote. */
		if (character == '"') {
			lex_position++;
			break;
		}

		/* An escape; an unknown one is the character after it. */
		if (character == '\\') {
			character = peek_character(1);
			if (character == '\n') {
				lex_position += 2;
				lex_current_line++;
				continue;
			}

			/* The escapes of one letter and the octal ones. */
			escaped = read_escape(&buffer);
			if (escaped)
				continue;
			lex_position++;
			if (character < 0)
				continue;
			buffer_append_byte(&buffer, (char)character);
			lex_position++;
			continue;
		}

		/* A character as it is. */
		buffer_append_byte(&buffer, (char)character);
		lex_position++;
	}

	/* Succeeded: the string takes the buffer. */
	token->kind = TOKEN_STRING;
	token->text = buffer.data;
	token->length = buffer.length;
}

/*
 * Reads an awk escape at the position (a backslash and what follows), and
 * appends the character it stands for.  Returns 0, reading nothing, when
 * the escape is not one of them.
 */
static int
read_escape(
	struct buffer *buffer)
{
	int character;
	int value;
	int count;

	/* The character after the backslash. */
	character = peek_character(1);

	/* An octal escape of up to three digits. */
	if (character >= '0' && character <= '7') {
		value = 0;
		lex_position++;
		for (count = 0; count < 3; count++) {
			character = peek_character(0);
			if (character < '0' || character > '7')
				break;
			value = value * 8 + (character - '0');
			lex_position++;
		}

		/* The character of the digits. */
		buffer_append_byte(buffer, (char)value);
		return 1;
	}

	/* The escapes of one letter. */
	switch (character) {
	case '"':
		value = '"';
		break;
	case '\\':
		value = '\\';
		break;
	case 'a':
		value = '\a';
		break;
	case 'b':
		value = '\b';
		break;
	case 'f':
		value = '\f';
		break;
	case 'n':
		value = '\n';
		break;
	case 'r':
		value = '\r';
		break;
	case 't':
		value = '\t';
		break;
	case 'v':
		value = '\v';
		break;
	case '/':
		value = '/';
		break;
	default:
		return 0;
	}

	/* Succeeded: the character, past the escape. */
	buffer_append_byte(buffer, (char)value);
	lex_position += 2;
	return 1;
}

/* Reads an operator or a punctuation mark; returns 0 for anything else. */
static int
read_operator(
	struct token *token)
{
	int character;
	int following;
	int kind;
	size_t length;

	/* The longest operator that starts here. */
	character = peek_character(0);
	following = peek_character(1);
	length = 1;

	/* The operator and how many characters it takes. */
	switch (character) {
	case '{':
		kind = TOKEN_LEFT_BRACE;
		break;
	case '}':
		kind = TOKEN_RIGHT_BRACE;
		break;
	case '(':
		kind = TOKEN_LEFT_PAREN;
		break;
	case ')':
		kind = TOKEN_RIGHT_PAREN;
		break;
	case '[':
		kind = TOKEN_LEFT_BRACKET;
		break;
	case ']':
		kind = TOKEN_RIGHT_BRACKET;
		break;
	case ';':
		kind = TOKEN_SEMICOLON;
		break;
	case ',':
		kind = TOKEN_COMMA;
		break;
	case '?':
		kind = TOKEN_QUESTION;
		break;
	case ':':
		kind = TOKEN_COLON;
		break;
	case '~':
		kind = TOKEN_TILDE;
		break;
	case '$':
		kind = TOKEN_DOLLAR;
		break;
	case '+':
		kind = TOKEN_PLUS;
		if (following == '+') {
			kind = TOKEN_INCREMENT;
			length = 2;
		} else if (following == '=') {
			kind = TOKEN_ADD_ASSIGN;
			length = 2;
		}

		break;
	case '-':
		kind = TOKEN_MINUS;
		if (following == '-') {
			kind = TOKEN_DECREMENT;
			length = 2;
		} else if (following == '=') {
			kind = TOKEN_SUBTRACT_ASSIGN;
			length = 2;
		}

		break;
	case '*':
		kind = TOKEN_STAR;
		if (following == '=') {
			kind = TOKEN_MULTIPLY_ASSIGN;
			length = 2;
		}

		break;
	case '/':
		kind = TOKEN_SLASH;
		if (following == '=') {
			kind = TOKEN_DIVIDE_ASSIGN;
			length = 2;
		}

		break;
	case '%':
		kind = TOKEN_PERCENT;
		if (following == '=') {
			kind = TOKEN_MODULO_ASSIGN;
			length = 2;
		}

		break;
	case '^':
		kind = TOKEN_CARET;
		if (following == '=') {
			kind = TOKEN_POWER_ASSIGN;
			length = 2;
		}

		break;
	case '!':
		kind = TOKEN_NOT;
		if (following == '=') {
			kind = TOKEN_NOT_EQUAL;
			length = 2;
		} else if (following == '~') {
			kind = TOKEN_NOT_TILDE;
			length = 2;
		}

		break;
	case '>':
		kind = TOKEN_GREATER;
		if (following == '=') {
			kind = TOKEN_GREATER_EQUAL;
			length = 2;
		} else if (following == '>') {
			kind = TOKEN_APPEND;
			length = 2;
		}

		break;
	case '<':
		kind = TOKEN_LESS;
		if (following == '=') {
			kind = TOKEN_LESS_EQUAL;
			length = 2;
		}

		break;
	case '|':
		kind = TOKEN_PIPE;
		if (following == '|') {
			kind = TOKEN_OR;
			length = 2;
		}

		break;
	case '&':
		if (following != '&')
			return 0;
		kind = TOKEN_AND;
		length = 2;
		break;
	case '=':
		kind = TOKEN_ASSIGN;
		if (following == '=') {
			kind = TOKEN_EQUAL;
			length = 2;
		}

		break;
	default:
		return 0;
	}

	/* Succeeded: past the operator. */
	token->kind = kind;
	lex_position += length;
	return 1;
}

/* Returns whether a character starts a name. */
static int
is_letter(
	int character)
{
	/* ASCII letters and the underscore. */
	if (character >= 'a' && character <= 'z')
		return 1;
	if (character >= 'A' && character <= 'Z')
		return 1;
	if (character == '_')
		return 1;
	return 0;
}

/* Returns whether a character is a decimal digit. */
static int
is_digit(
	int character)
{
	/* 0 to 9. */
	if (character >= '0' && character <= '9')
		return 1;
	return 0;
}
