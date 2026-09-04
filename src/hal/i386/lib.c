/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The self-contained i386 HAL C runtime implementation.
 *
 * The HAL owns no heap.  Its allocator is supplied by the kernel, console
 * output is supplied by the BSP, and formatted output implements only the
 * conversions used within the HAL.
 */

#include <hal/hal.h>

#include "asm.h"

static void *(*allocator_alloc)(size_t size);
static void (*allocator_free)(void *p);

static void put_unsigned(uint64_t value, unsigned base, int upper, int width, int zero);

/*
 * Counts bytes in a terminated string.
 */
int
hal_strlen(
	const char *s)
{
	int n;

	/* Scans through the first terminating byte. */
	n = 0;
	while (s[n] != '\0')
		n++;

	/* Returns the number of non-terminating bytes. */
	return n;
}

/*
 * Fills a byte-oriented memory range.
 */
void *
hal_memset(
	void *s,
	int c,
	size_t n)
{
	uint8_t *p;

	/* Fills every requested byte with the converted value. */
	p = s;
	while (n != 0U) {
		*p = (uint8_t)c;
		p++;
		n--;
	}

	/* Returns the original destination pointer. */
	return s;
}

/*
 * Fills a 16-bit memory range.
 */
void *
hal_memset16(
	uint16_t *s,
	uint16_t c,
	size_t n)
{
	uint16_t *p;

	/* Fills every requested 16-bit element. */
	p = s;
	while (n != 0U) {
		*p = c;
		p++;
		n--;
	}

	/* Returns the original destination pointer. */
	return s;
}

/*
 * Fills a 32-bit memory range.
 */
void *
hal_memset32(
	uint32_t *s,
	uint32_t c,
	size_t n)
{
	uint32_t *p;

	/* Fills every requested 32-bit element. */
	p = s;
	while (n != 0U) {
		*p = c;
		p++;
		n--;
	}

	/* Returns the original destination pointer. */
	return s;
}

/*
 * Copies a non-overlapping byte range.
 */
void *
hal_memcpy(
	void *dest,
	const void *src,
	size_t n)
{
	uint8_t *destination;
	const uint8_t *source;

	/* Copies every requested byte in increasing address order. */
	destination = dest;
	source = src;
	while (n != 0U) {
		*destination = *source;
		destination++;
		source++;
		n--;
	}

	/* Returns the original destination pointer. */
	return dest;
}

/*
 * Installs the kernel allocator used by the HAL.
 */
void
hal_set_allocator(
	void *(*alloc)(size_t size),
	void (*free_fn)(void *p))
{
	/* Requires one complete allocator installation exactly once. */
	if (alloc == NULL || free_fn == NULL || allocator_alloc != NULL ||
	    allocator_free != NULL) {
		HAL_FATAL("hal_set_allocator must be called exactly once");
	}

	/* Publishes both allocator callbacks as one interface. */
	allocator_alloc = alloc;
	allocator_free = free_fn;
}

/*
 * Allocates memory through the kernel allocator.
 */
void *
hal_malloc(
	size_t size)
{
	void *memory;

	/* Rejects allocation before the kernel installs its allocator. */
	if (allocator_alloc == NULL)
		HAL_FATAL("hal_malloc before hal_set_allocator");

	/* Allocates the requested storage through the installed callback. */
	memory = allocator_alloc(size);

	/* Returns the allocator result unchanged. */
	return memory;
}

/*
 * Releases memory through the kernel allocator.
 */
void
hal_free(
	void *ptr)
{
	/* Rejects release before the kernel installs its allocator. */
	if (allocator_free == NULL)
		HAL_FATAL("hal_free before hal_set_allocator");

	/* Releases the caller-owned storage through the installed callback. */
	allocator_free(ptr);
}

/*
 * Halts the current CPU until its next interrupt.
 */
void
hal_halt(
	void)
{
	/* Executes one processor halt instruction. */
	asm_hlt();
}

/*
 * Writes one character to the HAL console.
 */
int
hal_putchar(
	int c)
{
	/* Writes the character through the board console. */
	hal_cons_putc(c);

	/* Returns the written character. */
	return c;
}

/*
 * Writes one terminated string to the HAL console.
 */
int
hal_puts(
	const char *s)
{
	/* Writes the complete string through the board console. */
	hal_cons_write(s);

	/* Reports a successful write. */
	return 0;
}

/*
 * Writes a restricted formatted string to the HAL console.
 */
int
hal_printf(
	const char *format,
	...)
{
	__builtin_va_list ap;
	const char *p;
	const char *string;
	uint64_t magnitude;
	int64_t value;
	int longs;
	int width;
	int zero;

	/* Traverses the format string and consumes arguments in format order. */
	__builtin_va_start(ap, format);
	for (p = format; *p != '\0'; p++) {
		/* Emits ordinary characters without parsing a conversion. */
		if (*p != '%') {
			hal_cons_putc(*p);
			continue;
		}

		/* Parses the supported zero-fill flag and decimal field width. */
		longs = 0;
		zero = 0;
		width = 0;
		p++;

		/* Consumes the supported zero-fill flag. */
		if (*p == '0') {
			zero = 1;
			p++;
		}

		/* Accumulates every decimal width digit. */
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (*p - '0');
			p++;
		}

		/* Counts the optional l and ll length modifiers. */
		while (*p == 'l' && longs < 2) {
			longs++;
			p++;
		}

		/* Emits the selected restricted conversion. */
		switch (*p) {
		case 'c':
			hal_cons_putc(__builtin_va_arg(ap, int));
			break;
		case 's':
			string = __builtin_va_arg(ap, const char *);

			/* Emits the supplied string or its null-pointer marker. */
			if (string != NULL) {
				hal_cons_write(string);
			} else {
				hal_cons_write("(null)");
			}
			break;
		case 'd':
			/* Consumes the argument at its promoted width. */
			if (longs == 0) {
				value = __builtin_va_arg(ap, int);
			} else if (longs == 1) {
				value = __builtin_va_arg(ap, long);
			} else {
				value = __builtin_va_arg(ap, long long);
			}

			/* Emits a sign before converting a negative magnitude. */
			magnitude = (uint64_t)value;
			if (value < 0) {
				hal_cons_putc('-');
				magnitude = 0U - magnitude;
			}
			put_unsigned(magnitude, 10, 0, width, zero);
			break;
		case 'u':
		case 'x':
		case 'X':
			/* Consumes the argument at its promoted width. */
			if (longs == 0) {
				magnitude = __builtin_va_arg(ap, uint32_t);
			} else if (longs == 1) {
				magnitude = __builtin_va_arg(ap, unsigned long);
			} else {
				magnitude = __builtin_va_arg(ap,
				    unsigned long long);
			}
			put_unsigned(
				magnitude,
				*p == 'u' ? 10 : 16,
				*p == 'X',
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

			/* Emits the unknown conversion or reprocesses the terminator. */
			if (*p != '\0') {
				hal_cons_putc(*p);
			} else {
				p--;
			}
			break;
		}
	}
	__builtin_va_end(ap);

	/* Reports a successful formatted write. */
	return 0;
}

/*
 * Reports an assertion failure and halts permanently.
 */
void
hal_assert(
	const char *file,
	int line,
	const char *exp)
{
	/* Reports the failed source expression before disabling interrupts. */
	hal_printf("\nassert: %s:%d: %s\n", file, line, exp);
	asm_cli();

	/* Halts permanently after the assertion failure. */
	for (;;)
		asm_hlt();
}

/*
 * Reports a fatal HAL error and halts permanently.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *s)
{
	/* Reports the fatal source location before disabling interrupts. */
	hal_printf("\nfatal: %s:%d: %s\n", file, line, s);
	asm_cli();

	/* Halts permanently after the fatal error. */
	for (;;)
		asm_hlt();
}

/*
 * Fills a buffer with processor random-number output.
 */
bool
hal_entropy_fill(
	void *buffer,
	size_t size)
{
	uint8_t *bytes;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t value;
	unsigned char ready;
	unsigned attempt;
	size_t chunk;

	/* Queries whether this processor implements RDRAND. */
	bytes = buffer;
	eax = 1;
	__asm__ volatile("cpuid"
	    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	(void)ebx;
	(void)edx;

	/* Rejects processors without the RDRAND feature bit. */
	if ((ecx & (1U << 30)) == 0U)
		return false;

	/* Generates and copies random words until the buffer is full. */
	while (size != 0U) {
		value = 0;
		ready = 0;

		/* Retries a temporarily unavailable random-number instruction. */
		for (attempt = 0; attempt < 10U && ready == 0U; attempt++) {
			__asm__ volatile("rdrand %0; setc %1"
			    : "=r"(value), "=qm"(ready));
		}

		/* Reports failure when no retry produced a random word. */
		if (ready == 0U)
			return false;

		/* Copies the usable portion of this random word. */
		chunk = size < sizeof(value) ? size : sizeof(value);
		hal_memcpy(bytes, &value, chunk);
		bytes += chunk;
		size -= chunk;
	}

	/* Reports a completely filled entropy buffer. */
	return true;
}

/*
 * Issues a full memory barrier.
 */
void
hal_mb(
	void)
{
	unsigned value;

	/* Serializes memory through a locked operation. */
	value = 0;
	__asm__ volatile("lock; addl $0,%0" : "+m"(value) : : "memory");
}

/*
 * Issues a compiler read barrier on i386.
 */
void
hal_rmb(
	void)
{
	/* Prevents compiler movement across the read barrier. */
	__asm__ volatile("" : : : "memory");
}

/*
 * Issues a compiler write barrier on i386.
 */
void
hal_wmb(
	void)
{
	/* Prevents compiler movement across the write barrier. */
	__asm__ volatile("" : : : "memory");
}

/*
 * Issues a full I/O barrier on i386.
 */
void
hal_io_mb(
	void)
{
	/* Reuses the processor's full memory barrier. */
	hal_mb();
}

/*
 * Issues an I/O read barrier on i386.
 */
void
hal_io_rmb(
	void)
{
	/* Reuses the processor's read barrier. */
	hal_rmb();
}

/*
 * Issues an I/O write barrier on i386.
 */
void
hal_io_wmb(
	void)
{
	/* Reuses the processor's write barrier. */
	hal_wmb();
}

/* Writes one unsigned value in the requested base and field width. */
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
	int n;

	/* Converts digits in reverse order into the local buffer. */
	n = 0;
	do {
		digit = (unsigned)(value % base);
		digits[n] = (char)(digit < 10 ?
		    '0' + digit : (upper ? 'A' : 'a') + digit - 10);
		n++;
		value /= base;
	} while (value != 0);

	/* Emits enough leading fill characters for the requested width. */
	while (width > n) {
		hal_cons_putc(zero ? '0' : ' ');
		width--;
	}

	/* Emits the converted digits in normal order. */
	while (n > 0) {
		n--;
		hal_cons_putc(digits[n]);
	}
}
