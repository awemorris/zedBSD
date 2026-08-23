#include <hal/hal.h>
#include "asm.h"

int hal_strlen(const char *s) { int n = 0; while (s[n] != '\0') n++; return n; }
void *hal_memset(void *p, int c, size_t n)
{ uint8_t *d = p; while (n-- != 0) *d++ = (uint8_t)c; return p; }
void *hal_memset16(uint16_t *p, uint16_t c, size_t n)
{ uint16_t *d = p; while (n-- != 0) *d++ = c; return p; }
void *hal_memset32(uint32_t *p, uint32_t c, size_t n)
{ uint32_t *d = p; while (n-- != 0) *d++ = c; return p; }
void *hal_memcpy(void *d0, const void *s0, size_t n)
{ uint8_t *d = d0; const uint8_t *s = s0; while (n-- != 0) *d++ = *s++; return d0; }
static void *(*alloc_fn)(size_t);
static void (*free_fn)(void *);
void hal_set_allocator(void *(*a)(size_t), void (*f)(void *))
{
	if (a == NULL || f == NULL || alloc_fn != NULL || free_fn != NULL)
		HAL_FATAL("hal_set_allocator must be called exactly once");
	alloc_fn = a; free_fn = f;
}
void *hal_malloc(size_t n) { if (alloc_fn == NULL) HAL_FATAL("allocator unset"); return alloc_fn(n); }
void hal_free(void *p) { if (free_fn == NULL) HAL_FATAL("allocator unset"); free_fn(p); }

int hal_putchar(int c) { hal_cons_putc(c); return c; }
int hal_puts(const char *s) { hal_cons_write(s); return 0; }
static void put_u64(uint64_t value, unsigned base, int width)
{
	char b[24]; int n = 0;
	do { unsigned d = (unsigned)(value % base); b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); value /= base; } while (value != 0);
	while (width-- > n) hal_cons_putc('0');
	while (n != 0) hal_cons_putc(b[--n]);
}
int hal_printf(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	while (*fmt != '\0') {
		int width = 0; int long_arg = 0;
		if (*fmt != '%') { hal_cons_putc(*fmt++); continue; }
		fmt++;
		if (*fmt == '0') fmt++;
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l') { long_arg = 1; fmt++; if (*fmt == 'l') fmt++; }
		switch (*fmt++) {
		case 'c': hal_cons_putc(__builtin_va_arg(ap, int)); break;
		case 's': { const char *s = __builtin_va_arg(ap, const char *); hal_cons_write(s ? s : "(null)"); break; }
		case 'u': put_u64(long_arg ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, uint32_t), 10, width); break;
		case 'x': put_u64(long_arg ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, uint32_t), 16, width); break;
		case 'p': put_u64((uintptr_t)__builtin_va_arg(ap, void *), 16, 16); break;
		case '%': hal_cons_putc('%'); break;
		default: hal_cons_putc('?'); break;
		}
	}
	__builtin_va_end(ap);
	return 0;
}
void hal_assert(const char *f, int l, const char *e)
{ hal_printf("assert: %s:%u: %s\n", f, (uint32_t)l, e); arm64_irq_mask(); for (;;) arm64_wfe(); }
void hal_fatal(const char *f, int l, const char *s)
{ hal_printf("fatal: %s:%u: %s\n", f, (uint32_t)l, s); arm64_irq_mask(); for (;;) arm64_wfe(); }
