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
	uint8_t *p = s;

	while (n--)
		*p++ = (uint8_t)c;
	return s;
}

void *
hal_memset16(uint16_t *s, uint16_t c, size_t n)
{
	uint16_t *p = s;

	while (n--)
		*p++ = c;
	return s;
}

void *
hal_memset32(uint32_t *s, uint32_t c, size_t n)
{
	uint32_t *p = s;

	while (n--)
		*p++ = c;
	return s;
}

void *
hal_memcpy(void *dest, const void *src, size_t n)
{
	uint8_t *d = dest;
	const uint8_t *s = src;

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
	if (alloc == NULL || free_fn == NULL || allocator_alloc != NULL ||
	    allocator_free != NULL)
		HAL_FATAL("hal_set_allocator must be called exactly once");
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

void
hal_halt(void)
{
	asm_hlt();
}

/* --------------------------------------------------------------- */

int
hal_putchar(int c)
{
	hal_cons_putc(c);
	return c;
}

int
hal_puts(const char *s)
{
	hal_cons_write(s);
	return 0;
}

static void
put_unsigned(uint32_t value, unsigned base, int upper, int width, int zero)
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
		hal_cons_putc(zero ? '0' : ' ');
		width--;
	}
	while (n > 0)
		hal_cons_putc(digits[--n]);
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
			hal_cons_putc(*p);
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
			hal_cons_putc(__builtin_va_arg(ap, int));
			break;
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);

			hal_cons_write(s != NULL ? s : "(null)");
			break;
		}
		case 'd': {
			int v = __builtin_va_arg(ap, int);

			if (v < 0) {
				hal_cons_putc('-');
				v = -v;
			}
			put_unsigned((uint32_t)v, 10, 0, width, zero);
			break;
		}
		case 'u':
			put_unsigned(__builtin_va_arg(ap, uint32_t), 10, 0,
				     width, zero);
			break;
		case 'x':
			put_unsigned(__builtin_va_arg(ap, uint32_t), 16, 0,
				     width, zero);
			break;
		case 'X':
			put_unsigned(__builtin_va_arg(ap, uint32_t), 16, 1,
				     width, zero);
			break;
		case '%':
			hal_cons_putc('%');
			break;
		default:
			hal_cons_putc('%');
			if (*p != '\0')
				hal_cons_putc(*p);
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

void hal_mb(void)
{
	unsigned value = 0;
	__asm__ volatile("lock; addl $0,%0" : "+m"(value) : : "memory");
}
void hal_rmb(void) { __asm__ volatile("" ::: "memory"); }
void hal_wmb(void) { __asm__ volatile("" ::: "memory"); }
void hal_io_mb(void) { hal_mb(); }
void hal_io_rmb(void) { hal_rmb(); }
void hal_io_wmb(void) { hal_wmb(); }
