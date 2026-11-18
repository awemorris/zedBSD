/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What binds the display's Type-C ports (xd-tc-port.c) to the device
 * (tc-port.c): a lock per port, the power wells, the clock and the log;
 * the ports the VBT declares; what the USB Type-C connector layer is told
 * of each port; and the hooks a screen reaches its port and its Dekel
 * PHY through.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_TC_PORT_H
#define DRIVERS_GPU_I915_DISPLAY_TC_PORT_H

#include <stdint.h>

#include "xd-tc-port.h"

struct i915_display;

void drv_i915_tc_port_start(struct i915_display *display);
void drv_i915_tc_port_suspend(struct i915_display *display);
void drv_i915_tc_port_resume(struct i915_display *display);
void drv_i915_tc_port_stop(struct i915_display *display);
void drv_i915_tc_port_destroy(struct i915_display *display);
struct i915_xd_tc_ports *drv_i915_tc_port_ports(struct i915_display *display);
int drv_i915_tc_port_of(struct i915_display *display, int ddi);
unsigned drv_i915_tc_port_aux(struct i915_display *display, unsigned tc);
void drv_i915_tc_port_report(struct i915_xd_tc_ports *ports, unsigned tc);
void drv_i915_lcd_tc_put_link(void *ctx, int tc_port);
void drv_i915_lcd_tc_get_link(void *ctx, int tc_port, int required_lanes);
int drv_i915_lcd_tc_mode(void *ctx, int tc_port);
void drv_i915_lcd_tc_set_fia_lane_count(void *ctx, int tc_port, int required_lanes, int lane_reversal);
unsigned drv_i915_lcd_tc_pin_assignment(void *ctx, int tc_port);
uint32_t drv_i915_lcd_dkl_read(void *ctx, int tc_port, uint32_t phy_address);
void drv_i915_lcd_dkl_write(void *ctx, int tc_port, uint32_t phy_address, uint32_t value);
void drv_i915_lcd_dkl_rmw(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set);

#endif
