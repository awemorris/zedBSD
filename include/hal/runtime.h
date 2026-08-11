/*
 * Kernel C runtime subset, implemented inside the HAL (i386/lib.c).
 */

#ifndef _SYS_CRT_CRT_H_
#define _SYS_CRT_CRT_H_

#include <hal/types.h>

#define HAL_ASSERT(e)	((e) ? (void)0 : hal_assert(__FILE__, __LINE__, #e))
#define HAL_FATAL(msg)	hal_fatal(__FILE__, __LINE__, msg)

/* String and memory. */
int hal_strlen(const char *s);
void *hal_memset(void *s, int c, size_t n);
void *hal_memset16(uint16 *s, uint16 c, size_t n);
void *hal_memset32(uint32 *s, uint32 c, size_t n);
void *hal_memcpy(void *dest, const void *src, size_t n);

/* Freestanding compilers may also emit calls to the bare names; the
 * embedding kernel's libc provides them at link time. */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

/*
 * Allocation.  The HAL owns no heap: the embedding kernel injects an
 * allocator before any HAL path that allocates (task creation) runs.
 */
void hal_set_allocator(void *(*alloc)(size_t size), void (*free)(void *p));
void *hal_malloc(size_t size);
void hal_free(void *ptr);

/* Console output (backed by the BSP console). */
int hal_putchar(int c);
int hal_puts(const char *s);
int hal_printf(const char *format, ...);

/* Diagnostics; both halt. */
void hal_assert(const char *file, int line, const char *exp);
void hal_fatal(const char *file, int line, const char *s);

#endif
