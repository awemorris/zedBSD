/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

#ifndef ZEDBSD_DRIVERS_SUN4U_CMD646_H
#define ZEDBSD_DRIVERS_SUN4U_CMD646_H
#include <stdint.h>
struct disk;
int drv_sun4u_cmd646_init(uint16_t command_port, uint16_t control_port);
struct disk *drv_sun4u_cmd646_disk(void);
#endif
