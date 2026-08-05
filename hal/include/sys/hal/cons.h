/*
 * Kernel debug console (BSP-provided).
 */

#ifndef _KERNEL_ARCH_CONS_H_
#define _KERNEL_ARCH_CONS_H_

#include <sys/types.h>

void bsp_cons_init(void);
void cons_cls(void);
void cons_putc(int c);
void cons_puts(const char *s);
int cons_getc(void);
/*
 * Character attributes: foreground and background 0-15.  A target that
 * cannot express one of them degrades (the PC-98 text plane has no
 * per-cell background, so bg is ignored there).
 */
void cons_set_attr(int fg, int bg);

#endif
