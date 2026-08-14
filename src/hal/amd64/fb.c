/*
 * Framebuffer ownership flag (see <hal/hal.h>).
 */

#include <hal/hal.h>

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
