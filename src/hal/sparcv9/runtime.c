/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's C runtime: memory helpers, formatted output, the stops
 * on a failed check, the barriers and cache maintenance, and the compiler's
 * helper calls the kernel needs.
 */

#include <hal/hal.h>
#include "defs.h"
#include "runtime.h"

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
	sparcv9_halt_forever();
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
	sparcv9_halt_forever();
}

/*
 * Orders every earlier memory access before every later one.
 */
void
hal_mb(
	void)
{
	/* Waits for all outstanding accesses to complete. */
	__asm__ volatile("membar #Sync" : : : "memory");
}

/*
 * Orders earlier loads before later loads and stores.
 */
void
hal_rmb(
	void)
{
	/* Keeps loads ahead of what follows them. */
	__asm__ volatile("membar #LoadLoad | #LoadStore" : : : "memory");
}

/*
 * Orders earlier stores and loads before later stores.
 */
void
hal_wmb(
	void)
{
	/* Keeps stores behind what precedes them. */
	__asm__ volatile("membar #StoreStore | #LoadStore" : : : "memory");
}

/*
 * Orders every earlier device access before every later one.
 */
void
hal_io_mb(
	void)
{
	/* Device accesses complete in the same order as memory accesses. */
	hal_mb();
}

/*
 * Orders earlier device reads before what follows them.
 */
void
hal_io_rmb(
	void)
{
	/* Device reads are ordered as memory reads are. */
	hal_rmb();
}

/*
 * Orders earlier device writes before later ones.
 */
void
hal_io_wmb(
	void)
{
	/* Device writes are ordered as memory writes are. */
	hal_wmb();
}

/*
 * Discards the instruction cache's copy of a range.
 *
 * FLUSH keeps the instruction cache coherent with the stores made before
 * it; one per 32-byte line is enough on the UltraSPARC.
 */
void
hal_icache_invalidate_range(
	uintptr_t start,
	size_t size)
{
	uintptr_t address;
	uintptr_t end;

	/* Flushes every line the range touches. */
	end = start + size;
	for (address = start & ~(uintptr_t)31U; address < end; address += 32U)
		__asm__ volatile("flush %0" : : "r"(address) : "memory");
}

/*
 * Writes a range back to memory.
 *
 * The UltraSPARC's data caches are kept coherent with memory and with the
 * devices by the hardware, so only the instruction stream needs care.
 */
void
hal_dcache_clean_range(
	uintptr_t start,
	size_t size)
{
	/* Keeps instructions written through the data cache visible. */
	hal_icache_invalidate_range(start, size);
}

/*
 * Discards the data cache's copy of a range; the hardware keeps it
 * coherent, so only the instruction stream needs care.
 */
void
hal_dcache_invalidate_range(
	uintptr_t start,
	size_t size)
{
	/* Keeps instructions written through the data cache visible. */
	hal_icache_invalidate_range(start, size);
}

/*
 * Writes back and discards a range; the hardware keeps the data cache
 * coherent, so only the instruction stream needs care.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t start,
	size_t size)
{
	/* Keeps instructions written through the data cache visible. */
	hal_icache_invalidate_range(start, size);
}

/*
 * Makes instructions just written visible to instruction fetch.
 */
void
hal_sync_instruction_stream(
	void *address,
	size_t size)
{
	/* Flushes the range from the instruction cache. */
	hal_icache_invalidate_range((uintptr_t)address, size);

	/* Completes the flushes before the code is run. */
	hal_mb();
}

/*
 * Waits with interrupts off for good.
 */
void
hal_halt(
	void)
{
	/* The processor has no instruction that waits for an interrupt. */
	sparcv9_halt_forever();
}

/*
 * Reboots the machine; the port has no reset path, so it halts.
 */
void
hal_reset(
	void)
{
	/* Reports the stop, then halts. */
	hal_puts("SPARCV9 reset is not supported; halted\n");
	sparcv9_halt_forever();
}

/*
 * Turns the power off; the port has no power-off path, so it halts.
 */
void
hal_poweroff(
	void)
{
	/* Reports the stop, then halts. */
	hal_puts("SPARCV9 power-off is not supported; halted\n");
	sparcv9_halt_forever();
}

/*
 * Stops the processor after a kernel panic.
 */
void
hal_panic(
	void)
{
	/* The panic was already reported. */
	sparcv9_halt_forever();
}

/*
 * Reports that no entropy source is used: the UltraSPARC has no random
 * number source the HAL reads.
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

/*
 * Waits forever with interrupts off.
 */
void
sparcv9_halt_forever(
	void)
{
	/* Masks interrupts, then spins. */
	(void)hal_irq_disable();
	for (;;)
		__asm__ volatile("nop");
}

/*
 * Counts the trailing zero bits of a 64-bit word, for the compiler.
 *
 * The UltraSPARC has no instruction for it, so the compiler calls this.
 * The result for zero is undefined, as for __builtin_ctzll(); it is 64
 * here.
 */
int
__ctzdi2(
	unsigned long value)
{
	int count;

	/* Shifts the word right until its lowest bit is set. */
	count = 0;
	while (count < 64 && (value & 1UL) == 0) {
		value >>= 1;
		count++;
	}

	/* Succeeded: the number of zero bits below the lowest set bit. */
	return count;
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
