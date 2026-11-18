/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The objects of a PDF being read: the arena they live in, the lexer that
 * cuts a document's bytes into tokens, and the parser that builds direct
 * objects from them.
 *
 * The input is not trusted.  Every read is checked against the end of the
 * document, nesting is bounded, and all memory comes from an arena with a
 * limit, so a malformed file ends in PDF_EFORMAT or ENOMEM and never in a
 * read out of bounds.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The size of an ordinary arena block; a larger request gets a block of its own. */
#define PDF_ARENA_BLOCK_SIZE 65536

/* The alignment of every allocation from an arena, enough for any object the reader keeps. */
#define PDF_ARENA_ALIGNMENT 16

/* The first capacity of the list an array's items or a dictionary's entries are gathered in. */
#define PDF_PARSE_LIST_INITIAL 8

/*
 * A growing list of objects, used while one array or dictionary is parsed.
 *
 * It is freed once its items are copied into the arena.
 */
struct pdf_object_list {
	struct pdf_object **items;
	size_t count;
	size_t capacity;
};

static int is_space(unsigned char character);
static int is_delimiter(unsigned char character);
static int hex_value(unsigned char character);
static size_t regular_run(const struct pdf_lexer *lexer, size_t start);
static int lex_number(struct pdf_lexer *lexer, struct pdf_token *token);
static int lex_name(struct pdf_lexer *lexer, struct pdf_token *token);
static int lex_literal_string(struct pdf_lexer *lexer, struct pdf_token *token);
static size_t decode_literal_string(const unsigned char *raw, size_t raw_length, unsigned char *decoded);
static int lex_hex_string(struct pdf_lexer *lexer, struct pdf_token *token);
static int parse_token(struct pdf_lexer *lexer, const struct pdf_token *token, int depth, struct pdf_object **object);
static int parse_integer_or_reference(struct pdf_lexer *lexer, const struct pdf_token *token, struct pdf_object **object);
static int parse_array(struct pdf_lexer *lexer, int depth, struct pdf_object **object);
static int parse_dictionary(struct pdf_lexer *lexer, int depth, struct pdf_object **object);
static struct pdf_object *new_object(struct pdf_arena *arena, enum pdf_object_type type);
static int list_append(struct pdf_object_list *list, struct pdf_object *item);
static int list_publish(struct pdf_arena *arena, const struct pdf_object_list *list, struct pdf_object ***published);

/*
 * Allocates zeroed memory that lives until the arena is freed.
 *
 * It reports NULL when the memory runs out or the arena's limit is reached.
 */
void *
pdf_arena_allocate(
	struct pdf_arena *arena,
	size_t size)
{
	struct pdf_arena_block *block;
	size_t rounded;
	size_t header;
	size_t block_size;
	unsigned char *memory;

	/* Refuses a request past the limit before rounding it could overflow. */
	if (size > PDF_READER_ARENA_MAX)
		return NULL;

	/* Rounds the request up to the alignment and refuses one that would pass the limit. */
	rounded = (size + PDF_ARENA_ALIGNMENT - 1) & ~(size_t)(PDF_ARENA_ALIGNMENT - 1);
	if (rounded == 0)
		rounded = PDF_ARENA_ALIGNMENT;
	if (rounded > PDF_READER_ARENA_MAX - arena->total)
		return NULL;

	/* The bytes of a block start after its header, aligned. */
	header = (sizeof(struct pdf_arena_block) + PDF_ARENA_ALIGNMENT - 1) & ~(size_t)(PDF_ARENA_ALIGNMENT - 1);

	/* Starts a new block when the newest one has no room. */
	block = arena->blocks;
	if (block == NULL || block->size - block->used < rounded) {
		block_size = PDF_ARENA_BLOCK_SIZE;
		if (rounded > block_size)
			block_size = rounded;
		block = malloc(header + block_size);
		if (block == NULL)
			return NULL;
		block->next = arena->blocks;
		block->used = 0;
		block->size = block_size;
		arena->blocks = block;
	}

	/* Hands out the next bytes of the block, cleared. */
	memory = (unsigned char *)block + header + block->used;
	block->used += rounded;
	arena->total += rounded;
	memset(memory, 0, rounded);

	/* Succeeded: the memory lives as long as the arena. */
	return memory;
}

/*
 * Frees every block of an arena, and with them every object it held.
 */
void
pdf_arena_free(
	struct pdf_arena *arena)
{
	struct pdf_arena_block *block;
	struct pdf_arena_block *next;

	/* Frees the blocks from the newest to the oldest. */
	for (block = arena->blocks; block != NULL; block = next) {
		next = block->next;
		free(block);
	}

	/* Leaves the arena empty and usable again. */
	arena->blocks = NULL;
	arena->total = 0;
}

/*
 * Skips white space and comments.
 */
void
pdf_lexer_skip_space(
	struct pdf_lexer *lexer)
{
	int space;

	/* Advances until a byte that starts a token, or the end. */
	while (lexer->position < lexer->size) {
		/* Steps over one white-space byte. */
		space = is_space(lexer->data[lexer->position]);
		if (space) {
			lexer->position++;
			continue;
		}

		/* Anything but a comment starts a token. */
		if (lexer->data[lexer->position] != '%')
			break;

		/* Skips the comment to the end of its line. */
		while (lexer->position < lexer->size) {
			if (lexer->data[lexer->position] == '\r' || lexer->data[lexer->position] == '\n')
				break;
			lexer->position++;
		}
	}
}

/*
 * Reads the next token.
 *
 * At the end of the bytes the token is PDF_TOKEN_END.  A byte sequence that
 * is no token reports PDF_EFORMAT.
 */
int
pdf_lexer_next(
	struct pdf_lexer *lexer,
	struct pdf_token *token)
{
	unsigned char character;
	unsigned char following;
	size_t end;
	int starts_number;
	int error;

	/* Starts the token after any white space and comments. */
	memset(token, 0, sizeof(*token));
	pdf_lexer_skip_space(lexer);
	if (lexer->position >= lexer->size) {
		token->type = PDF_TOKEN_END;
		return 0;
	}

	/* Looks at the first byte and the one after it, if any. */
	character = lexer->data[lexer->position];
	following = 0;
	if (lexer->position + 1 < lexer->size)
		following = lexer->data[lexer->position + 1];

	/* Reads the token the first byte starts. */
	switch (character) {
	case '[':
		token->type = PDF_TOKEN_ARRAY_OPEN;
		lexer->position++;
		return 0;
	case ']':
		token->type = PDF_TOKEN_ARRAY_CLOSE;
		lexer->position++;
		return 0;
	case '<':
		/* A doubled angle bracket opens a dictionary; a single one, a hexadecimal string. */
		if (following == '<') {
			token->type = PDF_TOKEN_DICTIONARY_OPEN;
			lexer->position += 2;
			return 0;
		}

		/* A single one opens a hexadecimal string. */
		error = lex_hex_string(lexer, token);
		return error;
	case '>':
		/* Only a doubled angle bracket closes something outside a string. */
		if (following != '>')
			return PDF_EFORMAT;
		token->type = PDF_TOKEN_DICTIONARY_CLOSE;
		lexer->position += 2;
		return 0;
	case '(':
		error = lex_literal_string(lexer, token);
		return error;
	case '/':
		error = lex_name(lexer, token);
		return error;
	case ')':
	case '{':
	case '}':
		/* A stray parenthesis or a PostScript brace starts no token the reader knows. */
		return PDF_EFORMAT;
	default:
		break;
	}

	/* A sign, a digit or a point starts a number. */
	starts_number = 0;
	if (character == '+' || character == '-')
		starts_number = 1;
	if (character == '.')
		starts_number = 1;
	if (character >= '0' && character <= '9')
		starts_number = 1;
	if (starts_number) {
		error = lex_number(lexer, token);
		return error;
	}

	/* Anything else is a keyword: a run of regular bytes. */
	end = regular_run(lexer, lexer->position);
	token->type = PDF_TOKEN_KEYWORD;
	token->bytes = lexer->data + lexer->position;
	token->length = end - lexer->position;
	lexer->position = end;

	/* Succeeded: the token is a keyword. */
	return 0;
}

/*
 * Reports whether a token is a given keyword.
 */
int
pdf_token_is_keyword(
	const struct pdf_token *token,
	const char *keyword)
{
	size_t length;
	int difference;

	/* Only a keyword token can be the keyword. */
	if (token->type != PDF_TOKEN_KEYWORD)
		return 0;

	/* Compares the length, then the bytes. */
	length = strlen(keyword);
	if (token->length != length)
		return 0;
	difference = memcmp(token->bytes, keyword, length);
	if (difference != 0)
		return 0;

	/* The token is the keyword. */
	return 1;
}

/*
 * Parses one direct object at the lexer's position.
 *
 * depth counts the arrays and dictionaries the object is nested in.  An
 * indirect reference is returned as a reference object; the caller
 * resolves it.
 */
int
pdf_parse_object(
	struct pdf_lexer *lexer,
	int depth,
	struct pdf_object **object)
{
	struct pdf_token token;
	int error;

	/* Reads the object's first token. */
	error = pdf_lexer_next(lexer, &token);
	if (error != 0)
		return error;

	/* Builds the object that token starts. */
	error = parse_token(lexer, &token, depth, object);
	if (error != 0)
		return error;

	/* Succeeded: object is the parsed object. */
	return 0;
}

/*
 * Finds a key's value in a dictionary or a stream's dictionary.
 *
 * It reports NULL when the object has no dictionary or not the key.  The
 * value may be a reference.
 */
struct pdf_object *
pdf_object_get(
	const struct pdf_object *dictionary,
	const char *key)
{
	size_t length;
	size_t index;
	int difference;

	/* Only a dictionary or a stream has keys. */
	if (dictionary == NULL)
		return NULL;
	if (dictionary->type != PDF_OBJECT_DICTIONARY && dictionary->type != PDF_OBJECT_STREAM)
		return NULL;

	/* Looks for the first entry whose key is the name. */
	length = strlen(key);
	for (index = 0; index < dictionary->count; index++) {
		/* Skips a key of another length. */
		if (dictionary->keys[index]->length != length)
			continue;

		/* Reports the value of the key with the same bytes. */
		difference = memcmp(dictionary->keys[index]->bytes, key, length);
		if (difference == 0)
			return dictionary->values[index];
	}

	/* The dictionary does not have the key. */
	return NULL;
}

/*
 * Reports whether an object is a given name.
 */
int
pdf_object_is_name(
	const struct pdf_object *object,
	const char *name)
{
	size_t length;
	int difference;

	/* Only a name object can be the name. */
	if (object == NULL)
		return 0;
	if (object->type != PDF_OBJECT_NAME)
		return 0;

	/* Compares the length, then the bytes. */
	length = strlen(name);
	if (object->length != length)
		return 0;
	difference = memcmp(object->bytes, name, length);
	if (difference != 0)
		return 0;

	/* The object is the name. */
	return 1;
}

/*
 * Reads an integer or a real object as a number.
 *
 * Any other object reports PDF_EFORMAT.
 */
int
pdf_object_number(
	const struct pdf_object *object,
	double *number)
{
	/* Refuses a missing object. */
	if (object == NULL)
		return PDF_EFORMAT;

	/* Converts an integer. */
	if (object->type == PDF_OBJECT_INTEGER) {
		*number = (double)object->integer;
		return 0;
	}

	/* Refuses anything but a real. */
	if (object->type != PDF_OBJECT_REAL)
		return PDF_EFORMAT;

	/* Succeeded: number is the real's value. */
	*number = object->real;
	return 0;
}

/* Reports whether a byte is PDF white space. */
static int
is_space(
	unsigned char character)
{
	/* The six white-space bytes of PDF. */
	switch (character) {
	case 0x00:
	case 0x09:
	case 0x0a:
	case 0x0c:
	case 0x0d:
	case 0x20:
		return 1;
	default:
		break;
	}

	/* Every other byte is not white space. */
	return 0;
}

/* Reports whether a byte is a PDF delimiter, which ends a regular run. */
static int
is_delimiter(
	unsigned char character)
{
	/* The ten delimiters of PDF. */
	switch (character) {
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

	/* Every other byte is not a delimiter. */
	return 0;
}

/* Reports the value of a hexadecimal digit, or -1 for another byte. */
static int
hex_value(
	unsigned char character)
{
	/* A decimal digit. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* An upper-case digit. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* A lower-case digit. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Not a hexadecimal digit. */
	return -1;
}

/* Finds the end of the run of regular bytes that starts at an offset. */
static size_t
regular_run(
	const struct pdf_lexer *lexer,
	size_t start)
{
	size_t end;
	int space;
	int delimiter;

	/* Advances until white space, a delimiter or the end. */
	for (end = start; end < lexer->size; end++) {
		space = is_space(lexer->data[end]);
		if (space)
			break;
		delimiter = is_delimiter(lexer->data[end]);
		if (delimiter)
			break;
	}

	/* Reports the offset just past the run. */
	return end;
}

/*
 * Reads a number: an optional sign, digits, and an optional point with
 * more digits.
 *
 * A real is read as the whole number its digits make, divided once by the
 * power of ten its fraction needs, so a number the writer printed reads
 * back as the same double.  An integer too large for a long is read as a
 * real.
 */
static int
lex_number(
	struct pdf_lexer *lexer,
	struct pdf_token *token)
{
	const unsigned char *text;
	size_t end;
	size_t length;
	size_t index;
	long integer;
	double real;
	double scale;
	int negative;
	int digits;
	int has_point;
	int overflowed;
	int digit;

	/* Takes the run of regular bytes, which a real number never makes long. */
	end = regular_run(lexer, lexer->position);
	text = lexer->data + lexer->position;
	length = end - lexer->position;
	if (length > PDF_READER_NUMBER_MAX)
		return PDF_EFORMAT;

	/* Reads the sign. */
	index = 0;
	negative = 0;
	if (text[0] == '-') {
		negative = 1;
		index = 1;
	} else if (text[0] == '+') {
		index = 1;
	}

	/* Reads the digits and the point, keeping the digits both as an integer and as a real. */
	integer = 0;
	real = 0.0;
	scale = 1.0;
	digits = 0;
	has_point = 0;
	overflowed = 0;
	for (; index < length; index++) {
		/* A single point separates the fraction. */
		if (text[index] == '.') {
			if (has_point)
				return PDF_EFORMAT;
			has_point = 1;
			continue;
		}

		/* Refuses anything else that is not a digit. */
		if (text[index] < '0' || text[index] > '9')
			return PDF_EFORMAT;
		digit = text[index] - '0';
		digits++;

		/* Adds the digit; a fraction digit also grows the divisor, which stays exact to 22 digits. */
		real = real * 10.0 + digit;
		if (has_point)
			scale *= 10.0;
		if (integer > (LONG_MAX - digit) / 10)
			overflowed = 1;
		if (!overflowed)
			integer = integer * 10 + digit;
	}

	/* Refuses a sign or a point without digits. */
	if (digits == 0)
		return PDF_EFORMAT;

	/* Places the point, then applies the sign. */
	real /= scale;
	if (negative) {
		integer = -integer;
		real = -real;
	}

	/* Reports a real when there was a point or the integer did not fit. */
	lexer->position = end;
	if (has_point || overflowed) {
		token->type = PDF_TOKEN_REAL;
		token->real = real;
		return 0;
	}

	/* Succeeded: the token is an integer. */
	token->type = PDF_TOKEN_INTEGER;
	token->integer = integer;
	token->real = real;
	return 0;
}

/*
 * Reads a name, decoding its #xx escapes.
 *
 * A # that is not followed by two hexadecimal digits is kept as it is, as
 * PDF 1.1 wrote names.
 */
static int
lex_name(
	struct pdf_lexer *lexer,
	struct pdf_token *token)
{
	unsigned char *decoded;
	size_t start;
	size_t end;
	size_t index;
	size_t length;
	int high;
	int low;

	/* Takes the run of regular bytes after the slash. */
	start = lexer->position + 1;
	end = regular_run(lexer, start);
	if (end - start > PDF_READER_NAME_MAX)
		return PDF_EFORMAT;

	/* Allocates the decoded name, which is never longer than the run. */
	decoded = pdf_arena_allocate(lexer->arena, end - start + 1);
	if (decoded == NULL)
		return ENOMEM;

	/* Copies each byte, turning each #xx into the byte it names. */
	length = 0;
	for (index = start; index < end; index++) {
		/* Reads the two digits of an escape that the run holds whole. */
		high = -1;
		low = -1;
		if (lexer->data[index] == '#' && end - index >= 3) {
			high = hex_value(lexer->data[index + 1]);
			low = hex_value(lexer->data[index + 2]);
		}

		/* Decodes an escape; a NUL is not allowed in a name. */
		if (high >= 0 && low >= 0) {
			if (high == 0 && low == 0)
				return PDF_EFORMAT;
			decoded[length] = (unsigned char)(high * 16 + low);
			length++;
			index += 2;
			continue;
		}

		/* Copies an ordinary byte. */
		decoded[length] = lexer->data[index];
		length++;
	}

	/* Succeeded: the token is the decoded name. */
	decoded[length] = '\0';
	lexer->position = end;
	token->type = PDF_TOKEN_NAME;
	token->bytes = decoded;
	token->length = length;
	return 0;
}

/*
 * Reads a literal string in parentheses.
 *
 * The first pass finds the balancing parenthesis; the second decodes the
 * escapes into the arena.
 */
static int
lex_literal_string(
	struct pdf_lexer *lexer,
	struct pdf_token *token)
{
	unsigned char *decoded;
	size_t start;
	size_t index;
	size_t length;
	int depth;

	/* Finds the parenthesis that closes the string, stepping over escaped bytes. */
	start = lexer->position + 1;
	depth = 1;
	for (index = start; index < lexer->size; index++) {
		if (lexer->data[index] == '\\') {
			index++;
			continue;
		}

		/* Parentheses nest; the one that brings the depth to zero closes the string. */
		if (lexer->data[index] == '(')
			depth++;
		if (lexer->data[index] == ')') {
			depth--;
			if (depth == 0)
				break;
		}
	}

	/* Refuses a string the document ends inside. */
	if (index >= lexer->size)
		return PDF_EFORMAT;

	/* Allocates the decoded string, which is never longer than its source. */
	decoded = pdf_arena_allocate(lexer->arena, index - start + 1);
	if (decoded == NULL)
		return ENOMEM;

	/* Decodes the escapes and the line ends. */
	length = decode_literal_string(lexer->data + start, index - start, decoded);

	/* Succeeded: the token is the decoded string. */
	decoded[length] = '\0';
	lexer->position = index + 1;
	token->type = PDF_TOKEN_STRING;
	token->bytes = decoded;
	token->length = length;
	return 0;
}

/*
 * Decodes the body of a literal string and reports its decoded length.
 *
 * A backslash escape names a control byte, a parenthesis, a backslash or up
 * to three octal digits; a backslash before a line end continues the line.
 * An unescaped line end of any kind becomes one line feed.
 */
static size_t
decode_literal_string(
	const unsigned char *raw,
	size_t raw_length,
	unsigned char *decoded)
{
	size_t index;
	size_t length;
	size_t digits;
	unsigned int value;
	unsigned char character;

	/* Walks the body once. */
	length = 0;
	for (index = 0; index < raw_length; index++) {
		character = raw[index];

		/* An unescaped carriage return, alone or before a line feed, is one line feed. */
		if (character == '\r') {
			if (index + 1 < raw_length && raw[index + 1] == '\n')
				index++;
			decoded[length] = '\n';
			length++;
			continue;
		}

		/* An ordinary byte, or a backslash that ends the body, is copied. */
		if (character != '\\' || index + 1 >= raw_length) {
			decoded[length] = character;
			length++;
			continue;
		}

		/* Decodes the escape after the backslash. */
		index++;
		character = raw[index];
		switch (character) {
		case 'n':
			decoded[length] = '\n';
			length++;
			break;
		case 'r':
			decoded[length] = '\r';
			length++;
			break;
		case 't':
			decoded[length] = '\t';
			length++;
			break;
		case 'b':
			decoded[length] = '\b';
			length++;
			break;
		case 'f':
			decoded[length] = '\f';
			length++;
			break;
		case '\r':
			/* A backslash before a line end continues the string on the next line. */
			if (index + 1 < raw_length && raw[index + 1] == '\n')
				index++;
			break;
		case '\n':
			break;
		default:
			/* Up to three octal digits name a byte; any other byte stands for itself. */
			if (character >= '0' && character <= '7') {
				value = 0;
				for (digits = 0; digits < 3; digits++) {
					/* Stops at the end of the body or at a byte that is not an octal digit. */
					if (index >= raw_length)
						break;
					if (raw[index] < '0' || raw[index] > '7')
						break;
					value = value * 8 + (unsigned int)(raw[index] - '0');
					index++;
				}

				/* The loop stopped past the last digit, and the outer loop steps over it. */
				index--;
				decoded[length] = (unsigned char)(value & 0xff);
			} else {
				decoded[length] = character;
			}

			/* One byte more is decoded. */
			length++;
			break;
		}
	}

	/* Reports the decoded length. */
	return length;
}

/*
 * Reads a hexadecimal string in angle brackets.
 *
 * White space between the digits is ignored, and an odd last digit is
 * followed by an implied zero.
 */
static int
lex_hex_string(
	struct pdf_lexer *lexer,
	struct pdf_token *token)
{
	unsigned char *decoded;
	size_t start;
	size_t index;
	size_t digits;
	size_t length;
	int value;
	int space;

	/* Finds the closing bracket, refusing any byte that is not a digit or white space. */
	start = lexer->position + 1;
	digits = 0;
	for (index = start; index < lexer->size; index++) {
		if (lexer->data[index] == '>')
			break;
		space = is_space(lexer->data[index]);
		if (space)
			continue;
		value = hex_value(lexer->data[index]);
		if (value < 0)
			return PDF_EFORMAT;
		digits++;
	}

	/* Refuses a string the document ends inside. */
	if (index >= lexer->size)
		return PDF_EFORMAT;

	/* Allocates the decoded bytes. */
	decoded = pdf_arena_allocate(lexer->arena, digits / 2 + 2);
	if (decoded == NULL)
		return ENOMEM;

	/* Decodes each pair of digits; a lone last digit is the high half of its byte. */
	digits = 0;
	for (index = start; lexer->data[index] != '>'; index++) {
		value = hex_value(lexer->data[index]);
		if (value < 0)
			continue;
		if (digits % 2 == 0) {
			decoded[digits / 2] = (unsigned char)(value << 4);
		} else {
			decoded[digits / 2] |= (unsigned char)value;
		}

		/* One digit more is read. */
		digits++;
	}

	/* An odd last digit counts as a byte whose low digit is zero. */
	length = (digits + 1) / 2;

	/* Succeeded: the token is the decoded string. */
	decoded[length] = '\0';
	lexer->position = index + 1;
	token->type = PDF_TOKEN_STRING;
	token->bytes = decoded;
	token->length = length;
	return 0;
}

/* Builds the object that an already read token starts. */
static int
parse_token(
	struct pdf_lexer *lexer,
	const struct pdf_token *token,
	int depth,
	struct pdf_object **object)
{
	struct pdf_object *created;
	int is_null;
	int is_true;
	int is_false;
	int error;

	/* Refuses nesting deeper than the limit. */
	if (depth > PDF_READER_DEPTH_MAX)
		return PDF_EFORMAT;

	/* Builds the object by the kind of its first token. */
	created = NULL;
	switch (token->type) {
	case PDF_TOKEN_INTEGER:
		error = parse_integer_or_reference(lexer, token, object);
		return error;
	case PDF_TOKEN_ARRAY_OPEN:
		error = parse_array(lexer, depth, object);
		return error;
	case PDF_TOKEN_DICTIONARY_OPEN:
		error = parse_dictionary(lexer, depth, object);
		return error;
	case PDF_TOKEN_REAL:
		created = new_object(lexer->arena, PDF_OBJECT_REAL);
		if (created == NULL)
			return ENOMEM;
		created->real = token->real;
		break;
	case PDF_TOKEN_NAME:
		created = new_object(lexer->arena, PDF_OBJECT_NAME);
		if (created == NULL)
			return ENOMEM;
		created->bytes = token->bytes;
		created->length = token->length;
		break;
	case PDF_TOKEN_STRING:
		created = new_object(lexer->arena, PDF_OBJECT_STRING);
		if (created == NULL)
			return ENOMEM;
		created->bytes = token->bytes;
		created->length = token->length;
		break;
	case PDF_TOKEN_KEYWORD:
		/* Only the three constant keywords are objects. */
		is_null = pdf_token_is_keyword(token, "null");
		is_true = pdf_token_is_keyword(token, "true");
		is_false = pdf_token_is_keyword(token, "false");
		if (is_null) {
			created = new_object(lexer->arena, PDF_OBJECT_NULL);
		} else if (is_true || is_false) {
			created = new_object(lexer->arena, PDF_OBJECT_BOOLEAN);
		} else {
			return PDF_EFORMAT;
		}

		/* The keyword's object, when it could be made. */
		if (created == NULL)
			return ENOMEM;
		created->boolean = is_true;
		break;
	default:
		/* The end of the bytes or a closing bracket where an object belongs. */
		return PDF_EFORMAT;
	}

	/* Succeeded: object is the new direct object. */
	*object = created;
	return 0;
}

/*
 * Builds an integer, or the reference that the integer starts.
 *
 * A reference is two non-negative integers and the keyword R.  When the
 * next tokens are not that, the lexer goes back to just after the integer.
 */
static int
parse_integer_or_reference(
	struct pdf_lexer *lexer,
	const struct pdf_token *token,
	struct pdf_object **object)
{
	struct pdf_object *created;
	struct pdf_token generation;
	struct pdf_token keyword;
	size_t after_integer;
	int is_reference;
	int error;

	/*
	 * Looks ahead for a generation and the keyword R.  A token that cannot
	 * be read ends the look-ahead; the integer's reader will meet it again.
	 */
	after_integer = lexer->position;
	is_reference = 0;
	error = pdf_lexer_next(lexer, &generation);
	if (error != 0)
		generation.type = PDF_TOKEN_END;
	if (generation.type == PDF_TOKEN_INTEGER) {
		error = pdf_lexer_next(lexer, &keyword);
		if (error != 0)
			keyword.type = PDF_TOKEN_END;
		is_reference = pdf_token_is_keyword(&keyword, "R");
	}

	/* Builds the reference; a negative number cannot name an object. */
	if (is_reference) {
		if (token->integer < 0 || generation.integer < 0)
			return PDF_EFORMAT;
		created = new_object(lexer->arena, PDF_OBJECT_REFERENCE);
		if (created == NULL)
			return ENOMEM;
		created->number = (unsigned long)token->integer;
		created->generation = (unsigned long)generation.integer;
		*object = created;
		return 0;
	}

	/* Goes back to just after the integer, which stands alone. */
	lexer->position = after_integer;
	created = new_object(lexer->arena, PDF_OBJECT_INTEGER);
	if (created == NULL)
		return ENOMEM;
	created->integer = token->integer;

	/* Succeeded: object is the integer. */
	*object = created;
	return 0;
}

/* Builds an array from the tokens after its opening bracket. */
static int
parse_array(
	struct pdf_lexer *lexer,
	int depth,
	struct pdf_object **object)
{
	struct pdf_object_list list;
	struct pdf_object *created;
	struct pdf_object *item;
	struct pdf_token token;
	int error;

	/* Gathers the items until the closing bracket. */
	memset(&list, 0, sizeof(list));
	for (;;) {
		/* Reads the next item's first token, or the closing bracket. */
		error = pdf_lexer_next(lexer, &token);
		if (error != 0)
			break;
		if (token.type == PDF_TOKEN_ARRAY_CLOSE)
			break;

		/* Builds the item one level deeper and adds it. */
		error = parse_token(lexer, &token, depth + 1, &item);
		if (error != 0)
			break;
		error = list_append(&list, item);
		if (error != 0)
			break;
	}

	/* Reports why the items could not be gathered. */
	if (error != 0) {
		free(list.items);
		return error;
	}

	/* Allocates the array. */
	created = new_object(lexer->arena, PDF_OBJECT_ARRAY);
	if (created == NULL) {
		free(list.items);
		return ENOMEM;
	}

	/* Moves the items into the arena beside the array. */
	error = list_publish(lexer->arena, &list, &created->values);
	free(list.items);
	if (error != 0)
		return error;

	/* Succeeded: object is the array. */
	created->count = list.count;
	*object = created;
	return 0;
}

/* Builds a dictionary from the tokens after its opening brackets. */
static int
parse_dictionary(
	struct pdf_lexer *lexer,
	int depth,
	struct pdf_object **object)
{
	struct pdf_object_list keys;
	struct pdf_object_list values;
	struct pdf_object *created;
	struct pdf_object *key;
	struct pdf_object *value;
	struct pdf_token token;
	int error;

	/* Gathers the entries until the closing brackets. */
	memset(&keys, 0, sizeof(keys));
	memset(&values, 0, sizeof(values));
	for (;;) {
		/* Reads the next key, or the closing brackets; a key must be a name. */
		error = pdf_lexer_next(lexer, &token);
		if (error != 0)
			break;
		if (token.type == PDF_TOKEN_DICTIONARY_CLOSE)
			break;
		if (token.type != PDF_TOKEN_NAME) {
			error = PDF_EFORMAT;
			break;
		}

		/* The key, a name. */
		error = parse_token(lexer, &token, depth + 1, &key);
		if (error != 0)
			break;

		/* Builds the value one level deeper. */
		error = pdf_parse_object(lexer, depth + 1, &value);
		if (error != 0)
			break;

		/* Adds the entry. */
		error = list_append(&keys, key);
		if (error != 0)
			break;
		error = list_append(&values, value);
		if (error != 0)
			break;
	}

	/* Reports why the entries could not be gathered. */
	if (error != 0) {
		free(keys.items);
		free(values.items);
		return error;
	}

	/* Allocates the dictionary. */
	created = new_object(lexer->arena, PDF_OBJECT_DICTIONARY);
	if (created == NULL) {
		free(keys.items);
		free(values.items);
		return ENOMEM;
	}

	/* Moves the keys into the arena beside the dictionary. */
	error = list_publish(lexer->arena, &keys, &created->keys);
	free(keys.items);
	if (error != 0) {
		free(values.items);
		return error;
	}

	/* Moves the values into the arena beside the dictionary. */
	error = list_publish(lexer->arena, &values, &created->values);
	free(values.items);
	if (error != 0)
		return error;

	/* Succeeded: object is the dictionary. */
	created->count = keys.count;
	*object = created;
	return 0;
}

/* Allocates an object of a type from the arena. */
static struct pdf_object *
new_object(
	struct pdf_arena *arena,
	enum pdf_object_type type)
{
	struct pdf_object *object;

	/* Allocates the object with every field cleared. */
	object = pdf_arena_allocate(arena, sizeof(*object));
	if (object == NULL)
		return NULL;

	/* Succeeded: the object has its type. */
	object->type = type;
	return object;
}

/* Adds an object to a growing list. */
static int
list_append(
	struct pdf_object_list *list,
	struct pdf_object *item)
{
	struct pdf_object **grown;
	size_t capacity;

	/* Grows the list when it is full. */
	if (list->count == list->capacity) {
		capacity = list->capacity * 2;
		if (capacity == 0)
			capacity = PDF_PARSE_LIST_INITIAL;
		if (capacity > PDF_READER_ARENA_MAX / sizeof(*grown))
			return ENOMEM;
		grown = realloc(list->items, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		list->items = grown;
		list->capacity = capacity;
	}

	/* Succeeded: the item is the list's last. */
	list->items[list->count] = item;
	list->count++;
	return 0;
}

/* Copies a list's items into the arena, where the object that holds them lives. */
static int
list_publish(
	struct pdf_arena *arena,
	const struct pdf_object_list *list,
	struct pdf_object ***published)
{
	struct pdf_object **items;

	/* An empty list needs no memory. */
	if (list->count == 0) {
		*published = NULL;
		return 0;
	}

	/* Allocates the items' copy. */
	items = pdf_arena_allocate(arena, list->count * sizeof(*items));
	if (items == NULL)
		return ENOMEM;
	memcpy(items, list->items, list->count * sizeof(*items));

	/* Succeeded: the items live in the arena. */
	*published = items;
	return 0;
}
