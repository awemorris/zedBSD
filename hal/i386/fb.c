/*
 * Framebuffer ownership flag (see <sys/hal/fb.h>).
 */

#include <sys/hal/fb.h>

static int fb_active;

void
fb_set_active(int active)
{
	fb_active = active ? 1 : 0;
}

int
fb_is_active(void)
{
	return fb_active;
}
