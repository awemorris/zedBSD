/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What binds the display's Type-C ports (tc.c) to the device (tc-kern.c):
 * the registers, the power domains, a lock per port and the log, the ports
 * the VBT declares, and the hooks the other parts of the display reach the
 * ports through.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_TC_KERN_H
#define DRIVERS_GPU_I915_DISPLAY_TC_KERN_H

#include "internal.h"

void drv_i915_tc_kern_start(struct i915_display *display, struct i915_mmio *mmio, unsigned display_ver);
void drv_i915_tc_kern_stop(struct i915_display *display);
struct i915_tc *drv_i915_tc_kern_ports(struct i915_display *display);
int drv_i915_tc_kern_port_of(int port);
void drv_i915_lcd_tc_put_link(void *ctx, int tc_port);
void drv_i915_lcd_tc_get_link(void *ctx, int tc_port, int required_lanes);
int drv_i915_lcd_tc_mode(void *ctx, int tc_port);
void drv_i915_lcd_tc_set_fia_lane_count(void *ctx, int tc_port, int required_lanes, int lane_reversal);
unsigned drv_i915_lcd_tc_pin_assignment(void *ctx, int tc_port);
uint32_t drv_i915_lcd_dkl_read(void *ctx, int tc_port, uint32_t phy_address);
void drv_i915_lcd_dkl_write(void *ctx, int tc_port, uint32_t phy_address, uint32_t value);
void drv_i915_lcd_dkl_rmw(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set);
void drv_i915_tc_kern_report(struct i915_tc *tc, unsigned port);

#endif
