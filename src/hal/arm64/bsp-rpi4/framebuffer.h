#ifndef ZEDBSD_HAL_ARM64_RPI4_FRAMEBUFFER_H
#define ZEDBSD_HAL_ARM64_RPI4_FRAMEBUFFER_H
#include <hal/types.h>
int rpi4_framebuffer_init(uintptr_t mailbox_phys);
int rpi4_framebuffer_ready(void);
void rpi4_framebuffer_cell(unsigned row,unsigned column,int character,uint8 attribute);
void rpi4_framebuffer_cursor(unsigned row,unsigned column,int visible);
#endif
