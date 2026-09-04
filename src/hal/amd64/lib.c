/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The self-contained C runtime subset used by the amd64 HAL.
 *
 * The HAL owns no heap: hal_malloc uses an allocator supplied by the
 * embedding kernel. Console output passes through the BSP console.
 * hal_printf implements the conversions used by the HAL sources.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "smp.h"

static void *(*allocator_alloc)(size_t size);
static void (*allocator_free)(void *pointer);
static volatile unsigned panic_in_progress;

static void put_unsigned(uint64_t value, unsigned base, int upper, int width, int zero);

/*
 * Measures a terminated byte string.
 */
int
hal_strlen(
	const char *string)
{
	int length;

	/* Counts every byte before the terminator. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Returns the measured byte count. */
	return length;
}

/*
 * Fills a byte range with one value.
 */
void *
hal_memset(
	void *destination,
	int value,
	size_t size)
{
	uint8_t *byte;

	/* Fills the requested bytes in ascending address order. */
	byte = destination;
	while (size-- != 0)
		*byte++ = (uint8_t)value;

	/* Returns the original destination. */
	return destination;
}

/*
 * Fills a 16-bit range with one value.
 */
void *
hal_memset16(
	uint16_t *destination,
	uint16_t value,
	size_t count)
{
	uint16_t *element;

	/* Fills the requested elements in ascending address order. */
	element = destination;
	while (count-- != 0)
		*element++ = value;

	/* Returns the original destination. */
	return destination;
}

/*
 * Fills a 32-bit range with one value.
 */
void *
hal_memset32(
	uint32_t *destination,
	uint32_t value,
	size_t count)
{
	uint32_t *element;

	/* Fills the requested elements in ascending address order. */
	element = destination;
	while (count-- != 0)
		*element++ = value;

	/* Returns the original destination. */
	return destination;
}

/*
 * Copies a byte range without overlap handling.
 */
void *
hal_memcpy(
	void *destination,
	const void *source,
	size_t size)
{
	uint8_t *destination_byte;
	const uint8_t *source_byte;

	/* Copies the requested bytes in ascending address order. */
	destination_byte = destination;
	source_byte = source;
	while (size-- != 0)
		*destination_byte++ = *source_byte++;

	/* Returns the original destination. */
	return destination;
}

/*
 * Installs the kernel allocator callbacks.
 */
void
hal_set_allocator(
	void *(*allocate)(size_t size),
	void (*free_function)(void *pointer))
{
	/* Requires one complete allocator installation. */
	if (allocate == NULL ||
	    free_function == NULL ||
	    allocator_alloc != NULL ||
	    allocator_free != NULL)
		HAL_FATAL("hal_set_allocator must be called exactly once");

	/* Publishes the paired allocation callbacks. */
	allocator_alloc = allocate;
	allocator_free = free_function;
}

/*
 * Allocates kernel-owned memory through the installed allocator.
 */
void *
hal_malloc(
	size_t size)
{
	void *result;

	/* Rejects allocation before the kernel provides an allocator. */
	if (allocator_alloc == NULL)
		HAL_FATAL("hal_malloc before hal_set_allocator");

	/* Allocates the requested memory. */
	result = allocator_alloc(size);

	/* Returns the allocator result unchanged. */
	return result;
}

/*
 * Releases kernel-owned memory through the installed allocator.
 */
void
hal_free(
	void *pointer)
{
	/* Rejects release before the kernel provides an allocator. */
	if (allocator_free == NULL)
		HAL_FATAL("hal_free before hal_set_allocator");

	/* Releases the supplied allocation. */
	allocator_free(pointer);
}

/*
 * Writes one character to the HAL console.
 */
int
hal_putchar(
	int character)
{
	/* Writes the character through the console backend. */
	hal_cons_putc(character);

	/* Returns the written character. */
	return character;
}

/*
 * Writes one terminated string to the HAL console.
 */
int
hal_puts(
	const char *string)
{
	/* Writes the string without adding a terminator or newline. */
	hal_cons_write(string);

	/* Reports successful output. */
	return 0;
}

/*
 * Formats the HAL-supported conversions on the BSP console.
 */
int
hal_printf(
	const char *format,
	...)
{
	__builtin_va_list arguments;
	const char *position;
	const char *string;
	uint64_t output_token;
	uint64_t unsigned_value;
	int64_t signed_value;
	int character;
	int longs;
	int zero;
	int width;

	/* Serializes this complete formatted console record. */
	output_token = pcat_cons_output_begin();
	__builtin_va_start(arguments, format);

	/* Emits every literal byte or conversion in format order. */
	for (position = format; *position != '\0'; position++) {
		longs = 0;
		zero = 0;
		width = 0;

		/* Emits literal bytes without conversion processing. */
		if (*position != '%') {
			hal_cons_putc(*position);
			continue;
		}

		/* Consumes the conversion marker and optional zero flag. */
		position++;
		if (*position == '0') {
			zero = 1;
			position++;
		}

		/* Decodes the optional decimal field width. */
		while (*position >= '0' && *position <= '9') {
			width = width * 10 + (*position - '0');
			position++;
		}

		/* Counts the optional l and ll length modifiers. */
		while (*position == 'l' && longs < 2) {
			longs++;
			position++;
		}

		/* Emits the selected supported conversion. */
		switch (*position) {
		case 'c':
			character = __builtin_va_arg(arguments, int);
			hal_cons_putc(character);
			break;
		case 's':
			string = __builtin_va_arg(arguments, const char *);

			/* Substitutes a readable value for a null string. */
			if (string == NULL)
				string = "(null)";
			hal_cons_write(string);
			break;
		case 'd':
			/* Consumes the argument at its promoted width. */
			if (longs == 0)
				signed_value = __builtin_va_arg(arguments, int);
			else if (longs == 1)
				signed_value = __builtin_va_arg(arguments, long);
			else
				signed_value = __builtin_va_arg(arguments,
				    long long);

			/* Emits the sign before formatting the magnitude. */
			unsigned_value = (uint64_t)signed_value;
			if (signed_value < 0) {
				hal_cons_putc('-');
				unsigned_value = 0U - unsigned_value;
			}
			put_unsigned(unsigned_value, 10, 0, width, zero);
			break;
		case 'u':
		case 'x':
		case 'X':
			/* Consumes the argument at its promoted width. */
			if (longs == 0)
				unsigned_value = __builtin_va_arg(arguments,
				    uint32_t);
			else if (longs == 1)
				unsigned_value = __builtin_va_arg(arguments,
				    unsigned long);
			else
				unsigned_value = __builtin_va_arg(arguments,
				    unsigned long long);
			put_unsigned(
				unsigned_value,
				*position == 'u' ? 10 : 16,
				*position == 'X',
				width,
				zero);
			break;
		case '%':
			hal_cons_putc('%');
			break;
		default:
			hal_cons_putc('%');

			/* Restores any consumed length modifiers verbatim. */
			while (longs-- > 0)
				hal_cons_putc('l');

			/* Preserves an unknown conversion or trailing marker. */
			if (*position != '\0')
				hal_cons_putc(*position);
			else
				position--;
			break;
		}
	}

	/* Releases the argument list and serialized output record. */
	__builtin_va_end(arguments);
	pcat_cons_output_end(output_token);

	/* Reports successful formatting. */
	return 0;
}

/*
 * Reports an assertion failure and stops the CPU.
 */
void
hal_assert(
	const char *file,
	int line,
	const char *expression)
{
	/* Parks immediately when another CPU already owns panic output. */
	if (__atomic_exchange_n(
	    &panic_in_progress,
	    1U,
	    __ATOMIC_ACQ_REL) != 0) {
		asm_cli();

		/* Halts this CPU permanently. */
		for (;;)
			asm_hlt();
	}

	/* Reports the assertion through the serialized console path. */
	hal_printf("\nassert: %s:%d: %s\n", file, line, expression);

	/* Stops other CPUs only after SMP panic broadcast becomes safe. */
	if (amd64_smp_panic_available())
		hal_cpu_panic_all();

	/* Halts the panic owner permanently with interrupts disabled. */
	asm_cli();
	for (;;)
		asm_hlt();
}

/*
 * Reports a fatal HAL condition and stops the CPU.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *message)
{
	/* Parks immediately when another CPU already owns panic output. */
	if (__atomic_exchange_n(
	    &panic_in_progress,
	    1U,
	    __ATOMIC_ACQ_REL) != 0) {
		asm_cli();

		/* Halts this CPU permanently. */
		for (;;)
			asm_hlt();
	}

	/* Reports the fatal condition through the serialized console path. */
	hal_printf("\nfatal: %s:%d: %s\n", file, line, message);

	/* Stops other CPUs only after SMP panic broadcast becomes safe. */
	if (amd64_smp_panic_available())
		hal_cpu_panic_all();

	/* Halts the panic owner permanently with interrupts disabled. */
	asm_cli();
	for (;;)
		asm_hlt();
}

/*
 * Fills a buffer from the processor random-number instruction.
 */
bool
hal_entropy_fill(
	void *buffer,
	size_t size)
{
	uint8_t *bytes;
	uint64_t value;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	unsigned attempt;
	unsigned char ready;
	size_t chunk;

	/* Queries processor support for the RDRAND instruction. */
	bytes = buffer;
	eax = 1;
	__asm__ volatile("cpuid"
	    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	(void)ebx;
	(void)edx;

	/* Rejects processors without an architectural random source. */
	if ((ecx & (1U << 30)) == 0U)
		return false;

	/* Fills the buffer from bounded successful RDRAND samples. */
	while (size != 0U) {
		value = 0;
		ready = 0;

		/* Retries a temporarily unavailable hardware sample. */
		for (attempt = 0; attempt < 10U && ready == 0U; attempt++) {
			__asm__ volatile("rdrand %0; setc %1"
			    : "=r"(value), "=qm"(ready));
		}

		/* Reports failure after the bounded retry window. */
		if (ready == 0U)
			return false;

		/* Copies the available part of this machine-word sample. */
		if (size < sizeof(value))
			chunk = size;
		else
			chunk = sizeof(value);
		hal_memcpy(bytes, &value, chunk);
		bytes += chunk;
		size -= chunk;
	}

	/* Reports a completely filled buffer. */
	return true;
}

/* Emits an unsigned value with the requested base and padding. */
static void
put_unsigned(
	uint64_t value,
	unsigned base,
	int upper,
	int width,
	int zero)
{
	char digits[24];
	unsigned digit;
	int length;

	/* Builds the value in reverse order. */
	length = 0;
	do {
		digit = (unsigned)(value % base);

		/* Selects the requested alphabet for this digit. */
		if (digit < 10)
			digits[length++] = (char)('0' + digit);
		else if (upper)
			digits[length++] = (char)('A' + digit - 10);
		else
			digits[length++] = (char)('a' + digit - 10);
		value /= base;
	} while (value != 0);

	/* Pads the output to the requested field width. */
	while (width > length) {
		/* Selects zero or space padding for this output position. */
		if (zero)
			hal_cons_putc('0');
		else
			hal_cons_putc(' ');
		width--;
	}

	/* Emits the accumulated digits in display order. */
	while (length > 0)
		hal_cons_putc(digits[--length]);
}
