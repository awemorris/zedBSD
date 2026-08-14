#ifndef ZEDBSD_HAL_SPARCV9_BSP_H
#define ZEDBSD_HAL_SPARCV9_BSP_H

#include <kern/sun4u/boot.h>

void sun4u_boot_init(const struct zedbsd_sun4u_handoff *handoff);
const struct zedbsd_sun4u_handoff *sun4u_boot_handoff(void);

#endif
