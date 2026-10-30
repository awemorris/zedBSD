/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The formatted input of ISO C: scanf, fscanf, sscanf and their va_list
 * forms.
 *
 * One scanner serves a stream and a string alike.  It takes one character
 * at a time and gives back at most the one character it looked ahead at,
 * so a stream needs no more than the one character of pushback ungetc
 * promises, and what a conversion did not use is still in the stream for
 * the next read.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* How many bytes of a numeric item fit before its text moves to the heap. */
#define SCAN_TEXT_LOCAL 128

/*
 * What became of one directive of the format.
 *
 * ISO C (7.23.6.2) tells the two failures apart: a matching failure ends
 * the call with the count of the items assigned, an input failure (the
 * input ended, could not be read or held an encoding error) ends it with
 * EOF when nothing was assigned yet.
 */
enum scan_outcome {
	/* The directive was carried out; the next one follows. */
	SCAN_MATCHED,

	/* The input did not have the form the directive asks for. */
	SCAN_MATCHING_FAILURE,

	/* The input ended or failed before the directive was done. */
	SCAN_INPUT_FAILURE
};

/*
 * The object a conversion stores into, as its length modifier names it.
 *
 * SCAN_SIZE_POINTER is not a modifier: it is the void pointer of %p.
 */
enum scan_size {
	SCAN_SIZE_DEFAULT,
	SCAN_SIZE_CHAR,
	SCAN_SIZE_SHORT,
	SCAN_SIZE_LONG,
	SCAN_SIZE_LONG_LONG,
	SCAN_SIZE_INTMAX,
	SCAN_SIZE_SIZE,
	SCAN_SIZE_PTRDIFF,
	SCAN_SIZE_LONG_DOUBLE,
	SCAN_SIZE_POINTER
};

/*
 * Where one call's characters come from: a stream, or a string.
 *
 * It lives for one call.  consumed counts the characters taken and not
 * given back, which is what %n reports.
 */
struct scan_source {
	/* The stream read, or NULL when the string is read. */
	FILE *stream;

	/* The rest of the string, when there is no stream. */
	const unsigned char *string;

	/* How many characters the call has used so far. */
	size_t consumed;
};

/*
 * One conversion specification of the format, as parsed.
 */
struct scan_spec {
	/* Whether the item is read but not stored (%*d). */
	int suppress;

	/* The most characters the item may take; 0 when no width is given. */
	size_t width;

	/* The object the item is stored into. */
	enum scan_size size;

	/* The conversion character itself ('d', 's', '[', ...). */
	int conversion;
};

/*
 * The text of one numeric item, collected to be handed to strtoll, strtod
 * and their kin.
 *
 * A short item stays in local; a longer one moves to the heap, which the
 * conversion frees when it is done with the item.
 */
struct scan_text {
	/* The text so far, always NUL-terminated: local or a heap copy. */
	char *bytes;

	/* How many bytes the text holds, the NUL not counted. */
	size_t length;

	/* How many bytes bytes has room for. */
	size_t capacity;

	/* The room of a short item. */
	char local[SCAN_TEXT_LOCAL];
};

/*
 * The characters a %[ conversion accepts, one flag per byte.
 */
struct scan_set {
	/* Nonzero for a byte the set accepts. */
	unsigned char member[UCHAR_MAX + 1];
};

static int scan_format(struct scan_source *source, const char *format, va_list ap);
static const char *scan_skip_format_space(const char *format);
static const char *scan_parse_spec(const char *format, struct scan_spec *spec);
static const char *scan_parse_set(const char *format, struct scan_set *set);
static enum scan_outcome scan_convert(struct scan_source *source, const struct scan_spec *spec, const struct scan_set *set, void *target);
static int scan_get(struct scan_source *source);
static void scan_unget(struct scan_source *source, int character);
static int scan_next(struct scan_source *source, size_t *remaining);
static size_t scan_width(const struct scan_spec *spec);
static enum scan_outcome scan_skip_space(struct scan_source *source);
static enum scan_outcome scan_literal(struct scan_source *source, int expected);
static enum scan_outcome scan_outcome_of(int error);
static int scan_is_space(int character);
static int scan_is_digit(int character, int base);
static int scan_is_letter(int character, int letter);
static int scan_is_nan_character(int character);
static void scan_text_init(struct scan_text *text);
static int scan_text_add(struct scan_text *text, int character);
static void scan_text_free(struct scan_text *text);
static int scan_take(struct scan_source *source, size_t *remaining, struct scan_text *text, int *character);
static int scan_digits(struct scan_source *source, size_t *remaining, struct scan_text *text, int *character, int base, size_t *digits);
static size_t scan_word(struct scan_source *source, size_t *remaining, struct scan_text *text, int *character, const char *word);
static enum scan_outcome scan_integer(struct scan_source *source, const struct scan_spec *spec, int base, int is_signed, void *target);
static int scan_integer_text(struct scan_source *source, size_t *remaining, struct scan_text *text, int *base);
static void scan_integer_store(const struct scan_text *text, int base, int is_signed, enum scan_size size, void *target);
static void scan_store_signed(void *target, enum scan_size size, long long value);
static void scan_store_unsigned(void *target, enum scan_size size, unsigned long long value);
static enum scan_outcome scan_float(struct scan_source *source, const struct scan_spec *spec, void *target);
static int scan_float_text(struct scan_source *source, size_t *remaining, struct scan_text *text);
static int scan_float_infinity(struct scan_source *source, size_t *remaining, struct scan_text *text, int character);
static int scan_float_nan(struct scan_source *source, size_t *remaining, struct scan_text *text, int character);
static int scan_float_number(struct scan_source *source, size_t *remaining, struct scan_text *text, int character);
static int scan_float_exponent(struct scan_source *source, size_t *remaining, struct scan_text *text, int *character, int exponent);
static int scan_float_store(const struct scan_text *text, enum scan_size size, void *target);
static enum scan_outcome scan_string(struct scan_source *source, const struct scan_spec *spec, char *target);
static enum scan_outcome scan_chars(struct scan_source *source, const struct scan_spec *spec, char *target);
static enum scan_outcome scan_in_set(struct scan_source *source, const struct scan_spec *spec, const struct scan_set *set, char *target);
static enum scan_outcome scan_wide_one(struct scan_source *source, int first, mbstate_t *state, wchar_t *character);
static enum scan_outcome scan_wide_chars(struct scan_source *source, const struct scan_spec *spec, wchar_t *target);
static enum scan_outcome scan_wide_run(struct scan_source *source, const struct scan_spec *spec, const struct scan_set *set, wchar_t *target);

/*
 * Reads formatted input from a string.
 */
int
vsscanf(
	const char *input,
	const char *format,
	va_list ap)
{
	struct scan_source source;
	int count;

	/* The string is the source, read from its first character. */
	source.stream = NULL;
	source.string = (const unsigned char *)input;
	source.consumed = 0;

	/* Carries out the format. */
	count = scan_format(&source, format, ap);

	/* The items assigned, or EOF. */
	return count;
}

/*
 * Reads formatted input from a string.
 */
int
sscanf(
	const char *input,
	const char *format,
	...)
{
	va_list ap;
	int count;

	/* Carries out the format with the arguments. */
	va_start(ap, format);
	count = vsscanf(input, format, ap);
	va_end(ap);

	/* The items assigned, or EOF. */
	return count;
}

/*
 * Reads formatted input from a stream.
 *
 * The stream stays locked for the whole call, so another thread's reads do
 * not land between the characters of one item.  Whatever the format did
 * not use is left in the stream.
 */
int
vfscanf(
	FILE *stream,
	const char *format,
	va_list ap)
{
	struct scan_source source;
	int count;

	/* Refuses a missing stream. */
	if (stream == NULL) {
		errno = EINVAL;
		return EOF;
	}

	/* The stream is the source. */
	source.stream = stream;
	source.string = NULL;
	source.consumed = 0;

	/* Carries out the format while the stream is held. */
	flockfile(stream);

	count = scan_format(&source, format, ap);

	funlockfile(stream);

	/* The items assigned, or EOF. */
	return count;
}

/*
 * Reads formatted input from a stream.
 */
int
fscanf(
	FILE *stream,
	const char *format,
	...)
{
	va_list ap;
	int count;

	/* Carries out the format with the arguments. */
	va_start(ap, format);
	count = vfscanf(stream, format, ap);
	va_end(ap);

	/* The items assigned, or EOF. */
	return count;
}

/*
 * Reads formatted input from the standard input.
 */
int
vscanf(
	const char *format,
	va_list ap)
{
	int count;

	/* Carries out the format on the standard input. */
	count = vfscanf(stdin, format, ap);

	/* The items assigned, or EOF. */
	return count;
}

/*
 * Reads formatted input from the standard input.
 */
int
scanf(
	const char *format,
	...)
{
	va_list ap;
	int count;

	/* Carries out the format with the arguments. */
	va_start(ap, format);
	count = vfscanf(stdin, format, ap);
	va_end(ap);

	/* The items assigned, or EOF. */
	return count;
}

/* Carries out every directive of a format; returns the items assigned, or EOF. */
static int
scan_format(
	struct scan_source *source,
	const char *format,
	va_list ap)
{
	struct scan_spec spec;
	struct scan_set set;
	enum scan_outcome outcome;
	void *target;
	int assigned;
	int space;

	/* Nothing assigned yet. */
	assigned = 0;
	outcome = SCAN_MATCHED;

	/* Carries out the directives in order until one fails or the format ends. */
	while (*format != '\0') {
		/* A white-space directive skips any white space, new-lines included. */
		space = scan_is_space((unsigned char)*format);
		if (space) {
			format = scan_skip_format_space(format);
			(void)scan_skip_space(source);
			continue;
		}

		/* An ordinary character must be the input's next one. */
		if (*format != '%') {
			outcome = scan_literal(source, (unsigned char)*format);
			if (outcome != SCAN_MATCHED)
				break;
			format++;
			continue;
		}

		/* A conversion specification. */
		format = scan_parse_spec(format + 1, &spec);

		/* The set of a %[, without which the format is broken. */
		if (spec.conversion == '[') {
			format = scan_parse_set(format, &set);
			if (format == NULL) {
				outcome = SCAN_MATCHING_FAILURE;
				break;
			}
		}

		/* White space before the item is skipped, except for %[, %c and %n. */
		if (spec.conversion != '[' &&
		    spec.conversion != 'c' &&
		    spec.conversion != 'n') {
			outcome = scan_skip_space(source);
			if (outcome != SCAN_MATCHED)
				break;
		}

		/* The object the item goes to: none for %%, nor for a suppressed item. */
		target = NULL;
		if (spec.suppress == 0 && spec.conversion != '%')
			target = va_arg(ap, void *);

		/* Reads and stores the item. */
		outcome = scan_convert(source, &spec, &set, target);
		if (outcome != SCAN_MATCHED)
			break;

		/* An item stored counts; %n and %% are not items. */
		if (target != NULL && spec.conversion != 'n')
			assigned++;
	}

	/* An input failure before anything was assigned is reported as EOF. */
	if (outcome == SCAN_INPUT_FAILURE && assigned == 0)
		return EOF;

	/* Succeeded or stopped at a mismatch: the items assigned. */
	return assigned;
}

/* Skips the white space of a format; returns the format after it. */
static const char *
scan_skip_format_space(
	const char *format)
{
	int space;

	/* Steps over each white-space character. */
	for (;;) {
		space = scan_is_space((unsigned char)*format);
		if (!space)
			break;
		format++;
	}

	/* The first other character of the format. */
	return format;
}

/* Parses the flags, width and length of a specification; returns the format after its conversion character. */
static const char *
scan_parse_spec(
	const char *format,
	struct scan_spec *spec)
{
	size_t digit;

	/* No suppression, width or length modifier until the format gives one. */
	spec->suppress = 0;
	spec->width = 0;
	spec->size = SCAN_SIZE_DEFAULT;

	/* The assignment suppression. */
	if (*format == '*') {
		spec->suppress = 1;
		format++;
	}

	/* The maximum field width, held at the largest size_t a longer one would pass. */
	while (*format >= '0' && *format <= '9') {
		digit = (size_t)(*format - '0');
		if (spec->width <= (SIZE_MAX - digit) / 10U)
			spec->width = spec->width * 10U + digit;
		format++;
	}

	/* The length modifier. */
	if (format[0] == 'h' && format[1] == 'h') {
		spec->size = SCAN_SIZE_CHAR;
		format += 2;
	} else if (format[0] == 'h') {
		spec->size = SCAN_SIZE_SHORT;
		format++;
	} else if (format[0] == 'l' && format[1] == 'l') {
		spec->size = SCAN_SIZE_LONG_LONG;
		format += 2;
	} else if (format[0] == 'l') {
		spec->size = SCAN_SIZE_LONG;
		format++;
	} else if (format[0] == 'j') {
		spec->size = SCAN_SIZE_INTMAX;
		format++;
	} else if (format[0] == 'z') {
		spec->size = SCAN_SIZE_SIZE;
		format++;
	} else if (format[0] == 't') {
		spec->size = SCAN_SIZE_PTRDIFF;
		format++;
	} else if (format[0] == 'L') {
		spec->size = SCAN_SIZE_LONG_DOUBLE;
		format++;
	}

	/* The conversion character; a format ending here has none. */
	spec->conversion = (unsigned char)*format;
	if (*format != '\0')
		format++;

	/* The format after the specification. */
	return format;
}

/*
 * Parses the set of a %[ conversion; returns the format after its closing
 * ']', or NULL when there is none.
 *
 * A ']' right after '[' or '[^' is a member.  A '-' between two bytes, the
 * first not above the second, names the range from one to the other, as the
 * BSD and GNU libraries read it; a '-' first or last is itself a member.
 */
static const char *
scan_parse_set(
	const char *format,
	struct scan_set *set)
{
	unsigned int index;
	unsigned int low;
	unsigned int high;
	int previous;
	int invert;

	/* An empty set, and whether it is inverted. */
	memset(set->member, 0, sizeof(set->member));
	invert = 0;
	if (*format == '^') {
		invert = 1;
		format++;
	}

	/* A ']' first is a member, not the end. */
	previous = -1;
	if (*format == ']') {
		set->member[']'] = 1;
		previous = ']';
		format++;
	}

	/* Takes each member and range up to the closing ']'. */
	while (*format != '\0' && *format != ']') {
		/* A '-' after a member and before another byte, not below it, is a range. */
		if (*format == '-' &&
		    previous >= 0 &&
		    format[1] != '\0' &&
		    format[1] != ']' &&
		    (unsigned char)format[1] >= (unsigned int)previous) {
			low = (unsigned int)previous;
			high = (unsigned char)format[1];
			for (index = low; index <= high; index++)
				set->member[index] = 1;
			previous = -1;
			format += 2;
			continue;
		}

		/* Any other byte is a member. */
		set->member[(unsigned char)*format] = 1;
		previous = (unsigned char)*format;
		format++;
	}

	/* A set without its closing ']' is no set. */
	if (*format != ']')
		return NULL;

	/* An inverted set accepts what the format did not list. */
	if (invert) {
		for (index = 0; index <= UCHAR_MAX; index++)
			set->member[index] = (unsigned char)(set->member[index] == 0);
	}

	/* Succeeded: the format after the set. */
	return format + 1;
}

/* Reads one item as its conversion says and stores it into target unless that is NULL. */
static enum scan_outcome
scan_convert(
	struct scan_source *source,
	const struct scan_spec *spec,
	const struct scan_set *set,
	void *target)
{
	struct scan_spec pointer_spec;
	enum scan_outcome outcome;

	/* Chooses the reader of the conversion. */
	switch (spec->conversion) {
	case '%':
		outcome = scan_literal(source, '%');
		break;
	case 'd':
		outcome = scan_integer(source, spec, 10, 1, target);
		break;
	case 'i':
		outcome = scan_integer(source, spec, 0, 1, target);
		break;
	case 'o':
		outcome = scan_integer(source, spec, 8, 0, target);
		break;
	case 'u':
		outcome = scan_integer(source, spec, 10, 0, target);
		break;
	case 'x':
	case 'X':
		outcome = scan_integer(source, spec, 16, 0, target);
		break;
	case 'p':
		/* A pointer is read as hexadecimal and stored as a void pointer. */
		pointer_spec = *spec;
		pointer_spec.size = SCAN_SIZE_POINTER;
		outcome = scan_integer(source, &pointer_spec, 16, 0, target);
		break;
	case 'a':
	case 'A':
	case 'e':
	case 'E':
	case 'f':
	case 'F':
	case 'g':
	case 'G':
		outcome = scan_float(source, spec, target);
		break;
	case 's':
		/* %ls reads wide characters, %s bytes. */
		if (spec->size == SCAN_SIZE_LONG)
			outcome = scan_wide_run(source, spec, NULL, target);
		else
			outcome = scan_string(source, spec, target);
		break;
	case 'c':
		/* %lc reads wide characters, %c bytes. */
		if (spec->size == SCAN_SIZE_LONG)
			outcome = scan_wide_chars(source, spec, target);
		else
			outcome = scan_chars(source, spec, target);
		break;
	case '[':
		/* %l[ reads wide characters, %[ bytes. */
		if (spec->size == SCAN_SIZE_LONG)
			outcome = scan_wide_run(source, spec, set, target);
		else
			outcome = scan_in_set(source, spec, set, target);
		break;
	case 'n':
		/* The count of the characters used so far; nothing is read. */
		if (target != NULL)
			scan_store_signed(target, spec->size, (long long)source->consumed);
		outcome = SCAN_MATCHED;
		break;
	default:
		/* An unknown conversion, or a format that ended after '%'. */
		outcome = SCAN_MATCHING_FAILURE;
		break;
	}

	/* What became of the item. */
	return outcome;
}

/* Takes the source's next character, or EOF at its end. */
static int
scan_get(
	struct scan_source *source)
{
	int character;

	/* The next character of the stream or of the string. */
	if (source->stream != NULL) {
		character = getc(source->stream);
	} else if (*source->string != '\0') {
		character = *source->string;
		source->string++;
	} else {
		character = EOF;
	}

	/* A character taken counts as used, for %n, until it is given back. */
	if (character != EOF)
		source->consumed++;

	/* The character, or EOF. */
	return character;
}

/* Gives back the one character looked ahead at; EOF gives back nothing. */
static void
scan_unget(
	struct scan_source *source,
	int character)
{
	/* The end of the input is not a character. */
	if (character == EOF)
		return;

	/* The stream or the string gets the character back. */
	if (source->stream != NULL)
		(void)ungetc(character, source->stream);
	else
		source->string--;
	source->consumed--;
}

/* Takes the next character of an item, or EOF once its width is used up. */
static int
scan_next(
	struct scan_source *source,
	size_t *remaining)
{
	int character;

	/* The width is used up: the item ends without reading. */
	if (*remaining == 0U)
		return EOF;

	/* One more character of the width. */
	character = scan_get(source);
	if (character != EOF)
		(*remaining)--;

	/* The character, or EOF. */
	return character;
}

/* Gives the most characters a conversion's item may take. */
static size_t
scan_width(
	const struct scan_spec *spec)
{
	/* Without a width an item is limited only by its form. */
	if (spec->width == 0U)
		return SIZE_MAX;

	/* The width the format gives. */
	return spec->width;
}

/* Skips white space; an input failure when the input ends before anything else. */
static enum scan_outcome
scan_skip_space(
	struct scan_source *source)
{
	int character;
	int space;

	/* Takes characters while they are white space. */
	for (;;) {
		character = scan_get(source);
		space = scan_is_space(character);
		if (!space)
			break;
	}

	/* The input ended in the white space. */
	if (character == EOF)
		return SCAN_INPUT_FAILURE;

	/* The first other character stays unread. */
	scan_unget(source, character);

	/* Succeeded: the next character is not white space. */
	return SCAN_MATCHED;
}

/* Matches one ordinary character of the format against the input. */
static enum scan_outcome
scan_literal(
	struct scan_source *source,
	int expected)
{
	int character;

	/* The input's next character. */
	character = scan_get(source);
	if (character == EOF)
		return SCAN_INPUT_FAILURE;

	/* A different character stays unread. */
	if (character != expected) {
		scan_unget(source, character);
		return SCAN_MATCHING_FAILURE;
	}

	/* Succeeded: the character matched. */
	return SCAN_MATCHED;
}

/* Turns a reader's error (0, EINVAL or ENOMEM) into the directive's outcome. */
static enum scan_outcome
scan_outcome_of(
	int error)
{
	/* Memory ran out for a long item: the input could not be read. */
	if (error == ENOMEM) {
		errno = ENOMEM;
		return SCAN_INPUT_FAILURE;
	}

	/* The input was not an item of the conversion. */
	if (error != 0)
		return SCAN_MATCHING_FAILURE;

	/* Succeeded: the item was read. */
	return SCAN_MATCHED;
}

/* Tells whether a character (EOF included) is white space. */
static int
scan_is_space(
	int character)
{
	int space;

	/* The end of the input is no white space. */
	if (character == EOF)
		return 0;

	/* The locale's white space. */
	space = isspace(character);
	if (space != 0)
		return 1;

	/* Any other character. */
	return 0;
}

/* Tells whether a character (EOF included) is a digit of a base (8, 10 or 16). */
static int
scan_is_digit(
	int character,
	int base)
{
	int hexadecimal;

	/* The end of the input is no digit. */
	if (character == EOF)
		return 0;

	/* Hexadecimal digits include the letters a to f. */
	if (base == 16) {
		hexadecimal = isxdigit(character);
		if (hexadecimal != 0)
			return 1;
		return 0;
	}

	/* Other bases take the decimal digits below the base. */
	if (character >= '0' && character < '0' + base)
		return 1;

	/* Any other character. */
	return 0;
}

/* Tells whether a character (EOF included) is a lower-case letter in either case. */
static int
scan_is_letter(
	int character,
	int letter)
{
	int lower;

	/* The end of the input is no letter. */
	if (character == EOF)
		return 0;

	/* The letter, whatever its case. */
	lower = tolower(character);
	if (lower == letter)
		return 1;

	/* Any other character. */
	return 0;
}

/* Tells whether a character (EOF included) may stand between the parentheses of a NaN. */
static int
scan_is_nan_character(
	int character)
{
	int alphanumeric;

	/* The end of the input ends the characters. */
	if (character == EOF)
		return 0;

	/* Letters, digits and the underscore. */
	alphanumeric = isalnum(character);
	if (alphanumeric != 0)
		return 1;
	if (character == '_')
		return 1;

	/* Any other character. */
	return 0;
}

/* Starts an empty item text in its local room. */
static void
scan_text_init(
	struct scan_text *text)
{
	text->bytes = text->local;
	text->length = 0;
	text->capacity = sizeof(text->local);
	text->local[0] = '\0';
}

/* Appends one character to an item text; returns 0, or ENOMEM. */
static int
scan_text_add(
	struct scan_text *text,
	int character)
{
	char *grown;
	size_t capacity;

	/* Grows the text into the heap when the character and the NUL do not fit. */
	if (text->length + 1U >= text->capacity) {
		/* Twice the room, unless that does not fit in a size_t. */
		if (text->capacity > SIZE_MAX / 2U)
			return ENOMEM;
		capacity = text->capacity * 2U;

		/* The first growth copies the local text; a later one reallocates. */
		if (text->bytes == text->local) {
			grown = malloc(capacity);
			if (grown == NULL)
				return ENOMEM;
			memcpy(grown, text->local, text->length + 1U);
		} else {
			grown = realloc(text->bytes, capacity);
			if (grown == NULL)
				return ENOMEM;
		}

		/* The text lives in the larger room from now on. */
		text->bytes = grown;
		text->capacity = capacity;
	}

	/* The character and the new end. */
	text->bytes[text->length] = (char)character;
	text->length++;
	text->bytes[text->length] = '\0';

	/* Succeeded: the text is one character longer. */
	return 0;
}

/* Frees an item text's heap room, if it has one. */
static void
scan_text_free(
	struct scan_text *text)
{
	if (text->bytes != text->local)
		free(text->bytes);
}

/* Adds the current character to an item and takes the next one; returns 0, or ENOMEM. */
static int
scan_take(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int *character)
{
	int error;

	/* The current character joins the item. */
	error = scan_text_add(text, *character);
	if (error != 0)
		return error;

	/* The next one is looked at. */
	*character = scan_next(source, remaining);

	/* Succeeded: *character is the input's next character. */
	return 0;
}

/* Adds the digits of a base while they come, counting them; returns 0, or ENOMEM. */
static int
scan_digits(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int *character,
	int base,
	size_t *digits)
{
	int digit;
	int error;

	/* Takes each digit. */
	for (;;) {
		digit = scan_is_digit(*character, base);
		if (!digit)
			break;
		error = scan_take(source, remaining, text, character);
		if (error != 0)
			return error;
		(*digits)++;
	}

	/* Succeeded: *character is the first character after the digits. */
	return 0;
}

/*
 * Takes the letters of a lower-case word, in any case, while the input
 * spells it; returns how many it took.  *character is the input's
 * character on entry and the first one not taken on return.
 */
static size_t
scan_word(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int *character,
	const char *word)
{
	size_t matched;
	int same;
	int error;

	/* Takes each letter while the input has it. */
	matched = 0;
	while (word[matched] != '\0') {
		same = scan_is_letter(*character, (unsigned char)word[matched]);
		if (!same)
			break;
		error = scan_take(source, remaining, text, character);
		if (error != 0)
			break;
		matched++;
	}

	/* How much of the word the input spelt. */
	return matched;
}

/*
 * Reads an integer item (%d, %i, %o, %u, %x, %p) and stores it unless
 * target is NULL.
 */
static enum scan_outcome
scan_integer(
	struct scan_source *source,
	const struct scan_spec *spec,
	int base,
	int is_signed,
	void *target)
{
	struct scan_text text;
	enum scan_outcome outcome;
	size_t remaining;
	int error;

	/* The item's text, limited to the width; base 0 becomes the base its prefix chooses. */
	remaining = scan_width(spec);
	scan_text_init(&text);
	error = scan_integer_text(source, &remaining, &text, &base);

	/* The value, stored when the item was read and is wanted. */
	if (error == 0 && target != NULL)
		scan_integer_store(&text, base, is_signed, spec->size, target);

	/* The text is done with. */
	scan_text_free(&text);

	/* What became of the item. */
	outcome = scan_outcome_of(error);
	return outcome;
}

/*
 * Collects the text of an integer item: an optional sign, for base 16 an
 * optional 0x, for base 0 a prefix that chooses the base (0x hexadecimal,
 * 0 octal, decimal otherwise), and the digits.  The item ends at the first
 * character that cannot continue it, which stays unread.  Returns 0,
 * EINVAL when there is no digit, or ENOMEM.
 */
static int
scan_integer_text(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int *base)
{
	size_t digits;
	int character;
	int error;

	/* The item's first character. */
	character = scan_next(source, remaining);
	digits = 0;
	error = 0;

	/* The optional sign. */
	if (character == '+' || character == '-')
		error = scan_take(source, remaining, text, &character);

	/* A leading 0, which may start 0x and for base 0 chooses octal or hexadecimal. */
	if (error == 0 &&
	    character == '0' &&
	    (*base == 0 || *base == 16)) {
		error = scan_take(source, remaining, text, &character);
		digits = 1;
		if (error == 0 && (character == 'x' || character == 'X')) {
			/* The 0 was the prefix: a hexadecimal digit must follow. */
			error = scan_take(source, remaining, text, &character);
			digits = 0;
			*base = 16;
		} else if (*base == 0) {
			/* A leading 0 alone makes the number octal. */
			*base = 8;
		}
	} else if (*base == 0) {
		/* Without a prefix the number is decimal. */
		*base = 10;
	}

	/* The digits of the base. */
	if (error == 0)
		error = scan_digits(source, remaining, text, &character, *base, &digits);

	/* The character after the item stays unread. */
	scan_unget(source, character);

	/* Memory ran out for a long item. */
	if (error != 0)
		return error;

	/* An item without a digit does not match. */
	if (digits == 0U)
		return EINVAL;

	/* Succeeded: the text is an integer of the base. */
	return 0;
}

/* Stores an integer item's value, read as strtoll or strtoull reads its text. */
static void
scan_integer_store(
	const struct scan_text *text,
	int base,
	int is_signed,
	enum scan_size size,
	void *target)
{
	unsigned long long unsigned_value;
	long long signed_value;

	/* A signed conversion reads the text as strtoll does, an unsigned one as strtoull does. */
	if (is_signed) {
		signed_value = strtoll(text->bytes, NULL, base);
		scan_store_signed(target, size, signed_value);
	} else {
		unsigned_value = strtoull(text->bytes, NULL, base);
		scan_store_unsigned(target, size, unsigned_value);
	}
}

/* Stores a signed value into the object its length modifier names. */
static void
scan_store_signed(
	void *target,
	enum scan_size size,
	long long value)
{
	/* Chooses the object's type. */
	switch (size) {
	case SCAN_SIZE_CHAR:
		*(signed char *)target = (signed char)value;
		break;
	case SCAN_SIZE_SHORT:
		*(short *)target = (short)value;
		break;
	case SCAN_SIZE_LONG:
		*(long *)target = (long)value;
		break;
	case SCAN_SIZE_LONG_LONG:
	case SCAN_SIZE_LONG_DOUBLE:
		*(long long *)target = value;
		break;
	case SCAN_SIZE_INTMAX:
		*(intmax_t *)target = (intmax_t)value;
		break;
	case SCAN_SIZE_SIZE:
		/* The signed type of size_t's width has size_t's representation. */
		*(size_t *)target = (size_t)value;
		break;
	case SCAN_SIZE_PTRDIFF:
		*(ptrdiff_t *)target = (ptrdiff_t)value;
		break;
	case SCAN_SIZE_POINTER:
		*(void **)target = (void *)(uintptr_t)value;
		break;
	default:
		*(int *)target = (int)value;
		break;
	}
}

/* Stores an unsigned value into the object its length modifier names. */
static void
scan_store_unsigned(
	void *target,
	enum scan_size size,
	unsigned long long value)
{
	/* Chooses the object's type. */
	switch (size) {
	case SCAN_SIZE_CHAR:
		*(unsigned char *)target = (unsigned char)value;
		break;
	case SCAN_SIZE_SHORT:
		*(unsigned short *)target = (unsigned short)value;
		break;
	case SCAN_SIZE_LONG:
		*(unsigned long *)target = (unsigned long)value;
		break;
	case SCAN_SIZE_LONG_LONG:
	case SCAN_SIZE_LONG_DOUBLE:
		*(unsigned long long *)target = value;
		break;
	case SCAN_SIZE_INTMAX:
		*(uintmax_t *)target = (uintmax_t)value;
		break;
	case SCAN_SIZE_SIZE:
		*(size_t *)target = (size_t)value;
		break;
	case SCAN_SIZE_PTRDIFF:
		/* The unsigned type of ptrdiff_t's width has ptrdiff_t's representation. */
		*(ptrdiff_t *)target = (ptrdiff_t)value;
		break;
	case SCAN_SIZE_POINTER:
		*(void **)target = (void *)(uintptr_t)value;
		break;
	default:
		*(unsigned int *)target = (unsigned int)value;
		break;
	}
}

/*
 * Reads a floating item (%a, %e, %f, %g): what strtod reads, a decimal or
 * hexadecimal number, an infinity or a NaN.  Stores it unless target is
 * NULL.
 */
static enum scan_outcome
scan_float(
	struct scan_source *source,
	const struct scan_spec *spec,
	void *target)
{
	struct scan_text text;
	enum scan_outcome outcome;
	size_t remaining;
	int error;

	/* The item's text, limited to the width. */
	remaining = scan_width(spec);
	scan_text_init(&text);
	error = scan_float_text(source, &remaining, &text);

	/* The value, stored in the precision of its object. */
	if (error == 0)
		error = scan_float_store(&text, spec->size, target);

	/* The text is done with. */
	scan_text_free(&text);

	/* What became of the item. */
	outcome = scan_outcome_of(error);
	return outcome;
}

/*
 * Collects the text of a floating item; returns 0, EINVAL when the input
 * is not one, or ENOMEM.  The character after the item stays unread.
 */
static int
scan_float_text(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text)
{
	int character;
	int error;

	/* The optional sign. */
	character = scan_next(source, remaining);
	if (character == '+' || character == '-') {
		error = scan_take(source, remaining, text, &character);
		if (error != 0) {
			scan_unget(source, character);
			return error;
		}
	}

	/* An infinity, a NaN or a number, told by its first character. */
	if (character == 'i' || character == 'I')
		error = scan_float_infinity(source, remaining, text, character);
	else if (character == 'n' || character == 'N')
		error = scan_float_nan(source, remaining, text, character);
	else
		error = scan_float_number(source, remaining, text, character);

	/* The reader's error, or 0 when the text is a floating item. */
	if (error != 0)
		return error;

	/* Succeeded: the text is a floating item. */
	return 0;
}

/* Collects an infinity, inf or infinity in any case, that starts with character. */
static int
scan_float_infinity(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int character)
{
	size_t matched;

	/* As much of infinity as the input spells. */
	matched = scan_word(source, remaining, text, &character, "infinity");

	/* The character after it stays unread. */
	scan_unget(source, character);

	/* inf and infinity are infinities; a word cut between them is not. */
	if (matched == 3U)
		return 0;
	if (matched == 8U)
		return 0;

	/* Any other spelling does not match. */
	return EINVAL;
}

/* Collects a NaN, nan in any case with maybe its parenthesized characters, that starts with character. */
static int
scan_float_nan(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int character)
{
	size_t matched;
	int belongs;
	int error;

	/* The word nan. */
	matched = scan_word(source, remaining, text, &character, "nan");
	if (matched != 3U) {
		scan_unget(source, character);
		return EINVAL;
	}

	/* Without a parenthesis the NaN is done. */
	if (character != '(') {
		scan_unget(source, character);
		return 0;
	}

	/* The parenthesis and the characters up to the closing one. */
	error = scan_take(source, remaining, text, &character);
	while (error == 0) {
		belongs = scan_is_nan_character(character);
		if (!belongs)
			break;
		error = scan_take(source, remaining, text, &character);
	}

	/* Without its closing parenthesis it is no NaN. */
	if (error == 0 && character != ')')
		error = EINVAL;
	if (error != 0) {
		scan_unget(source, character);
		return error;
	}

	/* The closing parenthesis ends the NaN; nothing after it is read. */
	error = scan_text_add(text, character);
	if (error != 0)
		return error;

	/* Succeeded: the text is a NaN. */
	return 0;
}

/*
 * Collects a decimal or hexadecimal floating number that starts with
 * character: digits with an optional point, then an optional exponent (e
 * for decimal, p for hexadecimal) that must have digits.
 */
static int
scan_float_number(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int character)
{
	size_t digits;
	int exponent;
	int base;
	int error;

	/* Decimal unless it starts 0x. */
	base = 10;
	exponent = 'e';
	digits = 0;
	error = 0;

	/* A leading 0, which may start the 0x of a hexadecimal number. */
	if (character == '0') {
		error = scan_take(source, remaining, text, &character);
		digits = 1;
		if (error == 0 && (character == 'x' || character == 'X')) {
			error = scan_take(source, remaining, text, &character);
			digits = 0;
			base = 16;
			exponent = 'p';
		}
	}

	/* The digits before the point. */
	if (error == 0)
		error = scan_digits(source, remaining, text, &character, base, &digits);

	/* The point and the digits after it. */
	if (error == 0 && character == '.') {
		error = scan_take(source, remaining, text, &character);
		if (error == 0)
			error = scan_digits(source, remaining, text, &character, base, &digits);
	}

	/* Without a digit, or without memory, the number goes no further. */
	if (error == 0 && digits == 0U)
		error = EINVAL;
	if (error != 0) {
		scan_unget(source, character);
		return error;
	}

	/* The optional exponent. */
	error = scan_float_exponent(source, remaining, text, &character, exponent);

	/* The character after the number stays unread. */
	scan_unget(source, character);
	if (error != 0)
		return error;

	/* Succeeded: the text is a number. */
	return 0;
}

/* Collects the optional exponent of a number: its letter, an optional sign and at least one digit. */
static int
scan_float_exponent(
	struct scan_source *source,
	size_t *remaining,
	struct scan_text *text,
	int *character,
	int exponent)
{
	size_t digits;
	int same;
	int error;

	/* No exponent letter: the number has no exponent. */
	same = scan_is_letter(*character, exponent);
	if (!same)
		return 0;

	/* The letter. */
	error = scan_take(source, remaining, text, character);
	if (error != 0)
		return error;

	/* The optional sign. */
	if (*character == '+' || *character == '-') {
		error = scan_take(source, remaining, text, character);
		if (error != 0)
			return error;
	}

	/* The decimal digits. */
	digits = 0;
	error = scan_digits(source, remaining, text, character, 10, &digits);
	if (error != 0)
		return error;

	/* An exponent without a digit does not match. */
	if (digits == 0U)
		return EINVAL;

	/* Succeeded: the exponent is complete. */
	return 0;
}

/* Converts a floating item's text and stores it unless target is NULL; returns 0, or EINVAL. */
static int
scan_float_store(
	const struct scan_text *text,
	enum scan_size size,
	void *target)
{
	long double long_double_value;
	double double_value;
	float float_value;
	char *end;

	/* A text strtod does not read whole does not match. */
	double_value = strtod(text->bytes, &end);
	if (end != text->bytes + text->length)
		return EINVAL;

	/* A suppressed item is only read. */
	if (target == NULL)
		return 0;

	/* Stores the value in the precision of its object. */
	if (size == SCAN_SIZE_LONG_DOUBLE) {
		long_double_value = strtold(text->bytes, NULL);
		*(long double *)target = long_double_value;
	} else if (size == SCAN_SIZE_LONG) {
		*(double *)target = double_value;
	} else {
		float_value = strtof(text->bytes, NULL);
		*(float *)target = float_value;
	}

	/* Succeeded: the value is stored. */
	return 0;
}

/* Reads a %s item: the characters up to white space, NUL-terminated. */
static enum scan_outcome
scan_string(
	struct scan_source *source,
	const struct scan_spec *spec,
	char *target)
{
	size_t remaining;
	size_t count;
	int character;
	int space;

	/* The characters up to white space, the end or the width. */
	remaining = scan_width(spec);
	count = 0;
	character = scan_next(source, &remaining);
	for (;;) {
		if (character == EOF)
			break;
		space = scan_is_space(character);
		if (space)
			break;
		if (target != NULL)
			target[count] = (char)character;
		count++;
		character = scan_next(source, &remaining);
	}

	/* The character after the item stays unread. */
	scan_unget(source, character);

	/* Nothing read: the input ended. */
	if (count == 0U)
		return SCAN_INPUT_FAILURE;

	/* The string's end. */
	if (target != NULL)
		target[count] = '\0';

	/* Succeeded: the word is stored. */
	return SCAN_MATCHED;
}

/* Reads a %c item: exactly the width's characters (one by default), white space included, not terminated. */
static enum scan_outcome
scan_chars(
	struct scan_source *source,
	const struct scan_spec *spec,
	char *target)
{
	size_t wanted;
	size_t index;
	int character;

	/* One character unless the width asks for more. */
	wanted = spec->width;
	if (wanted == 0U)
		wanted = 1;

	/* Each character; the input ending first is an input failure. */
	for (index = 0; index < wanted; index++) {
		character = scan_get(source);
		if (character == EOF)
			return SCAN_INPUT_FAILURE;
		if (target != NULL)
			target[index] = (char)character;
	}

	/* Succeeded: the characters are stored. */
	return SCAN_MATCHED;
}

/* Reads a %[ item: the longest run of the set's characters, NUL-terminated. */
static enum scan_outcome
scan_in_set(
	struct scan_source *source,
	const struct scan_spec *spec,
	const struct scan_set *set,
	char *target)
{
	size_t remaining;
	size_t count;
	int character;

	/* The characters while the set accepts them. */
	remaining = scan_width(spec);
	count = 0;
	character = scan_next(source, &remaining);
	while (character != EOF && set->member[character] != 0) {
		if (target != NULL)
			target[count] = (char)character;
		count++;
		character = scan_next(source, &remaining);
	}

	/* The character after the item stays unread. */
	scan_unget(source, character);

	/* An empty run fails: at the end for want of input, otherwise for want of a match. */
	if (count == 0U) {
		if (character == EOF)
			return SCAN_INPUT_FAILURE;
		return SCAN_MATCHING_FAILURE;
	}

	/* The string's end. */
	if (target != NULL)
		target[count] = '\0';

	/* Succeeded: the run is stored. */
	return SCAN_MATCHED;
}

/* Decodes one multibyte character whose first byte was taken already. */
static enum scan_outcome
scan_wide_one(
	struct scan_source *source,
	int first,
	mbstate_t *state,
	wchar_t *character)
{
	size_t used;
	char byte;
	int next;

	/* Feeds the bytes one by one until they make a character. */
	byte = (char)first;
	for (;;) {
		used = mbrtowc(character, &byte, 1, state);

		/* Not a character of the locale's encoding. */
		if (used == (size_t)-1) {
			errno = EILSEQ;
			return SCAN_INPUT_FAILURE;
		}

		/* The bytes make a character. */
		if (used != (size_t)-2)
			break;

		/* The character goes on into the next byte. */
		next = scan_get(source);
		if (next == EOF) {
			errno = EILSEQ;
			return SCAN_INPUT_FAILURE;
		}

		/* That byte is fed next. */
		byte = (char)next;
	}

	/* Succeeded: *character is the character decoded. */
	return SCAN_MATCHED;
}

/* Reads a %lc item: the width's characters (one by default) as wide characters. */
static enum scan_outcome
scan_wide_chars(
	struct scan_source *source,
	const struct scan_spec *spec,
	wchar_t *target)
{
	enum scan_outcome outcome;
	mbstate_t state;
	wchar_t character;
	size_t wanted;
	size_t index;
	int first;

	/* One character unless the width asks for more, decoded from the initial shift state. */
	wanted = spec->width;
	if (wanted == 0U)
		wanted = 1;
	memset(&state, 0, sizeof(state));

	/* Each character; the input ending first is an input failure. */
	for (index = 0; index < wanted; index++) {
		first = scan_get(source);
		if (first == EOF)
			return SCAN_INPUT_FAILURE;
		outcome = scan_wide_one(source, first, &state, &character);
		if (outcome != SCAN_MATCHED)
			return outcome;
		if (target != NULL)
			target[index] = character;
	}

	/* Succeeded: the characters are stored. */
	return SCAN_MATCHED;
}

/*
 * Reads a %ls item (set NULL: up to white space) or a %l[ item (while the
 * set accepts the characters) as wide characters, NUL-terminated.  The
 * width counts characters.  Whether a character belongs is decided on its
 * first byte, so a character that ends the item is given back whole.
 */
static enum scan_outcome
scan_wide_run(
	struct scan_source *source,
	const struct scan_spec *spec,
	const struct scan_set *set,
	wchar_t *target)
{
	enum scan_outcome outcome;
	mbstate_t state;
	wchar_t character;
	size_t remaining;
	size_t count;
	int space;
	int first;

	/* An empty item, decoded from the initial shift state. */
	remaining = scan_width(spec);
	count = 0;
	memset(&state, 0, sizeof(state));

	/* Takes characters while they belong to the item. */
	first = scan_next(source, &remaining);
	while (first != EOF) {
		/* A %ls item ends at white space, a %l[ item at a byte its set lacks. */
		if (set == NULL) {
			space = scan_is_space(first);
			if (space)
				break;
		} else if (set->member[first] == 0) {
			break;
		}

		/* The whole character. */
		outcome = scan_wide_one(source, first, &state, &character);
		if (outcome != SCAN_MATCHED)
			return outcome;
		if (target != NULL)
			target[count] = character;
		count++;
		first = scan_next(source, &remaining);
	}

	/* The byte after the item stays unread. */
	scan_unget(source, first);

	/* An empty run fails: at the end for want of input, otherwise for want of a match. */
	if (count == 0U) {
		if (first == EOF)
			return SCAN_INPUT_FAILURE;
		return SCAN_MATCHING_FAILURE;
	}

	/* The string's end. */
	if (target != NULL)
		target[count] = L'\0';

	/* Succeeded: the run is stored. */
	return SCAN_MATCHED;
}
