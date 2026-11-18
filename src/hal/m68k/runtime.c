/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68030 HAL's C runtime: memory helpers, formatted output and the
 * stops on a failed check.  The barriers and the cache maintenance are in
 * cache.c.
 */

#include <hal/hal.h>
#include "defs.h"

static void cons_puts(const char *string);
static void put_number(uint64_t value, unsigned base, int width);
static void halt_forever(void) __attribute__((noreturn));

/*
 * Reports the length of a terminated string.
 */
int
hal_strlen(
	const char *s)
{
	int length;

	/* Counts up to the terminator. */
	length = 0;
	while (s[length] != '\0')
		length++;

	/* Succeeded: the length. */
	return length;
}

/*
 * Fills bytes with a value.
 */
void *
hal_memset(
	void *s,
	int c,
	size_t n)
{
	uint8_t *destination;
	size_t index;

	/* Stores the byte n times. */
	destination = s;
	for (index = 0; index < n; index++)
		destination[index] = (uint8_t)c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Fills 16-bit words with a value.
 */
void *
hal_memset16(
	uint16_t *s,
	uint16_t c,
	size_t n)
{
	size_t index;

	/* Stores the word n times. */
	for (index = 0; index < n; index++)
		s[index] = c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Fills 32-bit words with a value.
 */
void *
hal_memset32(
	uint32_t *s,
	uint32_t c,
	size_t n)
{
	size_t index;

	/* Stores the word n times. */
	for (index = 0; index < n; index++)
		s[index] = c;

	/* Succeeded: the destination. */
	return s;
}

/*
 * Copies bytes between areas that do not overlap.
 */
void *
hal_memcpy(
	void *dest,
	const void *src,
	size_t n)
{
	uint8_t *destination;
	const uint8_t *source;
	size_t index;

	/* Copies byte by byte, from the start. */
	destination = dest;
	source = src;
	for (index = 0; index < n; index++)
		destination[index] = source[index];

	/* Succeeded: the destination. */
	return dest;
}

/*
 * Writes one character, as the C library's putchar does.
 */
int
hal_putchar(
	int c)
{
	/* The early console takes it. */
	hal_putc(c);

	/* Succeeded: the character written. */
	return c;
}

/*
 * Writes a terminated string.
 */
int
hal_puts(
	const char *s)
{
	/* Writes every byte in order. */
	cons_puts(s);

	/* Succeeded. */
	return 0;
}

/*
 * Writes formatted output: %c, %s, %d, %u, %x, %p and %%, with an optional
 * zero-padded width and l or ll for a 64-bit argument.
 */
int
hal_printf(
	const char *format,
	...)
{
	__builtin_va_list arguments;
	const char *string;
	uint64_t value;
	int64_t signed_value;
	int width;
	int long_argument;
	char conversion;

	/* Walks the format, writing literal characters and converting the rest. */
	__builtin_va_start(arguments, format);
	while (*format != '\0') {
		/* A literal character. */
		if (*format != '%') {
			hal_putc(*format);
			format++;
			continue;
		}
		format++;

		/* Reads the width, after an optional zero, and the length. */
		width = 0;
		long_argument = 0;
		if (*format == '0')
			format++;
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}
		if (*format == 'l') {
			long_argument = 1;
			format++;
			if (*format == 'l')
				format++;
		}

		/* A format that ends inside a conversion ends the output. */
		conversion = *format;
		if (conversion == '\0')
			break;
		format++;

		/* Converts one argument. */
		switch (conversion) {
		case 'c':
			hal_putc(__builtin_va_arg(arguments, int));
			break;
		case 's':
			string = __builtin_va_arg(arguments, const char *);
			if (string == NULL)
				string = "(null)";
			cons_puts(string);
			break;
		case 'd':
			if (long_argument) {
				signed_value = __builtin_va_arg(arguments, int64_t);
			} else {
				signed_value = __builtin_va_arg(arguments, int32_t);
			}

			/* A negative number is its sign and its magnitude. */
			if (signed_value < 0) {
				hal_putc('-');
				value = (uint64_t)(-(signed_value + 1)) + 1U;
			} else {
				value = (uint64_t)signed_value;
			}
			put_number(value, 10, width);
			break;
		case 'u':
		case 'x':
			if (long_argument) {
				value = __builtin_va_arg(arguments, uint64_t);
			} else {
				value = __builtin_va_arg(arguments, uint32_t);
			}

			/* Writes the number in the base the conversion names. */
			if (conversion == 'u') {
				put_number(value, 10, width);
			} else {
				put_number(value, 16, width);
			}
			break;
		case 'p':
			put_number((uintptr_t)__builtin_va_arg(arguments, void *), 16, 8);
			break;
		case '%':
			hal_putc('%');
			break;
		default:
			hal_putc('?');
			break;
		}
	}
	__builtin_va_end(arguments);

	/* Succeeded. */
	return 0;
}

/*
 * Stops the processor on a failed assertion.
 */
void
hal_assert(
	const char *file,
	int line,
	const char *exp)
{
	/* Prints the failed assertion. */
	hal_printf("assert: %s:%u: %s\n", file, (uint32_t)line, exp);

	/* Halts with interrupts off for good. */
	halt_forever();
}

/*
 * Stops the processor on a fatal error.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *s)
{
	/* Prints the site and the message. */
	hal_printf("fatal: %s:%u: %s\n", file, (uint32_t)line, s);

	/* Halts with interrupts off for good. */
	halt_forever();
}

/*
 * Waits for the next interrupt with interrupts enabled.
 */
void
hal_halt(
	void)
{
	/* STOP enables every level and waits for one. */
	__asm__ volatile("stop #0x2000" : : : "memory");
}

/*
 * Stops the processor after a kernel panic.
 */
void
hal_panic(
	void)
{
	/* The panic was already reported. */
	halt_forever();
}

/*
 * Reports that no entropy source is used: the X68000 has no random number
 * source the HAL reads.
 */
bool
hal_entropy_fill(
	void *buffer,
	size_t size)
{
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);

	/* Failed: no source. */
	return false;
}

/* Waits forever with every interrupt level masked. */
static void
halt_forever(
	void)
{
	/* STOP with the mask at 7 waits for nothing but a reset. */
	for (;;)
		__asm__ volatile("stop #0x2700" : : : "memory");
}

/* Writes one terminated string through the console. */
static void
cons_puts(
	const char *string)
{
	/* Ignores a missing string. */
	if (string == NULL)
		return;

	/* Emits every byte in order. */
	while (*string != '\0') {
		hal_putc((unsigned char)*string);
		string++;
	}
}

/* Writes a number in a base, zero-padded to a width. */
static void
put_number(
	uint64_t value,
	unsigned base,
	int width)
{
	char digits[24];
	unsigned digit;
	int count;

	/* Collects the digits, least significant first. */
	count = 0;
	do {
		digit = (unsigned)(value % base);
		digits[count] = (char)('a' + digit - 10U);
		if (digit < 10U)
			digits[count] = (char)('0' + digit);
		count++;
		value /= base;
	} while (value != 0);

	/* Pads, then writes the digits most significant first. */
	while (width > count) {
		hal_putc('0');
		width--;
	}
	while (count != 0) {
		count--;
		hal_putc(digits[count]);
	}
}
