/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type 1 font programs of libpdf's reader (stage 3 of design-pdf.md):
 * an embedded /FontFile read as Adobe Type 1 Font Format describes it.
 *
 * The clear text gives the font matrix and the program's own encoding.
 * The eexec part (binary or hexadecimal) is decrypted with key 55665 into
 * a copy the program owns, and its private dictionary gives lenIV, the
 * subroutines and the charstrings, each decrypted in place with key 4330.
 * A glyph's charstring runs through the Type 1 operators into the path,
 * with the flex and hint-replacement othersubrs of the standard Subrs 0
 * to 3, and seac for accented characters; the hints themselves are not
 * used.
 *
 * The program is not trusted: counts, the stack, the call depth and the
 * number of operators are bounded, and a damaged charstring ends where the
 * damage is.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "charstrings.h"

/* The keys of eexec and of the charstrings, and the constants both use. */
#define TYPE1_EEXEC_KEY 55665U
#define TYPE1_CHARSTRING_KEY 4330U
#define TYPE1_C1 52845U
#define TYPE1_C2 22719U

/* The longest token the private dictionary's reader keeps. */
#define TYPE1_TOKEN_MAX 128

/* The points one flex gathers: its reference point and the six of its two curves. */
#define TYPE1_FLEX_POINTS 7

/* The deepest seac: an accented character is drawn from two others that are not accented. */
#define TYPE1_SEAC_DEPTH 1

/*
 * The kinds of token of a program's text.
 */
enum type1_token_type {
	TYPE1_TOKEN_END = 0,
	TYPE1_TOKEN_NAME,
	TYPE1_TOKEN_NUMBER,
	TYPE1_TOKEN_WORD,
	TYPE1_TOKEN_OTHER
};

/*
 * One token of a program's text: a literal name (/Subrs, without the
 * slash), a number, an executable word (RD, dup), or anything else.
 */
struct type1_token {
	enum type1_token_type type;
	const unsigned char *bytes;
	size_t length;
	long number;
};

/*
 * A glyph's charstring while it runs: the argument stack, the stack
 * othersubrs return values on (for pop), the pen in glyph space, the flex
 * in progress, and the operators run so far.
 */
struct type1_run {
	struct pdf_charstrings *font;
	struct charstrings_path *path;
	double stack[CHARSTRINGS_STACK_MAX];
	int count;
	double results[CHARSTRINGS_STACK_MAX];
	int results_count;
	double x;
	double y;
	double offset_x;
	double offset_y;
	double width;
	int in_flex;
	double flex[TYPE1_FLEX_POINTS][2];
	int flex_count;
	long operators;
	int ended;
};

static int read_token(const unsigned char *text, size_t size, size_t *position, struct type1_token *token);
static void skip_blank(const unsigned char *text, size_t size, size_t *position);
static void skip_string(const unsigned char *text, size_t size, size_t *position);
static void read_number_token(struct type1_token *token);
static int is_blank(unsigned char byte);
static int is_delimiter(unsigned char byte);
static int token_is(const struct type1_token *token, enum type1_token_type type, const char *text);
static size_t find_eexec(const unsigned char *data, size_t size, size_t clear_length);
static void read_matrix(const unsigned char *text, size_t size, double matrix[6]);
static int read_real(const unsigned char *bytes, size_t length, double *value);
static int decrypt_eexec(const unsigned char *data, size_t size, unsigned char **plain, size_t *plain_size);
static void decrypt_charstring(unsigned char *data, size_t size);
static int read_private(struct pdf_charstrings *font, unsigned char *text, size_t size);
static int add_charstring(struct pdf_charstrings *font, size_t *capacity, const struct type1_token *name, unsigned char *data, size_t size);
static void map_builtin(struct pdf_charstrings *font, const unsigned char *const names[256], const size_t lengths[256], int standard);
static void run_glyph(struct type1_run *run, unsigned glyph, int seac_depth);
static void run_charstring(struct type1_run *run, const struct charstrings_range *range, int depth, int seac_depth);
static void run_escape(struct type1_run *run, int code, int seac_depth);
static void run_othersubr(struct type1_run *run);
static void end_flex(struct type1_run *run);
static void run_seac(struct type1_run *run, int seac_depth);
static void pen_move(struct type1_run *run, double dx, double dy);
static void pen_line(struct type1_run *run, double dx, double dy);
static void pen_curve(struct type1_run *run, double dx1, double dy1, double dx2, double dy2, double dx3, double dy3);
static int push(struct type1_run *run, double value);

/*
 * Reads a Type 1 font program (/FontFile): the clear text of clear_length
 * bytes (/Length1; 0 when unknown, then the eexec keyword ends it) and the
 * eexec part after it.
 *
 * The program keeps no pointer into data.
 */
int
pdf_type1_open(
	const unsigned char *data,
	size_t size,
	size_t clear_length,
	struct pdf_charstrings **font)
{
	struct pdf_charstrings *created;
	const unsigned char *names[256];
	size_t lengths[256];
	unsigned char *plain;
	size_t plain_size;
	size_t encrypted;
	int standard;
	int has_encoding;
	int error;

	/* Allocates the program with the default matrix (a thousandth of an em a unit) and lenIV. */
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->kind = CHARSTRINGS_TYPE1;
	created->matrix[0] = 0.001;
	created->matrix[3] = 0.001;
	created->len_iv = 4;

	/* Finds where the clear text ends and the encrypted part starts. */
	encrypted = find_eexec(data, size, clear_length);
	if (encrypted == 0) {
		pdf_charstrings_close(created);
		return PDF_EFORMAT;
	}

	/* Reads the clear text's matrix and encoding (the names are found once the glyphs are read). */
	read_matrix(data, encrypted, created->matrix);
	has_encoding = 0;
	error = pdf_type1_encoding(data, encrypted, names, lengths, &standard);
	if (error == 0)
		has_encoding = 1;

	/* Decrypts the rest into the program's own copy. */
	error = decrypt_eexec(data + encrypted, size - encrypted, &plain, &plain_size);
	if (error != 0) {
		pdf_charstrings_close(created);
		return error;
	}

	/* The program owns the plain text its names and charstrings point into. */
	created->owned = plain;

	/* Reads the private dictionary: lenIV, the subroutines and the charstrings. */
	error = read_private(created, plain, plain_size);
	if (error != 0) {
		pdf_charstrings_close(created);
		return error;
	}

	/* Refuses a program without any charstring. */
	if (created->glyphs == 0) {
		pdf_charstrings_close(created);
		return PDF_EFORMAT;
	}

	/* Sorts the glyph names for the lookups. */
	error = charstrings_sort_names(created);
	if (error != 0) {
		pdf_charstrings_close(created);
		return error;
	}

	/* The program's own encoding, by its glyphs' names. */
	if (has_encoding)
		map_builtin(created, names, lengths, standard);

	/* Succeeded: the caller owns the program. */
	*font = created;
	return 0;
}

/*
 * Reads the encoding a Type 1 program's clear text defines:
 * *standard when it is StandardEncoding, else the names of its
 * "dup code /name put" entries (NULL for a code it leaves out).  Reports
 * ENOENT when the text has no /Encoding.
 */
int
pdf_type1_encoding(
	const unsigned char *data,
	size_t size,
	const unsigned char *names[256],
	size_t lengths[256],
	int *standard)
{
	struct type1_token token;
	struct type1_token code;
	struct type1_token name;
	size_t position;
	int found;
	int error;

	/* Nothing is known yet. */
	memset(names, 0, 256 * sizeof(names[0]));
	memset(lengths, 0, 256 * sizeof(lengths[0]));
	*standard = 0;

	/* Finds the literal name /Encoding. */
	position = 0;
	found = 0;
	for (;;) {
		error = read_token(data, size, &position, &token);
		if (error != 0)
			return ENOENT;
		if (token.type == TYPE1_TOKEN_END)
			return ENOENT;
		found = token_is(&token, TYPE1_TOKEN_NAME, "Encoding");
		if (found)
			break;
	}

	/* StandardEncoding is named. */
	error = read_token(data, size, &position, &token);
	if (error != 0)
		return ENOENT;
	found = token_is(&token, TYPE1_TOKEN_WORD, "StandardEncoding");
	if (found) {
		*standard = 1;
		return 0;
	}

	/* Otherwise each "dup code /name put" up to the def that ends the array. */
	for (;;) {
		error = read_token(data, size, &position, &token);
		if (error != 0)
			break;
		if (token.type == TYPE1_TOKEN_END)
			break;
		found = token_is(&token, TYPE1_TOKEN_WORD, "def");
		if (found)
			break;
		found = token_is(&token, TYPE1_TOKEN_WORD, "dup");
		if (!found)
			continue;

		/* The code and the name. */
		error = read_token(data, size, &position, &code);
		if (error != 0)
			break;
		error = read_token(data, size, &position, &name);
		if (error != 0)
			break;
		if (code.type != TYPE1_TOKEN_NUMBER || name.type != TYPE1_TOKEN_NAME)
			continue;
		if (code.number < 0 || code.number > 255)
			continue;
		names[code.number] = name.bytes;
		lengths[code.number] = name.length;
	}

	/* Succeeded: the entries read. */
	return 0;
}

/*
 * Runs a glyph's charstring into the path; *width is its advance in
 * glyph space.
 */
void
pdf_type1_run(
	struct pdf_charstrings *font,
	unsigned glyph,
	struct charstrings_path *path,
	double *width)
{
	struct type1_run run;

	/* Starts with an empty stack at the origin. */
	memset(&run, 0, sizeof(run));
	run.font = font;
	run.path = path;

	/* Runs the glyph's charstring. */
	run_glyph(&run, glyph, 0);

	/* The width hsbw or sbw set. */
	*width = run.width;
}

/*
 * Reads the next token of a program's text: comments and white space are
 * skipped; a string, a procedure's braces or a bracket is one OTHER token.
 * Reports ENOTSUP for a token longer than the reader keeps.
 */
static int
read_token(
	const unsigned char *text,
	size_t size,
	size_t *position,
	struct type1_token *token)
{
	size_t start;
	int delimiter;

	/* Skips white space and comments. */
	memset(token, 0, sizeof(*token));
	skip_blank(text, size, position);

	/* The end of the text. */
	if (*position >= size) {
		token->type = TYPE1_TOKEN_END;
		return 0;
	}

	/* A string is skipped whole. */
	if (text[*position] == '(') {
		skip_string(text, size, position);
		token->type = TYPE1_TOKEN_OTHER;
		return 0;
	}

	/* Another delimiter (but the slash of a name) is a token of its own. */
	start = *position;
	delimiter = is_delimiter(text[start]);
	if (delimiter && text[start] != '/') {
		token->type = TYPE1_TOKEN_OTHER;
		token->bytes = text + start;
		token->length = 1;
		(*position)++;
		return 0;
	}

	/* A literal name after its slash, or a word. */
	token->type = TYPE1_TOKEN_WORD;
	if (text[start] == '/') {
		token->type = TYPE1_TOKEN_NAME;
		(*position)++;
	}

	/* Its bytes run to white space or a delimiter. */
	start = *position;
	while (*position < size) {
		if (text[*position] <= ' ')
			break;
		delimiter = is_delimiter(text[*position]);
		if (delimiter)
			break;
		(*position)++;
	}

	/* Refuses a token longer than any a font program needs. */
	token->bytes = text + start;
	token->length = *position - start;
	if (token->length > TYPE1_TOKEN_MAX)
		return ENOTSUP;

	/* A word of digits is a number. */
	if (token->type == TYPE1_TOKEN_WORD)
		read_number_token(token);

	/* Succeeded: the token. */
	return 0;
}

/* Skips white space and comments (to the end of their line). */
static void
skip_blank(
	const unsigned char *text,
	size_t size,
	size_t *position)
{
	int blank;

	/* Each blank byte or comment. */
	while (*position < size) {
		/* A comment runs to its line's end. */
		if (text[*position] == '%') {
			while (*position < size) {
				if (text[*position] == '\n' || text[*position] == '\r')
					break;
				(*position)++;
			}

			continue;
		}

		/* White space is skipped; anything else starts the token. */
		blank = is_blank(text[*position]);
		if (!blank)
			break;
		(*position)++;
	}
}

/* Skips a string, whose parentheses nest and whose backslash escapes the next byte. */
static void
skip_string(
	const unsigned char *text,
	size_t size,
	size_t *position)
{
	int nesting;

	/* Each byte up to the parenthesis that closes the first. */
	nesting = 0;
	while (*position < size) {
		/* An escaped byte is skipped with its backslash. */
		if (text[*position] == '\\') {
			*position += 2;
			continue;
		}

		/* Parentheses nest. */
		if (text[*position] == '(')
			nesting++;
		if (text[*position] == ')')
			nesting--;
		(*position)++;

		/* The first parenthesis is closed. */
		if (nesting == 0)
			break;
	}

	/* An escape at the end may have stepped past it. */
	if (*position > size)
		*position = size;
}

/* Makes a word of digits, with an optional minus sign, a number token. */
static void
read_number_token(
	struct type1_token *token)
{
	size_t index;
	long value;
	int negative;

	/* An optional sign. */
	index = 0;
	negative = 0;
	if (token->length > 0 && token->bytes[0] == '-') {
		negative = 1;
		index = 1;
	}

	/* A word without digits is not a number. */
	if (index == token->length)
		return;

	/* The digits (a value past the reader's range stops growing; no count in a font is that large). */
	value = 0;
	for (; index < token->length; index++) {
		if (token->bytes[index] < '0' || token->bytes[index] > '9')
			return;
		if (value < 100000000L)
			value = value * 10 + (token->bytes[index] - '0');
	}

	/* The word is the number. */
	token->type = TYPE1_TOKEN_NUMBER;
	token->number = value;
	if (negative)
		token->number = -value;
}

/* Tells whether a byte is PostScript's white space (or another control byte, which a token cannot hold). */
static int
is_blank(
	unsigned char byte)
{
	/* Every byte up to the space. */
	if (byte <= ' ')
		return 1;

	/* Anything else. */
	return 0;
}

/* Tells whether a byte is one of PostScript's delimiters. */
static int
is_delimiter(
	unsigned char byte)
{
	/* The delimiters, each a token of its own. */
	switch (byte) {
	case '(':
	case ')':
	case '<':
	case '>':
	case '[':
	case ']':
	case '{':
	case '}':
	case '/':
	case '%':
		return 1;
	default:
		break;
	}

	/* Anything else is part of a token. */
	return 0;
}

/* Tells whether a token is of a type and has the given bytes. */
static int
token_is(
	const struct type1_token *token,
	enum type1_token_type type,
	const char *text)
{
	size_t length;
	int differs;

	/* Another type or length is another token. */
	if (token->type != type)
		return 0;
	length = strlen(text);
	if (token->length != length)
		return 0;

	/* The same bytes. */
	differs = memcmp(token->bytes, text, length);
	if (differs != 0)
		return 0;

	/* The token is the text. */
	return 1;
}

/*
 * Finds where the encrypted part starts: after /Length1's bytes when they
 * end at the eexec keyword's line, else after the keyword and its white
 * space.  Reports 0 when the program has no eexec.
 */
static size_t
find_eexec(
	const unsigned char *data,
	size_t size,
	size_t clear_length)
{
	size_t position;
	int differs;
	int blank;

	/* Looks for the keyword, within /Length1 when it is known. */
	for (position = 0; position + 5 <= size; position++) {
		differs = memcmp(data + position, "eexec", 5);
		if (differs == 0)
			break;
	}

	/* A program without the keyword has no encrypted part. */
	if (position + 5 > size)
		return 0;

	/* /Length1 is trusted when it is past the keyword and within the program. */
	if (clear_length >= position + 5 && clear_length < size)
		return clear_length;

	/* Else the part starts after the keyword's white space. */
	position += 5;
	while (position < size) {
		blank = is_blank(data[position]);
		if (!blank)
			break;
		position++;
	}

	/* Nothing after the keyword is no encrypted part. */
	if (position >= size)
		return 0;

	/* The encrypted part's start. */
	return position;
}

/* Reads /FontMatrix [a b c d e f] from the clear text; the default stays when it cannot be read. */
static void
read_matrix(
	const unsigned char *text,
	size_t size,
	double matrix[6])
{
	struct type1_token token;
	double values[6];
	size_t position;
	int found;
	int index;
	int error;

	/* Finds the name. */
	position = 0;
	for (;;) {
		error = read_token(text, size, &position, &token);
		if (error != 0)
			return;
		if (token.type == TYPE1_TOKEN_END)
			return;
		found = token_is(&token, TYPE1_TOKEN_NAME, "FontMatrix");
		if (found)
			break;
	}

	/* The bracket, then six numbers. */
	error = read_token(text, size, &position, &token);
	if (error != 0)
		return;
	if (token.type != TYPE1_TOKEN_OTHER)
		return;
	for (index = 0; index < 6; index++) {
		error = read_token(text, size, &position, &token);
		if (error != 0)
			return;
		if (token.type != TYPE1_TOKEN_WORD && token.type != TYPE1_TOKEN_NUMBER)
			return;
		error = read_real(token.bytes, token.length, &values[index]);
		if (error != 0)
			return;
	}

	/* A matrix that squashes glyphs flat, or scales them past an em a unit, is not used. */
	if (!(values[0] > 1e-6 || values[0] < -1e-6))
		return;
	if (!(values[3] > 1e-6 || values[3] < -1e-6))
		return;
	if (!(values[0] < 1.0 && values[0] > -1.0))
		return;

	/* Succeeded: the program's matrix. */
	memcpy(matrix, values, sizeof(values));
}

/* Reads a decimal number with an optional fraction and exponent. */
static int
read_real(
	const unsigned char *bytes,
	size_t length,
	double *value)
{
	char text[TYPE1_TOKEN_MAX + 1];
	char *end;

	/* Copies the token so that strtod sees its end. */
	if (length == 0 || length > TYPE1_TOKEN_MAX)
		return PDF_EFORMAT;
	memcpy(text, bytes, length);
	text[length] = '\0';

	/* Converts it; anything left over is not a number. */
	*value = strtod(text, &end);
	if (*end != '\0')
		return PDF_EFORMAT;

	/* Succeeded: the number. */
	return 0;
}

/*
 * Decrypts the eexec part into a new buffer, its first four bytes (the
 * random ones) dropped: binary, or hexadecimal digits when the first four
 * bytes are all digits.
 */
static int
decrypt_eexec(
	const unsigned char *data,
	size_t size,
	unsigned char **plain,
	size_t *plain_size)
{
	unsigned char *buffer;
	unsigned char cipher;
	unsigned key;
	size_t produced;
	size_t index;
	int hex;
	int high;
	int digit;

	/* Hexadecimal text when the first four bytes are hexadecimal digits. */
	hex = 1;
	if (size < 4)
		return PDF_EFORMAT;
	for (index = 0; index < 4; index++) {
		digit = data[index];
		if (digit >= '0' && digit <= '9')
			continue;
		if (digit >= 'a' && digit <= 'f')
			continue;
		if (digit >= 'A' && digit <= 'F')
			continue;
		hex = 0;
	}

	/* Allocates for the most bytes the part can give. */
	buffer = malloc(size + 1);
	if (buffer == NULL)
		return ENOMEM;

	/* Decrypts each cipher byte (a pair of digits in hexadecimal text). */
	key = TYPE1_EEXEC_KEY;
	produced = 0;
	high = -1;
	for (index = 0; index < size; index++) {
		/* The cipher byte. */
		if (hex) {
			digit = -1;
			if (data[index] >= '0' && data[index] <= '9')
				digit = data[index] - '0';
			if (data[index] >= 'a' && data[index] <= 'f')
				digit = data[index] - 'a' + 10;
			if (data[index] >= 'A' && data[index] <= 'F')
				digit = data[index] - 'A' + 10;
			if (digit < 0)
				continue;
			if (high < 0) {
				high = digit;
				continue;
			}

			/* The second digit completes the byte. */
			cipher = (unsigned char)(high * 16 + digit);
			high = -1;
		} else {
			cipher = data[index];
		}

		/* The plain byte, and the key for the next. */
		buffer[produced] = (unsigned char)(cipher ^ (key >> 8));
		produced++;
		key = ((cipher + key) * TYPE1_C1 + TYPE1_C2) & 0xffffU;
	}

	/* Refuses a part shorter than its random bytes. */
	if (produced < 4) {
		free(buffer);
		return PDF_EFORMAT;
	}

	/* Succeeded: the plain text after the four random bytes. */
	memmove(buffer, buffer + 4, produced - 4);
	*plain = buffer;
	*plain_size = produced - 4;
	return 0;
}

/* Decrypts a charstring or subroutine in place with the charstring key. */
static void
decrypt_charstring(
	unsigned char *data,
	size_t size)
{
	unsigned char cipher;
	unsigned key;
	size_t index;

	/* Each byte depends on the cipher bytes before it. */
	key = TYPE1_CHARSTRING_KEY;
	for (index = 0; index < size; index++) {
		cipher = data[index];
		data[index] = (unsigned char)(cipher ^ (key >> 8));
		key = ((cipher + key) * TYPE1_C1 + TYPE1_C2) & 0xffffU;
	}
}

/*
 * Reads the decrypted private part: /lenIV, the /Subrs array ("dup index
 * length RD bytes NP") and the /CharStrings dictionary ("/name length RD
 * bytes ND").  The binary bytes follow the RD word (or -|) and one space.
 */
static int
read_private(
	struct pdf_charstrings *font,
	unsigned char *text,
	size_t size)
{
	struct charstrings_private *private;
	struct type1_token token;
	struct type1_token name;
	size_t position;
	size_t capacity;
	size_t start;
	size_t skip;
	long last_number;
	long previous_number;
	int section;
	int is_binary;
	int found;
	int error;

	/* The one private dictionary. */
	font->privates = calloc(1, sizeof(*font->privates));
	if (font->privates == NULL)
		return ENOMEM;
	font->privates_count = 1;
	private = &font->privates[0];
	memcpy(private->matrix, font->matrix, sizeof(private->matrix));

	/* Walks the tokens, keeping the last two numbers and the last name for the binary entries. */
	position = 0;
	capacity = 0;
	last_number = -1;
	previous_number = -1;
	section = 0;
	memset(&name, 0, sizeof(name));
	for (;;) {
		error = read_token(text, size, &position, &token);
		if (error != 0)
			break;
		if (token.type == TYPE1_TOKEN_END)
			break;

		/* lenIV: how many random bytes start each charstring (-1: not encrypted). */
		found = token_is(&token, TYPE1_TOKEN_NAME, "lenIV");
		if (found) {
			error = read_token(text, size, &position, &token);
			if (error == 0 && token.type == TYPE1_TOKEN_NUMBER) {
				if (token.number >= -1 && token.number <= 64)
					font->len_iv = (int)token.number;
			}

			continue;
		}

		/* The subroutines' array and its count. */
		found = token_is(&token, TYPE1_TOKEN_NAME, "Subrs");
		if (found) {
			error = read_token(text, size, &position, &token);
			if (error != 0)
				break;
			if (token.type != TYPE1_TOKEN_NUMBER)
				continue;
			if (token.number <= 0 || token.number > CHARSTRINGS_SUBRS_MAX)
				continue;
			if (private->subrs != NULL)
				continue;
			private->subrs = calloc((size_t)token.number, sizeof(*private->subrs));
			if (private->subrs == NULL)
				return ENOMEM;
			private->subrs_count = (size_t)token.number;
			section = 1;
			continue;
		}

		/* The charstrings' dictionary. */
		found = token_is(&token, TYPE1_TOKEN_NAME, "CharStrings");
		if (found) {
			section = 2;
			continue;
		}

		/* A number, kept with the one before it. */
		if (token.type == TYPE1_TOKEN_NUMBER) {
			previous_number = last_number;
			last_number = token.number;
			continue;
		}

		/* A name, kept for the charstring it may name. */
		if (token.type == TYPE1_TOKEN_NAME) {
			name = token;
			continue;
		}

		/* RD or -| reads the binary bytes the last number counts, after one space. */
		is_binary = token_is(&token, TYPE1_TOKEN_WORD, "RD");
		if (!is_binary)
			is_binary = token_is(&token, TYPE1_TOKEN_WORD, "-|");
		if (!is_binary)
			continue;
		start = position + 1;
		if (last_number < 0 || start > size)
			break;
		if ((size_t)last_number > size - start)
			break;
		position = start + (size_t)last_number;

		/* Decrypts it and drops its random bytes. */
		decrypt_charstring(text + start, (size_t)last_number);
		skip = 0;
		if (font->len_iv > 0)
			skip = (size_t)font->len_iv;
		if (skip > (size_t)last_number)
			skip = (size_t)last_number;

		/* A subroutine by its index, or a charstring by its name. */
		if (section == 1) {
			if (previous_number >= 0 && (size_t)previous_number < private->subrs_count) {
				private->subrs[previous_number].data = text + start + skip;
				private->subrs[previous_number].size = (size_t)last_number - skip;
			}
		} else if (section == 2) {
			error = add_charstring(font, &capacity, &name, text + start + skip, (size_t)last_number - skip);
			if (error != 0)
				return error;
		}

		/* The numbers were the entry's own. */
		last_number = -1;
		previous_number = -1;
	}

	/* Succeeded: what the part holds is read. */
	return 0;
}

/* Adds a charstring and its glyph name, growing the arrays. */
static int
add_charstring(
	struct pdf_charstrings *font,
	size_t *capacity,
	const struct type1_token *name,
	unsigned char *data,
	size_t size)
{
	struct charstrings_range *charstrings;
	struct charstrings_range *names;
	size_t grown;

	/* Refuses more glyphs than a program may have, or a charstring without a name. */
	if (font->glyphs >= CHARSTRINGS_GLYPHS_MAX)
		return 0;
	if (name->type != TYPE1_TOKEN_NAME)
		return 0;

	/* Grows both arrays together. */
	if (font->glyphs == *capacity) {
		grown = *capacity * 2 + 64;
		charstrings = realloc(font->charstrings, grown * sizeof(*charstrings));
		if (charstrings == NULL)
			return ENOMEM;
		font->charstrings = charstrings;
		names = realloc(font->names, grown * sizeof(*names));
		if (names == NULL)
			return ENOMEM;
		font->names = names;
		*capacity = grown;
	}

	/* The glyph is the next number. */
	font->charstrings[font->glyphs].data = data;
	font->charstrings[font->glyphs].size = size;
	font->names[font->glyphs].data = name->bytes;
	font->names[font->glyphs].size = name->length;
	font->glyphs++;

	/* Succeeded: the glyph is the program's. */
	return 0;
}

/*
 * Maps the codes of the program's own encoding to its glyphs by name:
 * StandardEncoding's names, or those the clear text listed.
 */
static void
map_builtin(
	struct pdf_charstrings *font,
	const unsigned char *const names[256],
	const size_t lengths[256],
	int standard)
{
	unsigned glyph;
	unsigned code;
	int error;

	/* Each code's glyph; a name the program lacks leaves the code without one. */
	for (code = 0; code < 256; code++) {
		if (standard) {
			error = charstrings_standard_glyph(font, code, &glyph);
		} else if (names[code] != NULL) {
			error = pdf_charstrings_find(font, names[code], lengths[code], &glyph);
		} else {
			error = ENOENT;
		}

		/* Keeps the glyph found. */
		if (error == 0)
			font->builtin[code] = glyph;
	}

	/* The program has its own encoding. */
	font->has_builtin = 1;
}

/* Runs one glyph's charstring, offset for seac's accent. */
static void
run_glyph(
	struct type1_run *run,
	unsigned glyph,
	int seac_depth)
{
	/* A glyph the program does not have draws nothing. */
	if (glyph >= run->font->glyphs)
		return;

	/* Runs its charstring from the top. */
	run->count = 0;
	run->ended = 0;
	run_charstring(run, &run->font->charstrings[glyph], 0, seac_depth);
}

/*
 * Runs a charstring or a subroutine: numbers go on the stack, operators
 * take them.  Damage (an unknown operator, a stack past its limit, a
 * subroutine the program lacks) ends the glyph.
 */
static void
run_charstring(
	struct type1_run *run,
	const struct charstrings_range *range,
	int depth,
	int seac_depth)
{
	struct charstrings_private *private;
	const unsigned char *data;
	unsigned long bits;
	size_t size;
	size_t position;
	size_t index;
	double value;
	int byte;
	int pushed;
	int keeps;

	/* Refuses calls nested past the limit. */
	if (depth > CHARSTRINGS_CALL_DEPTH) {
		run->ended = 1;
		return;
	}

	/* The private dictionary's subroutines, and the charstring's bytes. */
	private = &run->font->privates[0];
	data = range->data;
	size = range->size;

	/* Reads each number or operator until the charstring ends. */
	position = 0;
	while (position < size && !run->ended) {
		/* Bounds the work one glyph may take. */
		run->operators++;
		if (run->operators > CHARSTRINGS_OPERATORS_MAX) {
			run->ended = 1;
			return;
		}

		/* The next byte: a number's first, or an operator. */
		byte = data[position];
		position++;

		/* A number of one, two or five bytes (the last a 32-bit two's complement integer). */
		if (byte >= 32) {
			if (byte <= 246) {
				value = (double)(byte - 139);
			} else if (byte <= 250) {
				if (position >= size)
					break;
				value = (double)((byte - 247) * 256 + data[position] + 108);
				position++;
			} else if (byte <= 254) {
				if (position >= size)
					break;
				value = (double)(-(byte - 251) * 256 - data[position] - 108);
				position++;
			} else {
				if (size - position < 4)
					break;
				bits = ((unsigned long)data[position] << 24) | ((unsigned long)data[position + 1] << 16);
				bits |= ((unsigned long)data[position + 2] << 8) | data[position + 3];
				value = (double)bits;
				if (bits >= 0x80000000UL)
					value -= 4294967296.0;
				position += 4;
			}

			/* The number goes on the stack. */
			pushed = push(run, value);
			if (!pushed)
				return;
			continue;
		}

		/* An operator. */
		switch (byte) {
		case 1:
		case 3:
			/* hstem and vstem: hints, not drawn. */
			break;
		case 4:
			/* vmoveto. */
			if (run->count >= 1)
				pen_move(run, 0.0, run->stack[run->count - 1]);
			break;
		case 5:
			/* rlineto. */
			if (run->count >= 2)
				pen_line(run, run->stack[0], run->stack[1]);
			break;
		case 6:
			/* hlineto. */
			if (run->count >= 1)
				pen_line(run, run->stack[0], 0.0);
			break;
		case 7:
			/* vlineto. */
			if (run->count >= 1)
				pen_line(run, 0.0, run->stack[0]);
			break;
		case 8:
			/* rrcurveto. */
			if (run->count >= 6)
				pen_curve(run, run->stack[0], run->stack[1], run->stack[2], run->stack[3], run->stack[4], run->stack[5]);
			break;
		case 9:
			/* closepath. */
			charstrings_close(run->path);
			break;
		case 10:
			/* callsubr: the index is the top of the stack, which the subroutine then sees below it. */
			if (run->count < 1) {
				run->ended = 1;
				return;
			}

			/* The index must name a subroutine the program has. */
			value = run->stack[run->count - 1];
			run->count--;
			if (!(value >= 0.0 && value < (double)private->subrs_count)) {
				run->ended = 1;
				return;
			}

			/* A subroutine the program left out ends the glyph. */
			index = (size_t)value;
			if (private->subrs[index].data == NULL) {
				run->ended = 1;
				return;
			}

			/* Runs it; the stack it leaves is this charstring's. */
			run_charstring(run, &private->subrs[index], depth + 1, seac_depth);
			continue;
		case 11:
			/* return. */
			return;
		case 12:
			/* An escaped operator. */
			if (position >= size) {
				run->ended = 1;
				return;
			}

			/* Runs it; div, callothersubr and pop leave the stack to the operators after them. */
			byte = data[position];
			position++;
			run_escape(run, byte, seac_depth);
			keeps = 0;
			if (byte == 12)
				keeps = 1;
			if (byte == 16)
				keeps = 1;
			if (byte == 17)
				keeps = 1;
			if (keeps)
				continue;
			break;
		case 13:
			/* hsbw: the side bearing is the pen's start and the width the advance. */
			if (run->count >= 2) {
				run->x = run->stack[0];
				run->y = 0.0;
				run->width = run->stack[1];
			}

			break;
		case 14:
			/* endchar. */
			charstrings_close(run->path);
			run->ended = 1;
			return;
		case 21:
			/* rmoveto. */
			if (run->count >= 2)
				pen_move(run, run->stack[run->count - 2], run->stack[run->count - 1]);
			break;
		case 22:
			/* hmoveto. */
			if (run->count >= 1)
				pen_move(run, run->stack[run->count - 1], 0.0);
			break;
		case 30:
			/* vhcurveto. */
			if (run->count >= 4)
				pen_curve(run, 0.0, run->stack[0], run->stack[1], run->stack[2], run->stack[3], 0.0);
			break;
		case 31:
			/* hvcurveto. */
			if (run->count >= 4)
				pen_curve(run, run->stack[0], 0.0, run->stack[1], run->stack[2], 0.0, run->stack[3]);
			break;
		default:
			/* An operator Type 1 does not have ends the glyph. */
			run->ended = 1;
			return;
		}

		/* The operator took the stack. */
		run->count = 0;
	}
}

/* Runs an escaped operator. */
static void
run_escape(
	struct type1_run *run,
	int code,
	int seac_depth)
{
	double quotient;

	/* By the second byte. */
	switch (code) {
	case 0:
	case 1:
	case 2:
		/* dotsection, vstem3, hstem3: hints. */
		break;
	case 6:
		/* seac. */
		run_seac(run, seac_depth);
		break;
	case 7:
		/* sbw. */
		if (run->count >= 4) {
			run->x = run->stack[0];
			run->y = run->stack[1];
			run->width = run->stack[2];
		}

		break;
	case 12:
		/* div: the two top numbers become their quotient (0 for a zero divisor). */
		if (run->count < 2)
			break;
		quotient = 0.0;
		if (run->stack[run->count - 1] != 0.0)
			quotient = run->stack[run->count - 2] / run->stack[run->count - 1];
		run->stack[run->count - 2] = quotient;
		run->count--;
		break;
	case 16:
		/* callothersubr. */
		run_othersubr(run);
		break;
	case 17:
		/* pop: the next value an othersubr returned (0 when none). */
		if (run->results_count > 0) {
			run->results_count--;
			(void)push(run, run->results[run->results_count]);
		} else {
			(void)push(run, 0.0);
		}

		break;
	case 33:
		/* setcurrentpoint. */
		if (run->count >= 2) {
			run->x = run->stack[run->count - 2];
			run->y = run->stack[run->count - 1];
		}

		break;
	default:
		/* An escaped operator Type 1 does not have ends the glyph. */
		run->ended = 1;
		break;
	}
}

/*
 * Runs callothersubr: its number and argument count are the top of the
 * stack, the arguments below them.  Flex (0 to 2) is drawn; any other
 * returns its arguments for pop, which hint replacement (3) expects.
 */
static void
run_othersubr(
	struct type1_run *run)
{
	double number_value;
	double arguments_value;
	int number;
	int arguments;
	int index;

	/* The number and the argument count, which must be counts the stack holds. */
	if (run->count < 2) {
		run->ended = 1;
		return;
	}

	/* Both come off the stack; the arguments must be on it. */
	number_value = run->stack[run->count - 1];
	arguments_value = run->stack[run->count - 2];
	run->count -= 2;
	if (!(number_value >= 0.0 && number_value < 1000.0)) {
		run->ended = 1;
		return;
	}

	/* The arguments must be on the stack. */
	if (!(arguments_value >= 0.0 && arguments_value <= (double)run->count)) {
		run->ended = 1;
		return;
	}

	/* Both are whole numbers in range. */
	number = (int)number_value;
	arguments = (int)arguments_value;

	/* Flex's start, its points (each rmoveto adds one), and its end. */
	if (number == 1) {
		run->in_flex = 1;
		run->flex_count = 0;
		run->count -= arguments;
		return;
	}

	/* A flex point: the rmoveto before it gathered it. */
	if (number == 2) {
		run->count -= arguments;
		return;
	}

	/* The flex's end. */
	if (number == 0) {
		run->count -= arguments;
		end_flex(run);
		return;
	}

	/* Any other returns its arguments, the first to be popped first. */
	run->results_count = 0;
	for (index = 0; index < arguments; index++) {
		run->results[run->results_count] = run->stack[run->count - 1 - index];
		run->results_count++;
	}

	/* The arguments come off the stack. */
	run->count -= arguments;
}

/*
 * Ends a flex: its seven points (the reference point, then the two
 * curves' three each) are drawn as the two curves, and the pen and the
 * values pop returns are the last point.
 */
static void
end_flex(
	struct type1_run *run)
{
	double ox;
	double oy;

	/* A flex without its seven points draws a line to its last point, if any. */
	run->in_flex = 0;
	ox = run->offset_x;
	oy = run->offset_y;
	if (run->flex_count != TYPE1_FLEX_POINTS) {
		if (run->flex_count > 0) {
			run->x = run->flex[run->flex_count - 1][0];
			run->y = run->flex[run->flex_count - 1][1];
			charstrings_line(run->path, run->x + ox, run->y + oy);
		}
	} else {
		charstrings_curve(run->path, run->flex[1][0] + ox, run->flex[1][1] + oy, run->flex[2][0] + ox, run->flex[2][1] + oy,
		    run->flex[3][0] + ox, run->flex[3][1] + oy);
		charstrings_curve(run->path, run->flex[4][0] + ox, run->flex[4][1] + oy, run->flex[5][0] + ox, run->flex[5][1] + oy,
		    run->flex[6][0] + ox, run->flex[6][1] + oy);
		run->x = run->flex[6][0];
		run->y = run->flex[6][1];
	}

	/* pop pop setcurrentpoint gets the point back: x first, then y. */
	run->results[0] = run->y;
	run->results[1] = run->x;
	run->results_count = 2;
}

/*
 * Runs seac (asb adx ady bchar achar): the base character, then the
 * accent moved by (adx - asb, ady), both named by their StandardEncoding
 * codes.
 */
static void
run_seac(
	struct type1_run *run,
	int seac_depth)
{
	double saved_x;
	double saved_y;
	double width;
	double accent_x;
	double accent_y;
	unsigned base;
	unsigned accent;
	int error;

	/* Refuses a seac inside a seac, and one without its arguments or with codes that are not codes. */
	run->ended = 1;
	if (seac_depth >= TYPE1_SEAC_DEPTH || run->count < 5)
		return;
	if (!(run->stack[3] >= 0.0 && run->stack[3] < 256.0))
		return;
	if (!(run->stack[4] >= 0.0 && run->stack[4] < 256.0))
		return;
	accent_x = run->stack[1] - run->stack[0];
	accent_y = run->stack[2];

	/* The two glyphs. */
	error = charstrings_standard_glyph(run->font, (unsigned)run->stack[3], &base);
	if (error != 0)
		return;
	error = charstrings_standard_glyph(run->font, (unsigned)run->stack[4], &accent);
	if (error != 0)
		return;

	/* The base where this glyph is, keeping this glyph's width. */
	width = run->width;
	saved_x = run->offset_x;
	saved_y = run->offset_y;
	charstrings_close(run->path);
	run_glyph(run, base, seac_depth + 1);
	charstrings_close(run->path);

	/* The accent, moved. */
	run->offset_x = saved_x + accent_x;
	run->offset_y = saved_y + accent_y;
	run_glyph(run, accent, seac_depth + 1);
	charstrings_close(run->path);

	/* The glyph is drawn. */
	run->offset_x = saved_x;
	run->offset_y = saved_y;
	run->width = width;
	run->ended = 1;
}

/* Moves the pen; within a flex the point is gathered instead. */
static void
pen_move(
	struct type1_run *run,
	double dx,
	double dy)
{
	/* The new point. */
	run->x += dx;
	run->y += dy;

	/* A flex keeps its points for its end. */
	if (run->in_flex) {
		if (run->flex_count < TYPE1_FLEX_POINTS) {
			run->flex[run->flex_count][0] = run->x;
			run->flex[run->flex_count][1] = run->y;
			run->flex_count++;
		}

		/* The pen moved without drawing. */
		return;
	}

	/* Starts a subpath there. */
	charstrings_move(run->path, run->x + run->offset_x, run->y + run->offset_y);
}

/* Draws a line by a displacement. */
static void
pen_line(
	struct type1_run *run,
	double dx,
	double dy)
{
	/* The line to the new point. */
	run->x += dx;
	run->y += dy;
	charstrings_line(run->path, run->x + run->offset_x, run->y + run->offset_y);
}

/* Draws a curve by three displacements, each from the point before. */
static void
pen_curve(
	struct type1_run *run,
	double dx1,
	double dy1,
	double dx2,
	double dy2,
	double dx3,
	double dy3)
{
	double x1;
	double y1;
	double x2;
	double y2;

	/* The control points and the end, in turn. */
	x1 = run->x + dx1;
	y1 = run->y + dy1;
	x2 = x1 + dx2;
	y2 = y1 + dy2;
	run->x = x2 + dx3;
	run->y = y2 + dy3;
	charstrings_curve(run->path, x1 + run->offset_x, y1 + run->offset_y, x2 + run->offset_x, y2 + run->offset_y,
	    run->x + run->offset_x, run->y + run->offset_y);
}

/* Pushes a number; a full stack ends the glyph (reported as 0). */
static int
push(
	struct type1_run *run,
	double value)
{
	/* Refuses a stack past its limit. */
	if (run->count >= CHARSTRINGS_STACK_MAX) {
		run->ended = 1;
		return 0;
	}

	/* Succeeded: the number is on top. */
	run->stack[run->count] = value;
	run->count++;
	return 1;
}
