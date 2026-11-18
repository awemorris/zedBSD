/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display half of the device interrupts (interrupts.c).
 *
 * The reset, postinstall and handler of the display engine sources (pipes,
 * ports, misc and the south display block), the pipe interrupt hooks the
 * power wells call, the drain of a pipe's in-flight handler work, and the
 * vblank references and waits of the panel runs.  The context of every
 * function is the display half of the interrupt device, struct
 * i915_display_irq; the top-level interrupt flow (../irq.c) reaches it
 * through the operations table drv_i915_display_irq_bind() installs.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_INTERRUPTS_H
#define DRIVERS_GPU_I915_DISPLAY_INTERRUPTS_H

#include "internal.h"

struct i915_display;
struct i915_irq_dev;

/*
 * ==== Binding to the interrupt device ====
 */

/*
 * Binds the display half to the interrupt device: fills the display
 * interrupt state from the display, binds the vblank delivery and the
 * power-well hooks, and installs the display operations table.
 */
void drv_i915_display_irq_bind(struct i915_display *display, struct i915_irq_dev *irq);

/* Unbinds the display operations table from the interrupt device (after uninstall). */
void drv_i915_display_irq_unbind(struct i915_display *display, struct i915_irq_dev *irq);

/* Binds the vblank state (before install) and the power-well hooks (pwc->irq_ops). */
void drv_i915_irq_vblank_init(struct i915_display_irq *d, struct i915_irq_vblank *v);

/* The power-well hooks (ctx: struct i915_display_irq). */
extern const struct i915_pw_irq_ops drv_i915_pw_irq_ops;

/*
 * ==== The pieces of the display interrupt flow ====
 */

void drv_i915_gen11_display_irq_reset(struct i915_display_irq *d);
void drv_i915_gen11_de_irq_postinstall(struct i915_display_irq *d);
void drv_i915_gen11_display_irq_handler(struct i915_display_irq *d);

/* gen8_irq_power_well_post_enable() / gen8_irq_power_well_pre_disable(). */
void drv_i915_gen8_irq_power_well_post_enable(struct i915_display_irq *d, unsigned pipe_mask);
int drv_i915_gen8_irq_power_well_pre_disable(struct i915_display_irq *d, unsigned pipe_mask);

/* Waits until no handler is inside the given pipes' registers: 0, ETIMEDOUT or EIO. */
int drv_i915_irq_drain_pipes(struct i915_display_irq *d, unsigned pipe_mask, unsigned timeout_us);

/* The ADL-P mask helpers (gen8_de_*_mask). */
uint32_t drv_i915_gen8_de_pipe_fault_mask(int display_ver);
uint32_t drv_i915_gen8_de_port_aux_mask(int display_ver);
uint32_t drv_i915_gen8_de_pipe_underrun_mask(int display_ver);
uint32_t drv_i915_gen8_de_pipe_flip_done_mask(int display_ver);

/*
 * ==== Vblank references and waits ====
 */

/*
 * A pipe's vblank references: the first get unmasks its vblank interrupt,
 * the last put masks it at once.  get: 0, or EINVAL when interrupts are
 * not enabled.
 */
int drv_i915_display_vblank_get(struct i915_display_irq *d, unsigned pipe);
void drv_i915_display_vblank_put(struct i915_display_irq *d, unsigned pipe);

/*
 * Waits for n new vblanks of a pipe (the vblank count and the hardware
 * frame counter must both advance).  0, EINVAL (no reference held),
 * EBUSY (the pipe already has a waiter), ETIMEDOUT or EIO (the time base
 * failed).
 */
int drv_i915_wait_vblank(struct i915_display_irq *d, unsigned pipe, unsigned n, unsigned timeout_ms, uint32_t (*read_frame)(void *ctx), void *frame_ctx, uint32_t *count_seen);

#endif /* DRIVERS_GPU_I915_DISPLAY_INTERRUPTS_H */
