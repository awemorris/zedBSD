/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel C runtime (kcrt).
 *
 * The memory and string functions are the byte loops of src/libc/string.c,
 * rewritten for the kernel.  The formatting engine began as the one in
 * src/kern/klog.c and now serves kern_logf() through kern_vsnprintf().
 *
 * Nothing here takes a lock, allocates, keeps state or calls the HAL, and
 * the object refers to no symbol it does not define.  It must be compiled
 * with -ffreestanding -fno-builtin: otherwise the compiler may turn a byte
 * loop below into a call to memcpy() or memset(), which is this very file.
 *
 * The formatting subset:
 *
 *   conversions  d i u x X c s p %
 *   flags        0, and # (0x or 0X before a nonzero x or X value)
 *   width        a decimal number
 *   length       hh h l ll z (hh and h narrow the value as C does)
 *
 * Any other conversion specification -- a precision, a * width, the - + or
 * space flag, a length on c s or p, the o n e f g a j t L conversions --
 * is not converted: the output shows a percent sign and the conversion
 * character instead.  Its arguments are still consumed as C consumes them,
 * so the arguments after it stay in step.  The floating-point conversions
 * are the exception: the kernel is built without floating-point registers,
 * so a double is never fetched, and no kernel format may use them.
 */

#include <limits.h>
#include <stdint.h>

#include <kern/kcrt.h>

/* The widest field width honoured; a larger width is cut to this. */
#define KCRT_WIDTH_MAX 4096U

/*
 * The machine word the copy and fill loops move at a time.
 *
 * It may alias any object, because the loops move the bytes of objects of
 * every type through it.  The loops use it only at word-aligned addresses,
 * so a processor that traps on an unaligned word access never sees one.
 */
typedef unsigned long kcrt_word __attribute__((__may_alias__));

/* The number of bytes in one kcrt_word. */
#define KCRT_WORD_SIZE ((size_t)sizeof(kcrt_word))

/*
 * The length modifier of one conversion specification.
 */
enum kcrt_length {
	KCRT_LENGTH_NONE = 0,
	KCRT_LENGTH_CHAR,
	KCRT_LENGTH_SHORT,
	KCRT_LENGTH_LONG,
	KCRT_LENGTH_LONG_LONG,
	KCRT_LENGTH_SIZE,
	KCRT_LENGTH_INTMAX,
	KCRT_LENGTH_PTRDIFF,
	KCRT_LENGTH_LONG_DOUBLE
};

/*
 * The destination of one formatting call.
 *
 * length counts every byte the format produces, including the ones that did
 * not fit, because that count is what kern_vsnprintf() reports.  Bytes are
 * stored only while one byte of the buffer remains for the terminator.
 */
struct kcrt_output {
	char *buffer;
	size_t capacity;
	size_t length;
};

/*
 * One parsed conversion specification.
 *
 * unsupported is set by any element outside the subset; such a
 * specification is shown rather than converted, but its arguments are still
 * consumed.
 */
struct kcrt_conversion {
	int zero;
	int alternate;
	int unsupported;
	unsigned width;
	enum kcrt_length length;
	char conversion;
};

static const char *kcrt_parse(const char *format, struct kcrt_conversion *conversion, va_list *arguments);
static void kcrt_convert(struct kcrt_output *output, const struct kcrt_conversion *conversion, va_list *arguments);
static uintmax_t kcrt_fetch_unsigned(enum kcrt_length length, va_list *arguments);
static intmax_t kcrt_fetch_signed(enum kcrt_length length, va_list *arguments);
static void kcrt_emit(struct kcrt_output *output, char character);
static void kcrt_emit_padding(struct kcrt_output *output, char character, unsigned count);
static void kcrt_emit_shown(struct kcrt_output *output, char conversion);
static void kcrt_emit_text(struct kcrt_output *output, const char *text, unsigned width);
static void kcrt_emit_number(struct kcrt_output *output, uintmax_t value, int negative, unsigned base, int upper, const char *prefix, unsigned minimum_digits, unsigned width, int zero);

/*
 * Copies count bytes between ranges that do not overlap.
 */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	unsigned char *to;
	const unsigned char *from;
	int aligned;

	to = destination;
	from = source;

	/* Copies whole words while both addresses are word-aligned. */
	aligned = ((uintptr_t)to | (uintptr_t)from) % KCRT_WORD_SIZE == 0;
	if (aligned) {
		while (count >= KCRT_WORD_SIZE) {
			*(kcrt_word *)to = *(const kcrt_word *)from;
			to += KCRT_WORD_SIZE;
			from += KCRT_WORD_SIZE;
			count -= KCRT_WORD_SIZE;
		}
	}

	/* Copies the remaining bytes in ascending order. */
	while (count != 0) {
		*to = *from;
		to++;
		from++;
		count--;
	}

	/* Reports the destination as memcpy() does. */
	return destination;
}

/*
 * Copies count bytes between ranges that may overlap.
 */
void *
kern_memmove(
	void *destination,
	const void *source,
	size_t count)
{
	unsigned char *to;
	const unsigned char *from;
	int aligned;

	to = destination;
	from = source;

	/*
	 * A word copy is safe in either direction: the word written is never
	 * above the next word read on the way up, nor below it on the way down.
	 */
	aligned = ((uintptr_t)to | (uintptr_t)from) % KCRT_WORD_SIZE == 0;

	/*
	 * Copies upward when the destination starts below the source and
	 * downward when it starts above, so no byte is overwritten before it is
	 * read.  Equal ranges need no copy.
	 */
	if (to < from) {
		/* Copies whole words while both addresses are word-aligned. */
		if (aligned) {
			while (count >= KCRT_WORD_SIZE) {
				*(kcrt_word *)to = *(const kcrt_word *)from;
				to += KCRT_WORD_SIZE;
				from += KCRT_WORD_SIZE;
				count -= KCRT_WORD_SIZE;
			}
		}

		/* Copies the remaining bytes upward. */
		while (count != 0) {
			*to = *from;
			to++;
			from++;
			count--;
		}
	} else if (to > from) {
		to += count;
		from += count;

		/*
		 * Copies the bytes past the last whole word first, so that the
		 * word loop below ends exactly on the word-aligned start.
		 */
		if (aligned) {
			while (count % KCRT_WORD_SIZE != 0) {
				to--;
				from--;
				*to = *from;
				count--;
			}
			while (count != 0) {
				to -= KCRT_WORD_SIZE;
				from -= KCRT_WORD_SIZE;
				*(kcrt_word *)to = *(const kcrt_word *)from;
				count -= KCRT_WORD_SIZE;
			}
		}

		/* Copies the remaining bytes downward. */
		while (count != 0) {
			to--;
			from--;
			*to = *from;
			count--;
		}
	}

	/* Reports the destination as memmove() does. */
	return destination;
}

/*
 * Fills count bytes with one value.
 */
void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	unsigned char *to;
	unsigned char byte;
	kcrt_word pattern;
	size_t index;

	to = destination;
	byte = (unsigned char)value;

	/* Builds one word that holds the byte value in every position. */
	pattern = 0;
	for (index = 0; index < KCRT_WORD_SIZE; index++)
		pattern = (pattern << CHAR_BIT) | byte;

	/* Stores whole words while the address is word-aligned. */
	if ((uintptr_t)to % KCRT_WORD_SIZE == 0) {
		while (count >= KCRT_WORD_SIZE) {
			*(kcrt_word *)to = pattern;
			to += KCRT_WORD_SIZE;
			count -= KCRT_WORD_SIZE;
		}
	}

	/* Stores the byte value into every remaining position. */
	while (count != 0) {
		*to = byte;
		to++;
		count--;
	}

	/* Reports the destination as memset() does. */
	return destination;
}

/*
 * Fills count bytes with one value in a way the compiler may not remove.
 *
 * A fill of memory that is never read again, such as the clearing of a key,
 * is a dead store an optimizer is free to delete.  The barrier after the fill
 * tells the compiler the memory may be read, so the stores stay.
 */
void *
kern_memset_explicit(
	void *destination,
	int value,
	size_t count)
{
	/* Fills the memory. */
	kern_memset(destination, value, count);

	/* Makes the filled memory look used to the compiler. */
	__asm__ __volatile__("" : : "r"(destination) : "memory");

	/* Reports the destination as memset_explicit() does. */
	return destination;
}

/*
 * Compares count bytes of two ranges.
 */
int
kern_memcmp(
	const void *left,
	const void *right,
	size_t count)
{
	const unsigned char *a;
	const unsigned char *b;

	/* Walks the ranges to the first byte that differs. */
	a = left;
	b = right;
	while (count != 0) {
		/* Orders the ranges by the first differing byte. */
		if (*a < *b)
			return -1;
		if (*a > *b)
			return 1;

		a++;
		b++;
		count--;
	}

	/* Reports equal ranges. */
	return 0;
}

/*
 * Finds the first occurrence of a byte value in count bytes.
 */
void *
kern_memchr(
	const void *memory,
	int character,
	size_t count)
{
	const unsigned char *bytes;
	unsigned char wanted;

	/* Walks the range to the first byte that matches. */
	bytes = memory;
	wanted = (unsigned char)character;
	while (count != 0) {
		/* Reports the matching byte. */
		if (*bytes == wanted)
			return (void *)(uintptr_t)bytes;

		bytes++;
		count--;
	}

	/* Reports that the value does not occur. */
	return NULL;
}

/*
 * Measures a string up to its terminator.
 */
size_t
kern_strlen(
	const char *string)
{
	size_t length;

	/* Counts the bytes before the terminator. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Reports the number of bytes before the terminator. */
	return length;
}

/*
 * Measures a string, looking at no more than maximum bytes.
 */
size_t
kern_strnlen(
	const char *string,
	size_t maximum)
{
	size_t length;

	/* Counts the bytes before the terminator or the limit. */
	length = 0;
	while (length < maximum && string[length] != '\0')
		length++;

	/* Reports the length, which is maximum when no terminator was seen. */
	return length;
}

/*
 * Compares two strings.
 */
int
kern_strcmp(
	const char *left,
	const char *right)
{
	const unsigned char *a;
	const unsigned char *b;

	/* Walks both strings while they agree and the left one continues. */
	a = (const unsigned char *)left;
	b = (const unsigned char *)right;
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}

	/* Reports the order of the first differing byte, or zero at the end. */
	return (int)*a - (int)*b;
}

/*
 * Compares two strings, looking at no more than count bytes.
 */
int
kern_strncmp(
	const char *left,
	const char *right,
	size_t count)
{
	const unsigned char *a;
	const unsigned char *b;

	/* Walks both strings while they agree, within the limit. */
	a = (const unsigned char *)left;
	b = (const unsigned char *)right;
	while (count != 0 && *a != '\0' && *a == *b) {
		a++;
		b++;
		count--;
	}

	/* Reports equal strings when the limit was reached. */
	if (count == 0)
		return 0;

	/* Reports the order of the first differing byte, or zero at the end. */
	return (int)*a - (int)*b;
}

/*
 * Copies a string with its terminator.
 */
char *
kern_strcpy(
	char *destination,
	const char *source)
{
	size_t index;

	/* Copies every byte before the terminator. */
	index = 0;
	while (source[index] != '\0') {
		destination[index] = source[index];
		index++;
	}

	/* Terminates the copy. */
	destination[index] = '\0';

	/* Reports the destination as strcpy() does. */
	return destination;
}

/*
 * Copies at most count bytes of a string and fills the rest with zeros.
 *
 * As in C, the copy is not terminated when the source is count bytes or
 * longer.
 */
char *
kern_strncpy(
	char *destination,
	const char *source,
	size_t count)
{
	size_t index;

	/* Copies the source up to its terminator or the limit. */
	index = 0;
	while (index < count && source[index] != '\0') {
		destination[index] = source[index];
		index++;
	}

	/* Fills the rest of the destination with zeros. */
	while (index < count) {
		destination[index] = '\0';
		index++;
	}

	/* Reports the destination as strncpy() does. */
	return destination;
}

/*
 * Appends a string to another one.
 */
char *
kern_strcat(
	char *destination,
	const char *source)
{
	size_t length;

	/* Finds the end of the existing string. */
	length = kern_strlen(destination);

	/* Copies the source, terminator included, after it. */
	kern_strcpy(destination + length, source);

	/* Reports the destination as strcat() does. */
	return destination;
}

/*
 * Finds the first occurrence of a character in a string.
 *
 * The terminator is part of the string, so searching for zero finds it.
 */
char *
kern_strchr(
	const char *string,
	int character)
{
	char wanted;

	/* Walks the string, terminator included, to the first match. */
	wanted = (char)character;
	while (*string != wanted) {
		/* Reports that the character does not occur. */
		if (*string == '\0')
			return NULL;

		string++;
	}

	/* Reports the matching position. */
	return (char *)(uintptr_t)string;
}

/*
 * Finds the last occurrence of a character in a string.
 *
 * The terminator is part of the string, so searching for zero finds it.
 */
char *
kern_strrchr(
	const char *string,
	int character)
{
	const char *found;
	char wanted;

	/* Walks the whole string and remembers the latest match. */
	found = NULL;
	wanted = (char)character;
	while (*string != '\0') {
		/* Remembers this position as the latest match. */
		if (*string == wanted)
			found = string;

		string++;
	}

	/* Reports the terminator itself when zero was searched for. */
	if (wanted == '\0')
		found = string;

	/* Reports the latest match, or none. */
	return (char *)(uintptr_t)found;
}

/*
 * Finds the first occurrence of one string inside another.
 *
 * An empty needle matches at the start of the haystack.
 */
char *
kern_strstr(
	const char *haystack,
	const char *needle)
{
	size_t length;
	int order;

	/* Measures the needle once. */
	length = kern_strlen(needle);

	/* Tries every starting position of the haystack. */
	for (;
	     *haystack != '\0';
	     haystack++) {
		/* Reports a position where the whole needle matches. */
		order = kern_strncmp(haystack, needle, length);
		if (order == 0)
			return (char *)(uintptr_t)haystack;
	}

	/* Reports an empty needle at the terminator of an empty haystack. */
	if (length == 0)
		return (char *)(uintptr_t)haystack;

	/* Reports that the needle does not occur. */
	return NULL;
}

/*
 * Formats into a bounded buffer with the kernel printf subset.
 *
 * A nonzero capacity always leaves the buffer terminated, with at most
 * capacity - 1 bytes of text.  The result is the length the whole text
 * would have had, as with vsnprintf(), or -1 when that length does not fit
 * in an int.  A null format produces the empty text.
 */
int
kern_vsnprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	va_list arguments)
{
	struct kcrt_output output;
	struct kcrt_conversion conversion;
	va_list walk;
	size_t terminator;

	/* Starts with nothing written. */
	output.buffer = buffer;
	output.capacity = capacity;
	output.length = 0;

	/*
	 * Renders every literal byte and conversion in format order.  The
	 * arguments are walked through a copy, whose address the conversion
	 * helpers take.
	 */
	if (format != NULL) {
		va_copy(walk, arguments);

		/* Takes the format one literal byte or one specification at a time. */
		while (*format != '\0') {
			/* Copies a literal byte. */
			if (*format != '%') {
				kcrt_emit(&output, *format);
				format++;
				continue;
			}

			/* Converts one specification. */
			format = kcrt_parse(format + 1, &conversion, &walk);
			kcrt_convert(&output, &conversion, &walk);
		}

		va_end(walk);
	}

	/* Terminates the text inside the buffer. */
	if (capacity != 0) {
		terminator = output.length;
		if (terminator > capacity - 1U)
			terminator = capacity - 1U;
		buffer[terminator] = '\0';
	}

	/* Reports a length an int cannot carry as a failure. */
	if (output.length > (size_t)INT_MAX)
		return -1;

	/* Succeeded: reports the length of the whole text. */
	return (int)output.length;
}

/*
 * Formats into a bounded buffer with the kernel printf subset.
 */
int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* Formats the variable arguments. */
	va_start(arguments, format);
	length = kern_vsnprintf(buffer, capacity, format, arguments);
	va_end(arguments);

	/* Reports a length an int cannot carry as a failure. */
	if (length < 0)
		return length;

	/* Succeeded: reports the length of the whole text. */
	return length;
}

#ifndef KERN_KCRT_NO_STANDARD_NAMES
/*
 * The standard names the compiler may call: for a structure copy or clear,
 * and, once it sees the whole kernel (link-time optimization, ws053), for a
 * copy it cannot prove does not overlap or a comparison it recognizes.  C
 * requires a freestanding program to provide all four.
 *
 * They are the kcrt functions under a second name, so there is one
 * implementation.  A host test of this file leaves them out, since the host
 * C library already defines them.
 */
void *memcpy(void *destination, const void *source, size_t count) __attribute__((alias("kern_memcpy")));
void *memmove(void *destination, const void *source, size_t count) __attribute__((alias("kern_memmove")));
void *memset(void *destination, int value, size_t count) __attribute__((alias("kern_memset")));
int memcmp(const void *left, const void *right, size_t count) __attribute__((alias("kern_memcmp")));
#endif

/* Parses the conversion specification after a percent sign. */
static const char *
kcrt_parse(
	const char *format,
	struct kcrt_conversion *conversion,
	va_list *arguments)
{
	unsigned digit;

	/* Starts from a plain specification. */
	conversion->zero = 0;
	conversion->alternate = 0;
	conversion->unsupported = 0;
	conversion->width = 0;
	conversion->length = KCRT_LENGTH_NONE;
	conversion->conversion = '\0';

	/* Reads the flags; only 0 and # are in the subset. */
	while (*format == '0' ||
	       *format == '#' ||
	       *format == '-' ||
	       *format == '+' ||
	       *format == ' ') {
		if (*format == '0') {
			conversion->zero = 1;
		} else if (*format == '#') {
			conversion->alternate = 1;
		} else {
			conversion->unsupported = 1;
		}

		format++;
	}

	/*
	 * Reads the field width.  A * width takes an int argument, which is
	 * consumed although the specification will not be converted.
	 */
	if (*format == '*') {
		(void)va_arg(*arguments, int);
		conversion->unsupported = 1;
		format++;
	} else {
		while (*format >= '0' && *format <= '9') {
			digit = (unsigned)(*format - '0');
			if (conversion->width <= KCRT_WIDTH_MAX)
				conversion->width = conversion->width * 10U + digit;
			format++;
		}

		if (conversion->width > KCRT_WIDTH_MAX)
			conversion->width = KCRT_WIDTH_MAX;
	}

	/*
	 * Reads a precision, which is outside the subset.  A * precision takes
	 * an int argument, which is consumed.
	 */
	if (*format == '.') {
		conversion->unsupported = 1;
		format++;
		if (*format == '*') {
			(void)va_arg(*arguments, int);
			format++;
		} else {
			while (*format >= '0' && *format <= '9')
				format++;
		}
	}

	/* Reads the length modifier. */
	if (*format == 'h') {
		format++;
		conversion->length = KCRT_LENGTH_SHORT;
		if (*format == 'h') {
			format++;
			conversion->length = KCRT_LENGTH_CHAR;
		}
	} else if (*format == 'l') {
		format++;
		conversion->length = KCRT_LENGTH_LONG;
		if (*format == 'l') {
			format++;
			conversion->length = KCRT_LENGTH_LONG_LONG;
		}
	} else if (*format == 'z') {
		format++;
		conversion->length = KCRT_LENGTH_SIZE;
	} else if (*format == 'j') {
		format++;
		conversion->length = KCRT_LENGTH_INTMAX;
		conversion->unsupported = 1;
	} else if (*format == 't') {
		format++;
		conversion->length = KCRT_LENGTH_PTRDIFF;
		conversion->unsupported = 1;
	} else if (*format == 'L') {
		format++;
		conversion->length = KCRT_LENGTH_LONG_DOUBLE;
		conversion->unsupported = 1;
	}

	/* Takes the conversion character, which is absent at the end of the format. */
	if (*format != '\0') {
		conversion->conversion = *format;
		format++;
	}

	/* Reports where the literal text resumes. */
	return format;
}

/* Consumes the arguments of one specification and renders it. */
static void
kcrt_convert(
	struct kcrt_output *output,
	const struct kcrt_conversion *conversion,
	va_list *arguments)
{
	const char *prefix;
	const char *text;
	uintmax_t magnitude;
	intmax_t signed_value;
	uintptr_t pointer;
	unsigned base;
	int negative;
	int upper;
	char character;

	/* Converts, or shows, the specification by its conversion character. */
	switch (conversion->conversion) {
	case '%':
		/* Shows an escaped percent sign. */
		kcrt_emit(output, '%');
		break;

	case 'd':
	case 'i':
		/* Consumes the signed argument. */
		signed_value = kcrt_fetch_signed(conversion->length, arguments);

		/* Shows a specification outside the subset. */
		if (conversion->unsupported) {
			kcrt_emit_shown(output, conversion->conversion);
			break;
		}

		/* Splits the value into its sign and magnitude. */
		negative = 0;
		magnitude = (uintmax_t)signed_value;
		if (signed_value < 0) {
			negative = 1;
			magnitude = (uintmax_t)0 - magnitude;
		}

		/* Shows the value in decimal. */
		kcrt_emit_number(output, magnitude, negative, 10U, 0, "", 1U, conversion->width, conversion->zero);
		break;

	case 'u':
	case 'x':
	case 'X':
	case 'o':
		/* Consumes the unsigned argument. */
		magnitude = kcrt_fetch_unsigned(conversion->length, arguments);

		/* Shows a specification outside the subset; o is always outside. */
		if (conversion->unsupported || conversion->conversion == 'o') {
			kcrt_emit_shown(output, conversion->conversion);
			break;
		}

		/* Chooses the base, the digit case and the # prefix. */
		base = 16U;
		upper = 0;
		prefix = "";
		if (conversion->conversion == 'u') {
			base = 10U;
		} else if (conversion->conversion == 'X') {
			upper = 1;
			if (conversion->alternate && magnitude != 0)
				prefix = "0X";
		} else {
			if (conversion->alternate && magnitude != 0)
				prefix = "0x";
		}

		/* Shows the value in the chosen base. */
		kcrt_emit_number(output, magnitude, 0, base, upper, prefix, 1U, conversion->width, conversion->zero);
		break;

	case 'c':
		/* Consumes the character, which arrives promoted to int. */
		character = (char)va_arg(*arguments, int);

		/* Shows a specification outside the subset, a wide character included. */
		if (conversion->unsupported || conversion->length != KCRT_LENGTH_NONE) {
			kcrt_emit_shown(output, conversion->conversion);
			break;
		}

		/* Pads the character to the field width. */
		if (conversion->width > 1U)
			kcrt_emit_padding(output, ' ', conversion->width - 1U);

		/* Shows the character. */
		kcrt_emit(output, character);
		break;

	case 's':
		/* Consumes the string pointer. */
		text = va_arg(*arguments, const char *);

		/* Shows a specification outside the subset, a wide string included. */
		if (conversion->unsupported || conversion->length != KCRT_LENGTH_NONE) {
			kcrt_emit_shown(output, conversion->conversion);
			break;
		}

		/* Shows a null string as a readable marker. */
		if (text == NULL)
			text = "(null)";

		/* Shows the string padded to the field width. */
		kcrt_emit_text(output, text, conversion->width);
		break;

	case 'p':
		/* Consumes the pointer. */
		pointer = (uintptr_t)va_arg(*arguments, void *);

		/* Shows a specification outside the subset. */
		if (conversion->unsupported || conversion->length != KCRT_LENGTH_NONE) {
			kcrt_emit_shown(output, conversion->conversion);
			break;
		}

		/* Shows every digit of the pointer after 0x. */
		kcrt_emit_number(output, pointer, 0, 16U, 0, "0x", (unsigned)(sizeof(uintptr_t) * 2U), conversion->width, 0);
		break;

	case 'n':
		/* Consumes the pointer, but never stores through it. */
		(void)va_arg(*arguments, void *);

		/* Shows the specification. */
		kcrt_emit_shown(output, conversion->conversion);
		break;

	case '\0':
		/* A percent sign at the very end of the format stands alone. */
		kcrt_emit(output, '%');
		break;

	default:
		/*
		 * A floating-point or unknown conversion.  A double is never
		 * fetched: the kernel has no floating-point registers to take it.
		 * Shows the specification.
		 */
		kcrt_emit_shown(output, conversion->conversion);
		break;
	}
}

/* Fetches an unsigned integer argument of the given length. */
static uintmax_t
kcrt_fetch_unsigned(
	enum kcrt_length length,
	va_list *arguments)
{
	uintmax_t value;

	/* Fetches the argument with the type its length modifier names. */
	switch (length) {
	case KCRT_LENGTH_CHAR:
		value = (unsigned char)va_arg(*arguments, unsigned int);
		break;

	case KCRT_LENGTH_SHORT:
		value = (unsigned short)va_arg(*arguments, unsigned int);
		break;

	case KCRT_LENGTH_LONG:
		value = va_arg(*arguments, unsigned long);
		break;

	case KCRT_LENGTH_LONG_LONG:
	case KCRT_LENGTH_LONG_DOUBLE:
		value = va_arg(*arguments, unsigned long long);
		break;

	case KCRT_LENGTH_SIZE:
		value = va_arg(*arguments, size_t);
		break;

	case KCRT_LENGTH_INTMAX:
		value = va_arg(*arguments, uintmax_t);
		break;

	case KCRT_LENGTH_PTRDIFF:
		value = (uintmax_t)va_arg(*arguments, ptrdiff_t);
		break;

	default:
		value = va_arg(*arguments, unsigned int);
		break;
	}

	/* Reports the value widened to the largest unsigned type. */
	return value;
}

/* Fetches a signed integer argument of the given length. */
static intmax_t
kcrt_fetch_signed(
	enum kcrt_length length,
	va_list *arguments)
{
	intmax_t value;

	/* Fetches the argument with the type its length modifier names. */
	switch (length) {
	case KCRT_LENGTH_CHAR:
		value = (signed char)va_arg(*arguments, int);
		break;

	case KCRT_LENGTH_SHORT:
		value = (short)va_arg(*arguments, int);
		break;

	case KCRT_LENGTH_LONG:
		value = va_arg(*arguments, long);
		break;

	case KCRT_LENGTH_LONG_LONG:
	case KCRT_LENGTH_LONG_DOUBLE:
		value = va_arg(*arguments, long long);
		break;

	case KCRT_LENGTH_SIZE:
		/* The signed type of size_t's width, which is ptrdiff_t's here. */
		value = (intmax_t)(ptrdiff_t)va_arg(*arguments, size_t);
		break;

	case KCRT_LENGTH_INTMAX:
		value = va_arg(*arguments, intmax_t);
		break;

	case KCRT_LENGTH_PTRDIFF:
		value = va_arg(*arguments, ptrdiff_t);
		break;

	default:
		value = va_arg(*arguments, int);
		break;
	}

	/* Reports the value widened to the largest signed type. */
	return value;
}

/* Appends one byte, storing it only while it and a terminator fit. */
static void
kcrt_emit(
	struct kcrt_output *output,
	char character)
{
	/* Stores the byte when a byte for the terminator remains after it. */
	if (output->capacity != 0 && output->length < output->capacity - 1U)
		output->buffer[output->length] = character;

	/* Counts the byte whether or not it was stored. */
	output->length++;
}

/* Appends count copies of one padding byte. */
static void
kcrt_emit_padding(
	struct kcrt_output *output,
	char character,
	unsigned count)
{
	/* Appends the padding bytes one at a time. */
	while (count != 0) {
		kcrt_emit(output, character);
		count--;
	}
}

/* Shows a specification outside the subset as a percent sign and its character. */
static void
kcrt_emit_shown(
	struct kcrt_output *output,
	char conversion)
{
	/* Shows the percent sign. */
	kcrt_emit(output, '%');

	/* Shows the conversion character, which is absent at the end of the format. */
	if (conversion != '\0')
		kcrt_emit(output, conversion);
}

/* Appends a string, padded on the left to the field width. */
static void
kcrt_emit_text(
	struct kcrt_output *output,
	const char *text,
	unsigned width)
{
	size_t length;

	/* Pads the string to the field width. */
	length = kern_strlen(text);
	if (length < width)
		kcrt_emit_padding(output, ' ', (unsigned)(width - length));

	/* Appends every byte of the string. */
	while (*text != '\0') {
		kcrt_emit(output, *text);
		text++;
	}
}

/* Appends a number with its sign, prefix and padding. */
static void
kcrt_emit_number(
	struct kcrt_output *output,
	uintmax_t value,
	int negative,
	unsigned base,
	int upper,
	const char *prefix,
	unsigned minimum_digits,
	unsigned width,
	int zero)
{
	char digits[sizeof(uintmax_t) * 3U];
	const char *alphabet;
	unsigned count;
	unsigned length;

	/* Selects the digit alphabet. */
	alphabet = "0123456789abcdef";
	if (upper)
		alphabet = "0123456789ABCDEF";

	/* Builds the digits in reverse order. */
	count = 0;
	do {
		digits[count] = alphabet[value % base];
		count++;
		value /= base;
	} while (value != 0 && count < sizeof(digits));

	/* Extends the digits with zeros to the minimum count. */
	while (count < minimum_digits && count < sizeof(digits)) {
		digits[count] = '0';
		count++;
	}

	/* Measures the whole field: the sign, the prefix and the digits. */
	length = count + (unsigned)kern_strlen(prefix);
	if (negative)
		length++;

	/*
	 * Pads the field.  Zero padding goes between the sign and prefix and the
	 * digits; space padding goes before everything.
	 */
	if (!zero && length < width)
		kcrt_emit_padding(output, ' ', width - length);

	/* Shows the sign of a negative value. */
	if (negative)
		kcrt_emit(output, '-');

	/* Shows the prefix. */
	while (*prefix != '\0') {
		kcrt_emit(output, *prefix);
		prefix++;
	}

	/* Pads with zeros inside the field. */
	if (zero && length < width)
		kcrt_emit_padding(output, '0', width - length);

	/* Shows the digits in display order. */
	while (count != 0) {
		count--;
		kcrt_emit(output, digits[count]);
	}
}
