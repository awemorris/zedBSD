/*
 * Kernel C runtime subset, self-contained in the HAL.
 *
 * The HAL owns no heap: hal_malloc goes through an allocator the
 * embedding kernel injects.  Console output goes through the BSP
 * console.  hal_printf implements exactly what the HAL sources use:
 * %c, %s, %d, %u, %x/%X with optional zero-padded width (e.g. %08X).
 */

#include <hal/hal.h>
#include "asm.h"

int
hal_strlen(const char *s)
{
	int n = 0;

	while (s[n] != '\0')
		n++;
	return n;
}

void *
hal_memset(void *s, int c, size_t n)
{
	uint8 *p = s;

	while (n--)
		*p++ = (uint8)c;
	return s;
}

void *
hal_memset16(uint16 *s, uint16 c, size_t n)
{
	uint16 *p = s;

	while (n--)
		*p++ = c;
	return s;
}

void *
hal_memset32(uint32 *s, uint32 c, size_t n)
{
	uint32 *p = s;

	while (n--)
		*p++ = c;
	return s;
}

void *
hal_memcpy(void *dest, const void *src, size_t n)
{
	uint8 *d = dest;
	const uint8 *s = src;

	while (n--)
		*d++ = *s++;
	return dest;
}

/* --------------------------------------------------------------- */

static void *(*allocator_alloc)(size_t size);
static void (*allocator_free)(void *p);

void
hal_set_allocator(void *(*alloc)(size_t size), void (*free_fn)(void *p))
{
	allocator_alloc = alloc;
	allocator_free = free_fn;
}

void *
hal_malloc(size_t size)
{
	if (allocator_alloc == NULL)
		HAL_FATAL("hal_malloc before hal_set_allocator");
	return allocator_alloc(size);
}

void
hal_free(void *ptr)
{
	if (allocator_free == NULL)
		HAL_FATAL("hal_free before hal_set_allocator");
	allocator_free(ptr);
}

/* --------------------------------------------------------------- */

int
hal_putchar(int c)
{
	cons_putc(c);
	return c;
}

int
hal_puts(const char *s)
{
	cons_puts(s);
	return 0;
}

static void
put_unsigned(uint32 value, unsigned base, int upper, int width, int zero)
{
	char digits[12];
	int n = 0;

	do {
		unsigned d = value % base;

		digits[n++] = (char)(d < 10 ? '0' + d :
				     (upper ? 'A' : 'a') + d - 10);
		value /= base;
	} while (value != 0);
	while (width > n) {
		cons_putc(zero ? '0' : ' ');
		width--;
	}
	while (n > 0)
		cons_putc(digits[--n]);
}

int
hal_printf(const char *format, ...)
{
	__builtin_va_list ap;
	const char *p;

	__builtin_va_start(ap, format);
	for (p = format; *p != '\0'; p++) {
		int zero = 0;
		int width = 0;

		if (*p != '%') {
			cons_putc(*p);
			continue;
		}
		p++;
		if (*p == '0') {
			zero = 1;
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (*p - '0');
			p++;
		}
		switch (*p) {
		case 'c':
			cons_putc(__builtin_va_arg(ap, int));
			break;
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);

			cons_puts(s != NULL ? s : "(null)");
			break;
		}
		case 'd': {
			int v = __builtin_va_arg(ap, int);

			if (v < 0) {
				cons_putc('-');
				v = -v;
			}
			put_unsigned((uint32)v, 10, 0, width, zero);
			break;
		}
		case 'u':
			put_unsigned(__builtin_va_arg(ap, uint32), 10, 0,
				     width, zero);
			break;
		case 'x':
			put_unsigned(__builtin_va_arg(ap, uint32), 16, 0,
				     width, zero);
			break;
		case 'X':
			put_unsigned(__builtin_va_arg(ap, uint32), 16, 1,
				     width, zero);
			break;
		case '%':
			cons_putc('%');
			break;
		default:
			cons_putc('%');
			if (*p != '\0')
				cons_putc(*p);
			else
				p--;
			break;
		}
	}
	__builtin_va_end(ap);
	return 0;
}

/* --------------------------------------------------------------- */

void
hal_assert(const char *file, int line, const char *exp)
{
	hal_printf("\nassert: %s:%d: %s\n", file, line, exp);
	asm_cli();
	for (;;)
		asm_hlt();
}

void
hal_fatal(const char *file, int line, const char *s)
{
	hal_printf("\nfatal: %s:%d: %s\n", file, line, s);
	asm_cli();
	for (;;)
		asm_hlt();
}
