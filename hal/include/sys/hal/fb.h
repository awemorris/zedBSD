/*
 * Framebuffer ownership.
 *
 * The framebuffer can be enabled and disabled at runtime; while it is
 * enabled, console ownership moves from cons to fb and the text console
 * suppresses its output.  The drawing operations themselves (FILL/LINE/
 * pattern/image, never a bare linear-buffer assumption) arrive with the
 * fb drivers; this header carries the ownership contract they and the
 * console agree on.
 */

#ifndef _SYS_ARCH_FB_H_
#define _SYS_ARCH_FB_H_

void fb_set_active(int active);
int fb_is_active(void);

#endif
