#ifndef KERN_HAL_SPARCV9_BSP_H
#define KERN_HAL_SPARCV9_BSP_H

#include <kern/boot.h>

void sun4u_boot_init(const struct kern_sun4u_boot_handoff *handoff);
const struct kern_sun4u_boot_handoff *sun4u_boot_handoff(void);
void sun4u_cons_init(void);

#endif
