#include <hal/hal.h>
#include "../bsp.h"

static struct sun4u_boot_handoff boot_handoff;

void sun4u_boot_init(const struct sun4u_boot_handoff *handoff)
{ hal_memcpy(&boot_handoff,handoff,sizeof(boot_handoff)); }
const struct sun4u_boot_handoff *sun4u_boot_handoff(void)
{ return &boot_handoff; }

void *
hal_get_arch_handoff(const char *name)
{
	(void)name;
	return NULL;
}
