/*
 * Framebuffer ownership flag (see <hal/framebuffer.h>).
 */

#include <hal/framebuffer.h>

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
