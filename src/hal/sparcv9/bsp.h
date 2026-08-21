#ifndef ZEDBSD_HAL_SPARCV9_BSP_H
#define ZEDBSD_HAL_SPARCV9_BSP_H

#include <kern/sun4u/boot.h>

void sun4u_boot_init(const struct sun4u_boot_handoff *handoff);
const struct sun4u_boot_handoff *sun4u_boot_handoff(void);
void sun4u_cons_init(void);

#endif
