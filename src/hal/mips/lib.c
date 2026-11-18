/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS HAL's C runtime: memory helpers, the early console and its
 * formatted output, and the stops on a failed check.
 *
 * The early console writes each character to the board's screen, until
 * the kernel publishes its own console; from then on every character goes
 * there.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "cpu.h"

static void cons_puts(const char *string);
static void put_number(uint64_t value, unsigned base, int width);

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
 * Writes one character to the early console, or to the kernel's console
 * once the kernel has published one.
 */
void
hal_putc(
	int c)
{
	void (*kernel_output)(int character);

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(c);
		return;
	}

	/* The board's screen, which keeps what it is given until it shows. */
	mips_board_putc(c);
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
	/* Every byte in order. */
	cons_puts(s);

	/* Succeeded. */
	return 0;
}

/*
 * Writes formatted output: %c, %s, %u, %x, %p and %%, with an optional
 * zero-padded width, l for a long argument and ll for a 64-bit one.
 */
int
hal_printf(
	const char *format,
	...)
{
	__builtin_va_list arguments;
	const char *string;
	uint64_t value;
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

		/* Steps past the percent sign. */
		format++;

		/* The width, after an optional zero. */
		width = 0;
		if (*format == '0')
			format++;
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}

		/* The length: l for a long, ll for 64 bits. */
		long_argument = 0;
		if (*format == 'l') {
			long_argument = 1;
			format++;
			if (*format == 'l') {
				long_argument = 2;
				format++;
			}
		}

		/* The conversion. */
		conversion = *format;
		if (conversion == '\0')
			break;
		format++;

		/* Writes the argument the conversion names. */
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
		case 'u':
		case 'x':
			/* The argument at its length. */
			if (long_argument == 2) {
				value = __builtin_va_arg(arguments, uint64_t);
			} else if (long_argument == 1) {
				value = __builtin_va_arg(arguments, unsigned long);
			} else {
				value = __builtin_va_arg(arguments, uint32_t);
			}

			/* Decimal or hexadecimal digits. */
			if (conversion == 'u') {
				put_number(value, 10, width);
			} else {
				put_number(value, 16, width);
			}

			break;
		case 'p':
			put_number((uintptr_t)__builtin_va_arg(arguments, void *), 16, 16);
			break;
		case '%':
			hal_putc('%');
			break;
		default:
			hal_putc('?');
			break;
		}
	}

	/* Ends the walk of the arguments. */
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
	mips_halt_forever();
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
	mips_halt_forever();
}

/*
 * Reports that no entropy source is used: the processors this port runs
 * on have no random number register the HAL reads.
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

/* Writes one terminated string through the early console. */
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
