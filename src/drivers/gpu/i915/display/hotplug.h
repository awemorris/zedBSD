/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hotplug path (hotplug.c).
 *
 * The HDMI hotplug receive path of the Linux text and the hotplug part of
 * the Linux driver probe:
 *
 *   start   intel_hpd_init_early() and the connectors intel_setup_outputs()
 *           would have made (one per encoder: eDP-1, HDMI-A-1, DP-1, DP-2),
 *           then the interrupt entry is opened;
 *   irq     from gen8_de_irq_handler(): the acknowledged SDEIIR value is
 *           handed to icp_irq_handler() (interrupt context);
 *   stop    the interrupt entry is closed, the entries in flight are
 *           drained, then intel_hpd_cancel_work() runs.
 *
 * Only the functions whose arguments are neutral display types are declared
 * here; the functions of the hotplug environment that take its Linux types
 * (the register access, the object walks, the port-to-PHY mapping) are
 * declared in hotplug-internal.h.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_HOTPLUG_H
#define DRIVERS_GPU_I915_DISPLAY_HOTPLUG_H

#include "internal.h"

struct i915_display;
struct i915_mmio;

/*
 * ==== The hotplug world ====
 */

int drv_i915_hpd_world_create(struct i915_display *display);
void drv_i915_hpd_world_destroy(struct i915_display *display);

/*
 * ==== The hotplug part of the Linux driver probe ====
 */

int drv_i915_ddi_hpd_pin(int display_ver, int port);
void drv_i915_hpd_init_pins(struct i915_hotplug *hp, int display_ver, int pch_type);
void drv_i915_hpd_irq_setup(struct i915_hotplug *hp, struct i915_mmio *m, int pch_type, int intel_irqs_enabled);
void drv_i915_hpd_init(struct i915_hotplug *hp, struct i915_mmio *m, int display_ver, int pch_type, int display_irqs_enabled, int intel_irqs_enabled);
void drv_i915_hpd_poll_disable(struct i915_hotplug *hp, struct i915_power_domains *pd, struct i915_pw_ctx *c);

/*
 * ==== The hotplug receive path ====
 */

int drv_i915_hpd_start(struct i915_display *display, struct i915_hotplug *hp, struct i915_mmio *m, const struct i915_display_nogem *nogem, struct i915_power_domains *pd, struct i915_pw_ctx *c, int pch_type, int intel_irqs_enabled, struct i915_hpd_fake_regs *fake);
void drv_i915_hpd_pch_irq(struct i915_display *display, uint32_t sde_iir);
void drv_i915_hpd_de_irq(struct i915_display *display, uint32_t de_hpd_iir);
void drv_i915_hpd_stop(struct i915_display *display);
int drv_i915_hpd_probe_connector(struct i915_display *display, unsigned idx);

/*
 * ==== What the path saw, for the diagnostics and the tests ====
 */

const char *drv_i915_hpd_connector_name(struct i915_display *display, unsigned idx);
void drv_i915_hpd_summary(struct i915_display *display, struct i915_hpd_summary *s);
const struct i915_hpd_irq_record *drv_i915_hpd_irq_record(struct i915_display *display, unsigned i);
const struct i915_hpd_hotplug_record *drv_i915_hpd_hotplug_record(struct i915_display *display, unsigned i);
uint32_t drv_i915_hpd_event_bits(struct i915_display *display);
uint32_t drv_i915_hpd_retry_bits(struct i915_display *display);
int drv_i915_hpd_pin_state(struct i915_display *display, int pin);
int drv_i915_hpd_pin_count(struct i915_display *display, int pin);
int drv_i915_hpd_connector_polled(struct i915_display *display, unsigned idx);
int drv_i915_hpd_connector_status(struct i915_display *display, unsigned idx);

/*
 * ==== The display lease's topology events ====
 */

uint64_t drv_i915_hpd_topology_sequence(struct i915_display *display);
void drv_i915_hpd_topology_touch(struct i915_display *display);

/*
 * ==== The connectors for the display inventory (ws113-p002) ====
 */

int drv_i915_hpd_output(struct i915_display *display, unsigned idx, struct i915_hpd_output *output);
unsigned drv_i915_hpd_output_count(struct i915_display *display);
void drv_i915_hpd_output_take(struct i915_display *display, unsigned idx);

#endif /* DRIVERS_GPU_I915_DISPLAY_HOTPLUG_H */
