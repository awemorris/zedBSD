/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel log.
 *
 * Log records are appended to a fixed ring buffer that drops its oldest
 * bytes when full and counts what it dropped, and every record is mirrored
 * to the platform debug console.  kern_logf() provides the small printf
 * subset the kernel needs without a heap or floating point.
 */

#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/platform.h"
#include <stdarg.h>
#include <string.h>

#define KLOG_CAPACITY (32U * 1024U)

static struct spinlock klog_lock;
static char klog_buffer[KLOG_CAPACITY];
static size_t klog_oldest;
static size_t klog_used;
static uint64_t klog_dropped;

static void append_locked(const char *bytes, size_t length);
static void emit_char(char *out, size_t cap, size_t *used, char c);
static void emit_text(char *out, size_t cap, size_t *used, const char *s);
static void emit_number(char *out, size_t cap, size_t *used, uint64_t value, unsigned base, unsigned width, int zero, int upper);

/*
 * Initializes an empty kernel log.
 */
void
kern_log_init(
	void)
{
	/* Starts with an empty ring and no dropped bytes. */
	spin_init(&klog_lock, LOCK_RANK_KLOG, "kernel log");
	klog_oldest = 0;
	klog_used = 0;
	klog_dropped = 0;
}

/*
 * Appends one record to the log and mirrors it to the debug console.
 */
void
kern_log_write(
	const char *bytes,
	size_t length)
{
	char chunk[257];
	unsigned long irq;
	void (*console_output)(int c);
	size_t at;
	size_t n;

	/* Ignores an empty record. */
	if (bytes == NULL || length == 0)
		return;

	/* Appends the record to the ring buffer. */
	irq = spin_lock_irqsave(&klog_lock);

	append_locked(bytes, length);

	spin_unlock_irqrestore(&klog_lock, irq);

	/*
	 * Mirrors the record to the console the kernel has published, so a
	 * driver diagnostic is visible on screen and not only in the ring.
	 */
	console_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (console_output != NULL) {
		for (at = 0; at < length; at++)
			console_output((unsigned char)bytes[at]);
	}

	/* Mirrors the record to the debug console in terminated chunks. */
	at = 0;
	while (at < length) {
		n = length - at;
		if (n > sizeof(chunk) - 1U)
			n = sizeof(chunk) - 1U;
		memcpy(chunk, bytes + at, n);
		chunk[n] = 0;
		kern_platform_debug_write(chunk);
		at += n;
	}
}

/*
 * Formats one record with the kernel printf subset and logs it.
 *
 * The subset covers %s, %c, %u, %x, %X, %d, %i, %p, and %%, with an optional
 * zero flag, decimal field width, and l or ll length modifiers.  Output is
 * truncated to the internal 512-byte buffer.
 */
void
kern_logf(
	const char *format,
	...)
{
	char text[512];
	va_list args;
	size_t used;
	unsigned width;
	unsigned long_count;
	uint64_t value;
	int64_t signed_value;
	uintptr_t pointer;
	int zero;
	char spec;

	/* Ignores a missing format. */
	if (format == NULL)
		return;

	/* Renders every literal byte or conversion in format order. */
	used = 0;
	va_start(args, format);
	while (*format) {
		width = 0;
		long_count = 0;
		zero = 0;

		/* Copies a literal byte. */
		if (*format != '%') {
			emit_char(text, sizeof(text) - 1U, &used, *format++);
			continue;
		}

		format++;

		/* Copies an escaped percent sign. */
		if (*format == '%') {
			emit_char(text, sizeof(text) - 1U, &used, *format++);
			continue;
		}

		/* Parses the zero flag, field width, and length modifiers. */
		if (*format == '0') {
			zero = 1;
			format++;
		}
		while (*format >= '0' && *format <= '9') {
			width = width * 10U + (unsigned)(*format - '0');
			format++;
		}
		while (*format == 'l') {
			long_count++;
			format++;
		}

		/* Takes the conversion character, or none at the end of the format. */
		spec = 0;
		if (*format)
			spec = *format++;

		/* Emits the conversion. */
		if (spec == 's') {
			emit_text(text, sizeof(text) - 1U, &used, va_arg(args, const char *));
		} else if (spec == 'c') {
			emit_char(text, sizeof(text) - 1U, &used, (char)va_arg(args, int));
		} else if (spec == 'u' || spec == 'x' || spec == 'X') {
			if (long_count >= 2)
				value = va_arg(args, unsigned long long);
			else if (long_count)
				value = va_arg(args, unsigned long);
			else
				value = va_arg(args, unsigned);
			emit_number(
				text,
				sizeof(text) - 1U,
				&used,
				value,
				spec == 'u' ? 10U : 16U,
				width,
				zero,
				spec == 'X');
		} else if (spec == 'd' || spec == 'i') {
			if (long_count >= 2)
				signed_value = va_arg(args, long long);
			else if (long_count)
				signed_value = va_arg(args, long);
			else
				signed_value = va_arg(args, int);

			/* Emits the sign before the magnitude. */
			value = (uint64_t)signed_value;
			if (signed_value < 0) {
				emit_char(text, sizeof(text) - 1U, &used, '-');
				value = 0U - value;
			}

			emit_number(
				text,
				sizeof(text) - 1U,
				&used,
				value,
				10U,
				width,
				zero,
				0);
		} else if (spec == 'p') {
			pointer = (uintptr_t)va_arg(args, void *);
			emit_text(text, sizeof(text) - 1U, &used, "0x");
			emit_number(
				text,
				sizeof(text) - 1U,
				&used,
				pointer,
				16U,
				(unsigned)(sizeof(uintptr_t) * 2U),
				1,
				0);
		} else {
			emit_char(text, sizeof(text) - 1U, &used, '%');
			if (spec)
				emit_char(text, sizeof(text) - 1U, &used, spec);
		}
	}

	va_end(args);

	/* Terminates the text inside the buffer. */
	if (used >= sizeof(text))
		used = sizeof(text) - 1U;
	text[used] = 0;

	/* Records the rendered text. */
	kern_log_write(text, used);
}

/*
 * Copies the retained log into a caller buffer.
 *
 * The retained length is always reported; the copy happens only when the
 * buffer can hold all of it.
 */
size_t
kern_log_snapshot(
	char *buffer,
	size_t capacity,
	uint64_t *dropped)
{
	unsigned long irq;
	size_t i;
	size_t needed;

	irq = spin_lock_irqsave(&klog_lock);

	/* Reports the retained length and the dropped byte count. */
	needed = klog_used;
	if (dropped)
		*dropped = klog_dropped;

	/* Copies the ring contents in order when the buffer is large enough. */
	if (buffer && capacity >= needed) {
		for (i = 0; i < needed; i++)
			buffer[i] = klog_buffer[(klog_oldest + i) % KLOG_CAPACITY];
	}

	spin_unlock_irqrestore(&klog_lock, irq);

	/* Reports the retained length. */
	return needed;
}

/*
 * Reports the ring buffer capacity in bytes.
 */
size_t
kern_log_capacity(
	void)
{
	/* Reports the fixed capacity. */
	return KLOG_CAPACITY;
}

/* Appends bytes to the ring, dropping the oldest bytes when it is full. */
static void
append_locked(
	const char *bytes,
	size_t length)
{
	size_t i;
	uint64_t lost;
	size_t evicted;

	/* Keeps only the tail of a record larger than the whole ring. */
	if (length >= KLOG_CAPACITY) {
		lost = (uint64_t)klog_used + (uint64_t)(length - KLOG_CAPACITY);
		if (UINT64_MAX - klog_dropped < lost)
			klog_dropped = UINT64_MAX;
		else
			klog_dropped += lost;
		bytes += length - KLOG_CAPACITY;
		length = KLOG_CAPACITY;
		klog_oldest = 0;
		klog_used = 0;
	} else if (length > KLOG_CAPACITY - klog_used) {
		/* Evicts just enough of the oldest bytes to fit the record. */
		evicted = length - (KLOG_CAPACITY - klog_used);
		klog_oldest = (klog_oldest + evicted) % KLOG_CAPACITY;
		klog_used -= evicted;
		if (UINT64_MAX - klog_dropped < (uint64_t)evicted)
			klog_dropped = UINT64_MAX;
		else
			klog_dropped += (uint64_t)evicted;
	}

	/* Copies the record after the retained bytes, wrapping around. */
	for (i = 0; i < length; i++)
		klog_buffer[(klog_oldest + klog_used + i) % KLOG_CAPACITY] = bytes[i];
	klog_used += length;
}

/* Appends one byte while counting every byte, even past the capacity. */
static void
emit_char(
	char *out,
	size_t cap,
	size_t *used,
	char c)
{
	/* Stores the byte only while it fits. */
	if (*used < cap)
		out[*used] = c;
	(*used)++;
}

/* Appends a string, substituting a marker for a null pointer. */
static void
emit_text(
	char *out,
	size_t cap,
	size_t *used,
	const char *s)
{
	/* Substitutes a readable marker for a null string. */
	if (s == NULL)
		s = "(null)";

	/* Appends every byte of the string. */
	while (*s)
		emit_char(out, cap, used, *s++);
}

/* Appends an unsigned number in the requested base and field width. */
static void
emit_number(
	char *out,
	size_t cap,
	size_t *used,
	uint64_t value,
	unsigned base,
	unsigned width,
	int zero,
	int upper)
{
	char digits[32];
	const char *alphabet;
	unsigned count;

	/* Selects the digit alphabet. */
	alphabet = "0123456789abcdef";
	if (upper)
		alphabet = "0123456789ABCDEF";

	/* Builds the digits in reverse order. */
	count = 0;
	do {
		digits[count++] = alphabet[value % base];
		value /= base;
	} while (value && count < sizeof(digits));

	/* Pads the number to the field width. */
	while (width > count) {
		emit_char(out, cap, used, zero ? '0' : ' ');
		width--;
	}

	/* Emits the digits in display order. */
	while (count)
		emit_char(out, cap, used, digits[--count]);
}
