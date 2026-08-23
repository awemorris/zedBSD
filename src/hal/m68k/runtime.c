/* Freestanding runtime and diagnostics for m68k. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>

int
hal_strlen(const char *string)
{
	int length = 0;
	while (string[length] != '\0')
		length++;
	return length;
}

void *
hal_memset(void *pointer, int value, size_t size)
{
	uint8_t *output = pointer;
	while (size-- != 0)
		*output++ = (uint8_t)value;
	return pointer;
}

void *
hal_memset16(uint16_t *pointer, uint16_t value, size_t count)
{
	uint16_t *output = pointer;
	while (count-- != 0)
		*output++ = value;
	return pointer;
}

void *
hal_memset32(uint32_t *pointer, uint32_t value, size_t count)
{
	uint32_t *output = pointer;
	while (count-- != 0)
		*output++ = value;
	return pointer;
}

void *
hal_memcpy(void *destination, const void *source, size_t size)
{
	uint8_t *output = destination;
	const uint8_t *input = source;
	while (size-- != 0)
		*output++ = *input++;
	return destination;
}

static void *(*allocator)(size_t);
static void (*deallocator)(void *);

void
hal_set_allocator(void *(*alloc)(size_t), void (*free_function)(void *))
{
	allocator = alloc;
	deallocator = free_function;
}

void *
hal_malloc(size_t size)
{
	if (allocator == NULL)
		HAL_FATAL("allocator unset");
	return allocator(size);
}

void
hal_free(void *pointer)
{
	if (deallocator == NULL)
		HAL_FATAL("allocator unset");
	deallocator(pointer);
}

int hal_putchar(int character) { hal_cons_putc(character); return character; }
int hal_puts(const char *string) { hal_cons_write(string); return 0; }

static uint64_t
divide_unsigned(uint64_t value, unsigned base, uint64_t *remainder)
{
	uint64_t quotient = 0, rest = 0;
	int bit;
	for (bit = 63; bit >= 0; bit--) {
		rest = (rest << 1) | ((value >> (unsigned)bit) & 1U);
		if (rest >= base) {
			rest -= base;
			quotient |= 1ULL << (unsigned)bit;
		}
	}
	*remainder = rest;
	return quotient;
}

static void
put_number(uint64_t value, unsigned base, int width)
{
	char digits[24];
	int count = 0;
	do {
		uint64_t remainder;
		value = divide_unsigned(value, base, &remainder);
		digits[count++] = (char)(remainder < 10 ? '0' + remainder :
		    'a' + remainder - 10);
	} while (value != 0);
	while (width-- > count)
		hal_cons_putc('0');
	while (count != 0)
		hal_cons_putc(digits[--count]);
}

int
hal_printf(const char *format, ...)
{
	__builtin_va_list arguments;
	__builtin_va_start(arguments, format);
	while (*format != '\0') {
		int width = 0, long_argument = 0;
		if (*format != '%') {
			hal_cons_putc(*format++);
			continue;
		}
		format++;
		if (*format == '0')
			format++;
		while (*format >= '0' && *format <= '9')
			width = width * 10 + (*format++ - '0');
		if (*format == 'l') {
			long_argument = 1;
			format++;
			if (*format == 'l')
				format++;
		}
		switch (*format++) {
		case 'c': hal_cons_putc(__builtin_va_arg(arguments, int)); break;
		case 's': {
			const char *string = __builtin_va_arg(arguments, const char *);
			hal_cons_write(string != NULL ? string : "(null)");
			break;
		}
		case 'u': put_number(long_argument ?
		    __builtin_va_arg(arguments, uint64_t) :
		    __builtin_va_arg(arguments, uint32_t), 10, width); break;
		case 'x': put_number(long_argument ?
		    __builtin_va_arg(arguments, uint64_t) :
		    __builtin_va_arg(arguments, uint32_t), 16, width); break;
		case 'd': {
			int64_t value = long_argument ?
			    __builtin_va_arg(arguments, int64_t) :
			    __builtin_va_arg(arguments, int32_t);
			uint64_t magnitude;
			if (value < 0) {
				hal_cons_putc('-');
				magnitude = (uint64_t)(-(value + 1)) + 1;
			} else {
				magnitude = (uint64_t)value;
			}
			put_number(magnitude, 10, width);
			break;
		}
		case 'p': put_number((uintptr_t)__builtin_va_arg(arguments, void *),
		    16, 8); break;
		case '%': hal_cons_putc('%'); break;
		default: hal_cons_putc('?'); break;
		}
	}
	__builtin_va_end(arguments);
	return 0;
}

void
hal_assert(const char *file, int line, const char *expression)
{
	hal_printf("assert: %s:%u: %s\n", file, (uint32_t)line, expression);
	(void)hal_irq_disable();
	for (;;)
		__asm__ volatile("stop #0x2700");
}

void
hal_fatal(const char *file, int line, const char *message)
{
	hal_printf("fatal: %s:%u: %s\n", file, (uint32_t)line, message);
	(void)hal_irq_disable();
	for (;;)
		__asm__ volatile("stop #0x2700");
}

void
hal_halt(void)
{
	__asm__ volatile("stop #0x2000" ::: "memory");
}

void hal_panic(void) { HAL_FATAL("kernel panic"); }
