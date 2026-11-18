/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's C runtime: memory helpers, the early console and its
 * formatted output, and the stops on a failed check.
 *
 * The early console writes each character to the firmware's console and
 * to the board's screen, until the kernel publishes its own console;
 * from then on every character goes there.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cons.h"
#include "sbi.h"

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

	/* The firmware's console, which ends its lines itself. */
	riscv64_sbi_putchar(c);

	/* And the early console, which draws on the board's screen once it has one. */
	riscv64_cons_putc(c);
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
	int character;
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

		/* Skips the optional zero; every width pads with zeros. */
		if (*format == '0')
			format++;

		/* Reads the width's digits. */
		width = 0;
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (*format - '0');
			format++;
		}

		/* A lowercase l asks for a 64-bit argument. */
		long_argument = 0;
		if (*format == 'l') {
			long_argument = 1;
			format++;

			/* A second l asks for the same. */
			if (*format == 'l')
				format++;
		}

		/* A format that ends inside a conversion ends the output. */
		conversion = *format;
		if (conversion == '\0')
			break;
		format++;

		/* Converts the argument as the conversion character asks. */
		switch (conversion) {
		case 'c':
			/* One character. */
			character = __builtin_va_arg(arguments, int);
			hal_putc(character);
			break;
		case 's':
			/* A string, with a missing one spelled out. */
			string = __builtin_va_arg(arguments, const char *);
			if (string == NULL)
				string = "(null)";

			/* Writes the string. */
			cons_puts(string);
			break;
		case 'u':
		case 'x':
			/* Takes the argument at its width. */
			if (long_argument) {
				value = __builtin_va_arg(arguments, uint64_t);
			} else {
				value = __builtin_va_arg(arguments, uint32_t);
			}

			/* Spells the number in decimal or hexadecimal. */
			if (conversion == 'u') {
				put_number(value, 10, width);
			} else {
				put_number(value, 16, width);
			}

			break;
		case 'p':
			/* A pointer, as sixteen hexadecimal digits. */
			value = (uintptr_t)__builtin_va_arg(arguments, void *);
			put_number(value, 16, 16);
			break;
		case '%':
			/* A percent sign. */
			hal_putc('%');
			break;
		default:
			/* A conversion this printf does not know. */
			hal_putc('?');
			break;
		}
	}

	/* Ends the reading of the arguments. */
	__builtin_va_end(arguments);

	/* Succeeded. */
	return 0;
}

/*
 * Stops the calling hart on a failed assertion.
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
	riscv64_halt_forever();
}

/*
 * Stops the calling hart on a fatal error.
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
	riscv64_halt_forever();
}

/*
 * Reports that no entropy source is used: the processors this port runs
 * on have no seed register the HAL reads.
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
		/* Spells the lowest digit, a letter unless it is below ten. */
		digit = (unsigned)(value % base);
		digits[count] = (char)('a' + digit - 10U);
		if (digit < 10U)
			digits[count] = (char)('0' + digit);

		/* Moves on to the next digit. */
		count++;
		value /= base;
	} while (value != 0);

	/* Pads with zeros up to the width. */
	while (width > count) {
		hal_putc('0');
		width--;
	}

	/* Writes the digits most significant first. */
	while (count != 0) {
		count--;
		hal_putc(digits[count]);
	}
}
