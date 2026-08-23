#ifndef ZEDBSD_HAL_ARM64_RPI4_MAILBOX_H
#define ZEDBSD_HAL_ARM64_RPI4_MAILBOX_H
#include <hal/types.h>
int rpi4_mailbox_property(uintptr_t mailbox_phys,uint32_t *message,size_t bytes);
#endif
