/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display half of the device interrupts.
 *
 * This is the display part of the reference's intel_display_irq.c for the
 * Gen11+ path: the reset and the postinstall of the display engine sources
 * (pipes, ports, misc, the Type-C hotplug block and the south display
 * block), the handler that acknowledges every source it finds pending, the
 * pipe interrupt hooks the power wells call when a pipe's well comes up or
 * goes down, and the vblank references and waits of the panel runs.
 *
 * The top-level flow lives in ../irq.c and calls this half through the
 * operations table drv_i915_display_irq_bind() installs, with the display
 * half of the interrupt device (struct i915_display_irq) as the context.
 *
 * A pipe's registers live in the pipe's power well.  The handler enters a
 * pipe only while the pipe is open and counts itself in the pipe's
 * in-flight count; the power well's pre-disable closes the pipe and drains
 * that count before the well may go off, which is exact where a
 * whole-handler synchronize would rely on invocations never overlapping.
 */

#include "internal.h"
#include "interrupts.h"
#include "hotplug.h"
#include "opregion.h"
#include "power.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../irq.h"
#include "../mmio.h"
#include "../sync.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The display interrupt summary control and the master control's display bits. */
#define GEN11_DISPLAY_INT_CTL		0x44200U
#define GEN8_DE_PCH_IRQ			(1U << 23)
#define GEN8_DE_MISC_IRQ		(1U << 22)
#define GEN11_DE_HPD_IRQ		(1U << 21)
#define GEN8_DE_PORT_IRQ		(1U << 20)
#define GEN8_DE_PIPE_IRQ(pipe)		(1U << (16U + (unsigned)(pipe)))
#define GEN11_DISPLAY_IRQ_ENABLE	(1U << 31)

/* The PSR interrupt registers of a transcoder. */
#define TRANS_PSR_IMR(t)		(0x60814U + 0x1000U * (unsigned)(t))
#define TRANS_PSR_IIR(t)		(0x60818U + 0x1000U * (unsigned)(t))

/* The interrupt registers of a pipe. */
#define GEN8_DE_PIPE_IMR(p)		(0x44404U + 0x10U * (unsigned)(p))
#define GEN8_DE_PIPE_IIR(p)		(0x44408U + 0x10U * (unsigned)(p))
#define GEN8_DE_PIPE_IER(p)		(0x4440cU + 0x10U * (unsigned)(p))

/* The port, misc and Type-C hotplug interrupt registers. */
#define GEN8_DE_PORT_IMR		0x44444U
#define GEN8_DE_PORT_IIR		0x44448U
#define GEN8_DE_PORT_IER		0x4444cU
#define GEN8_DE_MISC_IMR		0x44464U
#define GEN8_DE_MISC_IIR		0x44468U
#define GEN8_DE_MISC_IER		0x4446cU
#define GEN11_DE_HPD_IMR		0x44474U
#define GEN11_DE_HPD_IIR		0x44478U
#define GEN11_DE_HPD_IER		0x4447cU

/* The south display block and its GMBUS source. */
#define SDEIMR				0xc4004U
#define SDEIIR				0xc4008U
#define SDEIER				0xc400cU
#define SDE_GMBUS_ICP			(1U << 23)

/* The pipe interrupt bits. */
#define GEN8_PIPE_FIFO_UNDERRUN		(1U << 31)
#define GEN8_PIPE_CDCLK_CRC_DONE	(1U << 28)
#define XELPD_PIPE_SOFT_UNDERRUN	(1U << 22)
#define XELPD_PIPE_HARD_UNDERRUN	(1U << 21)
#define GEN8_PIPE_CURSOR_FAULT		(1U << 10)
#define GEN8_PIPE_SPRITE_FAULT		(1U << 9)
#define GEN8_PIPE_PRIMARY_FAULT		(1U << 8)
#define GEN8_PIPE_PRIMARY_FLIP_DONE	(1U << 4)
#define GEN8_PIPE_VBLANK		(1U << 0)
#define GEN9_PIPE_CURSOR_FAULT		(1U << 11)
#define GEN11_PIPE_PLANE7_FAULT		(1U << 22)
#define GEN11_PIPE_PLANE6_FAULT		(1U << 21)
#define GEN11_PIPE_PLANE5_FAULT		(1U << 20)
#define GEN9_PIPE_PLANE4_FAULT		(1U << 10)
#define GEN9_PIPE_PLANE3_FAULT		(1U << 9)
#define GEN9_PIPE_PLANE2_FAULT		(1U << 8)
#define GEN9_PIPE_PLANE1_FAULT		(1U << 7)
#define GEN9_PIPE_PLANE1_FLIP_DONE	(1U << 3)

/* The plane fault bits of each display generation. */
#define GEN8_DE_PIPE_IRQ_FAULT_ERRORS	(GEN8_PIPE_CURSOR_FAULT | GEN8_PIPE_SPRITE_FAULT | GEN8_PIPE_PRIMARY_FAULT)
#define GEN9_DE_PIPE_IRQ_FAULT_ERRORS	(GEN9_PIPE_CURSOR_FAULT | GEN9_PIPE_PLANE4_FAULT | GEN9_PIPE_PLANE3_FAULT | GEN9_PIPE_PLANE2_FAULT | GEN9_PIPE_PLANE1_FAULT)
#define GEN11_DE_PIPE_IRQ_FAULT_ERRORS	(GEN9_DE_PIPE_IRQ_FAULT_ERRORS | GEN11_PIPE_PLANE7_FAULT | GEN11_PIPE_PLANE6_FAULT | GEN11_PIPE_PLANE5_FAULT)
#define RKL_DE_PIPE_IRQ_FAULT_ERRORS	(GEN9_DE_PIPE_IRQ_FAULT_ERRORS | GEN11_PIPE_PLANE5_FAULT)

/* The AUX channel bits of the port interrupt register. */
#define GEN8_AUX_CHANNEL_A		(1U << 0)
#define GEN9_AUX_CHANNEL_B		(1U << 25)
#define GEN9_AUX_CHANNEL_C		(1U << 26)
#define GEN9_AUX_CHANNEL_D		(1U << 27)
#define ICL_AUX_CHANNEL_E		(1U << 29)
#define ICL_AUX_CHANNEL_F		(1U << 28)
#define TGL_DE_PORT_AUX_DDIA		(1U << 0)
#define TGL_DE_PORT_AUX_DDIB		(1U << 1)
#define TGL_DE_PORT_AUX_DDIC		(1U << 2)
#define TGL_DE_PORT_AUX_USBC1		(1U << 8)
#define TGL_DE_PORT_AUX_USBC2		(1U << 9)
#define TGL_DE_PORT_AUX_USBC3		(1U << 10)
#define TGL_DE_PORT_AUX_USBC4		(1U << 11)
#define TGL_DE_PORT_AUX_USBC5		(1U << 12)
#define TGL_DE_PORT_AUX_USBC6		(1U << 13)
#define XELPD_DE_PORT_AUX_DDID		(1U << 12)
#define XELPD_DE_PORT_AUX_DDIE		(1U << 13)
#define DSI0_TE				(1U << 23)
#define DSI1_TE				(1U << 24)

/* The misc interrupt bits. */
#define GEN8_DE_MISC_GSE		(1U << 27)
#define GEN8_DE_EDP_PSR			(1U << 19)

/* The Type-C and Thunderbolt hotplug bits of TC1 to TC6. */
#define GEN11_DE_TC_HOTPLUG_MASK	0x003f0000U
#define GEN11_DE_TBT_HOTPLUG_MASK	0x0000003fU

/* How long a pipe's in-flight handler work may take to drain before its well goes off (100 ms). */
#define I915_IRQ_DRAIN_TIMEOUT_US	100000U

/* The pause between two looks at the in-flight counts. */
#define I915_IRQ_DRAIN_STEP_US		10U

static void i915_display_irq_set_enabled(void *context, int enabled);
static void i915_display_irq_uninstall_check(void *context);
static void i915_display_irq_reset(void *context);
static void i915_display_irq_postinstall(void *context);
static void i915_display_irq_handle(void *context, uint32_t master_ctl);
static void i915_display_irq_gse(void *context);
static void i915_pw_irq_post_enable(void *context, unsigned pipe_mask);
static int i915_pw_irq_pre_disable(void *context, unsigned pipe_mask);
static void i915_display_irq_write(struct i915_display_irq *d, uint32_t reg, uint32_t value, unsigned *counter);
static int i915_pipe_power_on(struct i915_display_irq *d, unsigned pipe);
static int i915_transcoder_power_on(struct i915_display_irq *d, unsigned transcoder);
static int i915_irqs_enabled(struct i915_display_irq *d);
static void i915_icp_irq_postinstall(struct i915_display_irq *d);
static void i915_gen8_de_irq_postinstall(struct i915_display_irq *d);
static void i915_gen8_de_irq_handler(struct i915_display_irq *d, uint32_t master_ctl);
static void i915_gen8_de_pipe_irq_handler(struct i915_display_irq *d, unsigned pipe);
static void i915_bdw_update_pipe_irq(struct i915_display_irq *d, unsigned pipe, uint32_t interrupt_mask, uint32_t enabled_irq_mask);
static void i915_vblank_snapshot(struct i915_display_irq *d, unsigned pipe, uint32_t *count);

/*
 * The display half of the interrupt flow, as the interrupt device calls it.
 *
 * The table never changes; its context is the display's struct
 * i915_display_irq.
 */
static const struct i915_irq_display_ops i915_display_irq_ops = {
	i915_display_irq_set_enabled,
	i915_display_irq_uninstall_check,
	i915_display_irq_reset,
	i915_display_irq_postinstall,
	i915_display_irq_handle,
	i915_display_irq_gse
};

/*
 * The pipe interrupt hooks of the power wells.
 *
 * The power-well code calls them through pwc->irq_ops with the display's
 * struct i915_display_irq as the context.  The table never changes.
 */
const struct i915_pw_irq_ops drv_i915_pw_irq_ops = {
	i915_pw_irq_post_enable,
	i915_pw_irq_pre_disable
};

/*
 * Binds the display half to the interrupt device.
 *
 * Fills the display interrupt state as the reference's intel_irq_install()
 * finds it (the display version, the pipe and transcoder masks of the
 * platform, the PCH the display detected, no DSI port), binds the vblank
 * delivery and the power-well hooks, and installs the display operations
 * table in place of whatever the interrupt device held.  Runs before
 * install.
 */
void
drv_i915_display_irq_bind(
	struct i915_display *display,
	struct i915_irq_dev *irq)
{
	struct i915_display_irq *d;

	d = &display->irq;

	/* Starts from an empty display interrupt state. */
	kern_memset(d, 0, sizeof(*d));

	/* Names the device's registers and the power state the pipes live in. */
	d->irq = irq;
	d->m = irq->m;
	d->pd = &display->power_domains;
	d->pwc = &display->pwc;

	/* Binds the pipes' vblank delivery and the power-well hooks that restore and stop it. */
	drv_i915_irq_vblank_init(d, &display->irq_vblank);
	display->pwc.irq_ops = &drv_i915_pw_irq_ops;
	display->pwc.irq_ctx = d;

	/*
	 * The platform: the PCH, the display version, pipes and transcoders A
	 * to D (the DSI transcoders are separate), a display, and no DSI child
	 * device in the VBT.
	 */
	d->pch = &display->pch;
	d->display_ver = (int)display->device->gt.display_ver;
	d->pipe_mask = 0xfU;
	d->cpu_transcoder_mask = 0xfU;
	d->has_display = 1;
	d->dsi_present = 0;

	/* Replaces the interrupt device's display hooks with this half. */
	irq->display_ops = &i915_display_irq_ops;
	irq->display_context = d;
}

/*
 * Unbinds the display half from the interrupt device.
 *
 * Runs after uninstall; the device's interrupt flow then touches no
 * display source.
 */
void
drv_i915_display_irq_unbind(
	struct i915_display *display,
	struct i915_irq_dev *irq)
{
	/* Only this display's table is taken away. */
	if (irq->display_context != &display->irq)
		return;

	irq->display_ops = NULL;
	irq->display_context = NULL;
}

/*
 * Prepares the vblank delivery of the pipes and binds it.
 *
 * Clears the state, creates its lock and one completion per pipe, and
 * makes it the vblank state of the display interrupt state.
 */
void
drv_i915_irq_vblank_init(
	struct i915_display_irq *d,
	struct i915_irq_vblank *v)
{
	unsigned pipe;

	/* Starts from an empty vblank state. */
	kern_memset(v, 0, sizeof(*v));

	/* Creates the lock that stands for the Linux irq_lock. */
	spin_init(&v->lock, LOCK_RANK_DEVICE, "i915-irq-lock");

	/* Creates the completion that wakes each pipe's waiter. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++)
		drv_i915_completion_init(&v->wake[pipe], "i915-vblank");

	/* The vblank state is ready and bound. */
	v->inited = 1;
	d->vbl = v;
}

/*
 * Returns the plane fault bits of a display version (gen8_de_pipe_fault_mask()).
 *
 * HAS_D12_PLANE_MINIMIZATION is Rocket Lake and Alder Lake-S only; Alder
 * Lake-P takes the display version 13 arm.
 */
uint32_t
drv_i915_gen8_de_pipe_fault_mask(
	int display_ver)
{
	/* Picks the fault bits of the display generation. */
	if (display_ver >= 13)
		return RKL_DE_PIPE_IRQ_FAULT_ERRORS;
	else if (display_ver >= 11)
		return GEN11_DE_PIPE_IRQ_FAULT_ERRORS;
	else if (display_ver >= 9)
		return GEN9_DE_PIPE_IRQ_FAULT_ERRORS;

	/* The Broadwell fault bits. */
	return GEN8_DE_PIPE_IRQ_FAULT_ERRORS;
}

/*
 * Returns the AUX channel bits of a display version (gen8_de_port_aux_mask()).
 */
uint32_t
drv_i915_gen8_de_port_aux_mask(
	int display_ver)
{
	uint32_t mask;

	/* Display version 20 has no AUX bits here. */
	if (display_ver >= 20)
		return 0U;

	/* Meteor Lake: DDI A and B. */
	if (display_ver >= 14)
		return TGL_DE_PORT_AUX_DDIA | TGL_DE_PORT_AUX_DDIB;

	/* XE_LPD: DDI A to E and Type-C 1 to 4. */
	if (display_ver >= 13) {
		return TGL_DE_PORT_AUX_DDIA |
		    TGL_DE_PORT_AUX_DDIB |
		    TGL_DE_PORT_AUX_DDIC |
		    XELPD_DE_PORT_AUX_DDID |
		    XELPD_DE_PORT_AUX_DDIE |
		    TGL_DE_PORT_AUX_USBC1 |
		    TGL_DE_PORT_AUX_USBC2 |
		    TGL_DE_PORT_AUX_USBC3 |
		    TGL_DE_PORT_AUX_USBC4;
	}

	/* Tiger Lake: DDI A to C and Type-C 1 to 6. */
	if (display_ver >= 12) {
		return TGL_DE_PORT_AUX_DDIA |
		    TGL_DE_PORT_AUX_DDIB |
		    TGL_DE_PORT_AUX_DDIC |
		    TGL_DE_PORT_AUX_USBC1 |
		    TGL_DE_PORT_AUX_USBC2 |
		    TGL_DE_PORT_AUX_USBC3 |
		    TGL_DE_PORT_AUX_USBC4 |
		    TGL_DE_PORT_AUX_USBC5 |
		    TGL_DE_PORT_AUX_USBC6;
	}

	/* Earlier displays: channel A, then B to D from Skylake, E and F on Ice Lake. */
	mask = GEN8_AUX_CHANNEL_A;
	if (display_ver >= 9)
		mask |= GEN9_AUX_CHANNEL_B | GEN9_AUX_CHANNEL_C | GEN9_AUX_CHANNEL_D;

	if (display_ver == 11)
		mask |= ICL_AUX_CHANNEL_F | ICL_AUX_CHANNEL_E;

	/* Reports the channels of the earlier display. */
	return mask;
}

/*
 * Returns the underrun bits of a display version (gen8_de_pipe_underrun_mask()).
 */
uint32_t
drv_i915_gen8_de_pipe_underrun_mask(
	int display_ver)
{
	uint32_t mask;

	/* Every display reports the FIFO underrun. */
	mask = GEN8_PIPE_FIFO_UNDERRUN;

	/* XE_LPD also reports the soft and hard underruns. */
	if (display_ver >= 13)
		mask |= XELPD_PIPE_SOFT_UNDERRUN | XELPD_PIPE_HARD_UNDERRUN;

	/* Reports the underrun bits. */
	return mask;
}

/*
 * Returns the flip-done bit of a display version (gen8_de_pipe_flip_done_mask()).
 */
uint32_t
drv_i915_gen8_de_pipe_flip_done_mask(
	int display_ver)
{
	/* Skylake and later report the flip on plane 1. */
	if (display_ver >= 9)
		return GEN9_PIPE_PLANE1_FLIP_DONE;

	/* Broadwell reports it on the primary plane. */
	return GEN8_PIPE_PRIMARY_FLIP_DONE;
}

/*
 * Masks, disables and clears every display source (gen11_display_irq_reset()).
 *
 * A pipe or transcoder whose power well is off is skipped: its registers
 * are not there.  The south display block is reset for an ICP or later PCH.
 */
void
drv_i915_gen11_display_irq_reset(
	struct i915_display_irq *d)
{
	unsigned pipe;
	unsigned transcoder;

	/* A device without a display has no display sources. */
	if (!d->has_display)
		return;

	/* Turns the display summary off first. */
	i915_display_irq_write(d, GEN11_DISPLAY_INT_CTL, 0U, &d->irq->reset_writes);

	/* Masks and clears the PSR interrupts of every powered transcoder. */
	if (d->display_ver >= 12) {
		for (transcoder = 0U; transcoder < 4U; transcoder++) {
			if ((d->cpu_transcoder_mask & (1U << transcoder)) == 0U)
				continue;
			if (!i915_transcoder_power_on(d, transcoder))
				continue;

			i915_display_irq_write(d, TRANS_PSR_IMR(transcoder), 0xffffffffU, &d->irq->reset_writes);
			i915_display_irq_write(d, TRANS_PSR_IIR(transcoder), 0xffffffffU, &d->irq->reset_writes);
		}
	}

	/* Resets the interrupt registers of every powered pipe. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if ((d->pipe_mask & (1U << pipe)) == 0U)
			continue;
		if (!i915_pipe_power_on(d, pipe))
			continue;

		drv_i915_gen3_irq_reset(d->irq, GEN8_DE_PIPE_IMR(pipe), GEN8_DE_PIPE_IIR(pipe), GEN8_DE_PIPE_IER(pipe));
	}

	/* Resets the port and misc sources. */
	drv_i915_gen3_irq_reset(d->irq, GEN8_DE_PORT_IMR, GEN8_DE_PORT_IIR, GEN8_DE_PORT_IER);
	drv_i915_gen3_irq_reset(d->irq, GEN8_DE_MISC_IMR, GEN8_DE_MISC_IIR, GEN8_DE_MISC_IER);

	/* Display version below 14: the GEN11 hotplug block, not PICA. */
	drv_i915_gen3_irq_reset(d->irq, GEN11_DE_HPD_IMR, GEN11_DE_HPD_IIR, GEN11_DE_HPD_IER);

	/* Resets the south display block of an ICP or later PCH. */
	if (d->pch != NULL && d->pch->type >= I915_PCH_ICP)
		drv_i915_gen3_irq_reset(d->irq, SDEIMR, SDEIIR, SDEIER);
}

/*
 * Enables the display sources and then the display summary (gen11_de_irq_postinstall()).
 */
void
drv_i915_gen11_de_irq_postinstall(
	struct i915_display_irq *d)
{
	/* A device without a display has no display sources. */
	if (!d->has_display)
		return;

	/* Enables the pipe, port, misc, hotplug and south display sources. */
	i915_gen8_de_irq_postinstall(d);

	/* Turns the display summary on. */
	i915_display_irq_write(d, GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE, &d->irq->postinstall_writes);
}

/*
 * Reads and acknowledges every pending display source (gen11_display_irq_handler()).
 *
 * The display summary is gated off while the sources are read and
 * acknowledged, and turned back on afterwards.
 */
void
drv_i915_gen11_display_irq_handler(
	struct i915_display_irq *d)
{
	uint32_t disp_ctl;

	/* Samples which display blocks are pending. */
	disp_ctl = drv_i915_raw_read32(d->m, GEN11_DISPLAY_INT_CTL);
	d->last_disp_ctl = disp_ctl;

	/* Serves them with the display summary off. */
	drv_i915_raw_write32(d->m, GEN11_DISPLAY_INT_CTL, 0U);
	i915_gen8_de_irq_handler(d, disp_ctl);
	drv_i915_raw_write32(d->m, GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE);
}

/*
 * Restores the interrupt registers of pipes whose power well came up
 * (gen8_irq_power_well_post_enable()).
 *
 * Each pipe gets the mask the reference keeps for it and the enables of
 * its vblank, underrun and flip-done sources, and is opened for the
 * handler again.  Nothing is written while interrupts are not enabled.
 */
void
drv_i915_gen8_irq_power_well_post_enable(
	struct i915_display_irq *d,
	unsigned pipe_mask)
{
	unsigned long flags;
	uint32_t extra_ier;
	unsigned pipe;
	int enabled;

	/* The vblank state carries the lock; without it nothing is bound. */
	if (d->vbl == NULL || !d->vbl->inited)
		return;

	/* The sources every powered pipe enables besides its unmasked ones. */
	extra_ier = GEN8_PIPE_VBLANK |
	    drv_i915_gen8_de_pipe_underrun_mask(d->display_ver) |
	    drv_i915_gen8_de_pipe_flip_done_mask(d->display_ver);

	/* Programs the pipes under the IRQ lock. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	/* Interrupts are not enabled yet: the install programs the pipes. */
	enabled = i915_irqs_enabled(d);
	if (!enabled) {
		d->vbl->skipped_irqs_disabled++;
		spin_unlock_irqrestore(&d->vbl->lock, flags);
		return;
	}

	d->vbl->post_enable_calls++;

	/* Restores each named pipe and opens it for the handler. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if ((d->pipe_mask & pipe_mask & (1U << pipe)) == 0U)
			continue;

		/* GEN8_IRQ_INIT_NDX(uncore, DE_PIPE, pipe, de_irq_mask[pipe], ~de_irq_mask[pipe] | extra_ier) */
		drv_i915_gen3_irq_init(d->irq, GEN8_DE_PIPE_IMR(pipe), d->de_irq_mask[pipe], GEN8_DE_PIPE_IER(pipe), ~d->de_irq_mask[pipe] | extra_ier, GEN8_DE_PIPE_IIR(pipe));
		d->vbl->post_imr[pipe] = d->de_irq_mask[pipe];
		d->vbl->post_ier[pipe] = ~d->de_irq_mask[pipe] | extra_ier;

		/* An open pipe may be entered by the handler again. */
		__atomic_store_n(&d->pipe_closed[pipe], 0, __ATOMIC_SEQ_CST);
	}

	spin_unlock_irqrestore(&d->vbl->lock, flags);
}

/*
 * Stops the interrupts of pipes whose power well is about to go off
 * (gen8_irq_power_well_pre_disable()).
 *
 * The reference resets the pipe's interrupt registers and then completes
 * intel_synchronize_irq() before the well goes off.  Here the pipe's
 * admission is closed (no new handler work enters its registers), its
 * source is reset, and what had already entered is drained.  The pipe stays
 * closed until the next post-enable.  Returns 0, or the drain's ETIMEDOUT
 * or EIO, in which case the caller keeps the well on.
 */
int
drv_i915_gen8_irq_power_well_pre_disable(
	struct i915_display_irq *d,
	unsigned pipe_mask)
{
	unsigned long flags;
	unsigned closing;
	unsigned pipe;
	int enabled;
	int drained;

	/* The vblank state carries the lock; without it nothing is bound. */
	if (d->vbl == NULL || !d->vbl->inited)
		return 0;

	/* Closes and resets the pipes under the IRQ lock. */
	closing = 0U;
	flags = spin_lock_irqsave(&d->vbl->lock);

	/* Interrupts are not enabled: no handler enters a pipe. */
	enabled = i915_irqs_enabled(d);
	if (!enabled) {
		d->vbl->skipped_irqs_disabled++;
		spin_unlock_irqrestore(&d->vbl->lock, flags);
		return 0;
	}

	d->vbl->pre_disable_calls++;

	/* Closes each named pipe first, then resets its interrupt registers. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if ((d->pipe_mask & pipe_mask & (1U << pipe)) == 0U)
			continue;

		/* A closed pipe is refused by the handler from here on. */
		__atomic_store_n(&d->pipe_closed[pipe], 1, __ATOMIC_SEQ_CST);
		closing |= 1U << pipe;

		/* GEN8_IRQ_RESET_NDX(uncore, DE_PIPE, pipe) */
		drv_i915_gen3_irq_reset(d->irq, GEN8_DE_PIPE_IMR(pipe), GEN8_DE_PIPE_IIR(pipe), GEN8_DE_PIPE_IER(pipe));
	}

	spin_unlock_irqrestore(&d->vbl->lock, flags);

	/* Makes sure no handler is still processing these pipes' interrupts. */
	d->vbl->sync_calls++;
	drained = drv_i915_irq_drain_pipes(d, closing, I915_IRQ_DRAIN_TIMEOUT_US);
	if (drained != 0)
		return drained;

	/* Succeeded: the pipes are closed and no handler is inside them. */
	return 0;
}

/*
 * Waits until no handler is inside the given pipes' registers.
 *
 * The pipes stay as they are, open or closed.  Returns 0, ETIMEDOUT when
 * a pipe is still busy after timeout_us, or EIO when the time base fails
 * while waiting.
 */
int
drv_i915_irq_drain_pipes(
	struct i915_display_irq *d,
	unsigned pipe_mask,
	unsigned timeout_us)
{
	unsigned waited;
	unsigned pipe;
	unsigned busy;
	unsigned inflight;
	int delay_error;

	/* Looks at the in-flight counts every 10 microseconds. */
	for (waited = 0U;; waited += I915_IRQ_DRAIN_STEP_US) {
		/* Collects the named pipes a handler is still inside. */
		busy = 0U;
		for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
			if ((pipe_mask & (1U << pipe)) == 0U)
				continue;

			inflight = __atomic_load_n(&d->pipe_inflight[pipe], __ATOMIC_SEQ_CST);
			if (inflight != 0U)
				busy |= 1U << pipe;
		}

		/* Every named pipe is free of handler work. */
		if (busy == 0U)
			return 0;

		/* The handler did not leave in time. */
		if (waited >= timeout_us) {
			if (d->vbl != NULL)
				d->vbl->drain_timeouts++;
			kern_logf("i915: IRQ drain: the handler did not leave pipe(s) 0x%x within %u us\n", busy, timeout_us);

			return ETIMEDOUT;
		}

		/* Lets the handler make progress; a failed time base is not a timeout. */
		delay_error = drv_i915_udelay(I915_IRQ_DRAIN_STEP_US);
		if (delay_error != 0) {
			if (d->vbl != NULL)
				d->vbl->drain_time_faults++;
			kern_logf("i915: IRQ drain: the time base failed while waiting (not a timeout)\n");

			return EIO;
		}
	}
}

/*
 * Takes a vblank reference on a pipe (drm_vblank_get()).
 *
 * The first reference enables the pipe's vblank interrupt
 * (bdw_enable_vblank()).  Returns 0, or EINVAL when the vblank state is not
 * bound, the pipe is out of range or interrupts are not enabled.
 */
int
drv_i915_drm_vblank_get(
	struct i915_display_irq *d,
	unsigned pipe)
{
	unsigned long flags;
	int enabled;

	/* Refuses a pipe the vblank state has no room for. */
	if (d->vbl == NULL || !d->vbl->inited)
		return EINVAL;
	if (pipe >= I915_IRQ_MAX_PIPES)
		return EINVAL;

	/* Counts the reference and enables the interrupt under the IRQ lock. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	/* Interrupts that are not enabled deliver no vblank. */
	enabled = i915_irqs_enabled(d);
	if (!enabled) {
		spin_unlock_irqrestore(&d->vbl->lock, flags);
		return EINVAL;
	}

	/* The first reference turns the delivery and the interrupt on. */
	if (d->vbl->refs[pipe] == 0U) {
		d->vbl->enable_calls[pipe]++;
		d->vbl->enabled[pipe] = 1;
		i915_bdw_update_pipe_irq(d, pipe, GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK);
	}

	d->vbl->refs[pipe]++;

	spin_unlock_irqrestore(&d->vbl->lock, flags);

	/* Succeeded: the caller holds a vblank reference. */
	return 0;
}

/*
 * Drops a vblank reference on a pipe (drm_vblank_put()).
 *
 * The last reference masks the pipe's vblank interrupt at once
 * (bdw_disable_vblank(), vblank_disable_immediate).  Adaptation: Linux still
 * disables through the vblank core after the pending vblank and event
 * processing; this path has no DRM events or workers, so it masks inside
 * the put.
 */
void
drv_i915_drm_vblank_put(
	struct i915_display_irq *d,
	unsigned pipe)
{
	unsigned long flags;

	/* Nothing is bound or the pipe is out of range. */
	if (d->vbl == NULL || !d->vbl->inited)
		return;
	if (pipe >= I915_IRQ_MAX_PIPES)
		return;

	/* Drops the reference and disables the interrupt under the IRQ lock. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	/* The last reference turns the interrupt and the delivery off. */
	if (d->vbl->refs[pipe] != 0U) {
		d->vbl->refs[pipe]--;
		if (d->vbl->refs[pipe] == 0U) {
			d->vbl->disable_calls[pipe]++;
			i915_bdw_update_pipe_irq(d, pipe, GEN8_PIPE_VBLANK, 0U);
			d->vbl->enabled[pipe] = 0;
		}
	}

	spin_unlock_irqrestore(&d->vbl->lock, flags);
}

/*
 * Waits for n new vblanks of a pipe.
 *
 * The pipe's vblank count must advance by n from the moment of the call,
 * and read_frame (the pipe's hardware frame counter, may be NULL) must
 * have advanced too: a stale pending bit delivered at unmask, another
 * pipe's interrupt or mere elapsed time never satisfy it.  Needs a vblank
 * reference; one waiter per pipe.  Returns 0, EINVAL (no reference, a pipe
 * out of range or n of 0), EBUSY (the pipe already has a waiter) or
 * ETIMEDOUT.
 */
int
drv_i915_wait_vblank(
	struct i915_display_irq *d,
	unsigned pipe,
	unsigned n,
	unsigned timeout_ms,
	uint32_t (*read_frame)(void *ctx),
	void *frame_ctx,
	uint32_t *count_seen)
{
	unsigned long flags;
	uint64_t deadline;
	uint32_t start;
	uint32_t frame0;
	uint32_t frame;
	uint32_t now;
	int woken;
	int satisfied;

	/* Refuses a wait the vblank state cannot serve. */
	if (d->vbl == NULL || !d->vbl->inited)
		return EINVAL;
	if (pipe >= I915_IRQ_MAX_PIPES)
		return EINVAL;
	if (n == 0U)
		return EINVAL;

	/* Registers the one waiter of the pipe and samples the count under the IRQ lock. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	/* A wait needs the pipe's vblank delivery to be referenced. */
	if (d->vbl->refs[pipe] == 0U) {
		spin_unlock_irqrestore(&d->vbl->lock, flags);
		return EINVAL;
	}

	/* A second waiter would re-arm the first one's wake-up. */
	if (d->vbl->waiting[pipe]) {
		d->vbl->second_waiter_refusals++;
		spin_unlock_irqrestore(&d->vbl->lock, flags);
		return EBUSY;
	}

	d->vbl->waiting[pipe] = 1;
	drv_i915_reinit_completion(&d->vbl->wake[pipe]);
	start = d->vbl->count[pipe];

	spin_unlock_irqrestore(&d->vbl->lock, flags);

	/* Samples the hardware frame counter the wait must see move. */
	frame0 = 0U;
	if (read_frame != NULL)
		frame0 = read_frame(frame_ctx);

	/* Waits in whole ticks, one tick more than the timeout asks for. */
	deadline = sched_ticks() + ((uint64_t)timeout_ms * KERN_CLOCK_HZ + 999U) / 1000U + 1U;
	satisfied = 0;
	for (;;) {
		/* Enough new vblanks, and the frame counter moved: the wait is over. */
		i915_vblank_snapshot(d, pipe, &now);
		if ((uint32_t)(now - start) >= n) {
			if (read_frame == NULL) {
				satisfied = 1;
				break;
			}

			frame = read_frame(frame_ctx);
			if (frame != frame0) {
				satisfied = 1;
				break;
			}
		}

		/* Sleeps until the next vblank of the pipe or the deadline. */
		woken = drv_i915_wait_for_completion(&d->vbl->wake[pipe], deadline);
		if (!woken)
			break;
	}

	/* Reports how many vblanks the wait saw. */
	if (count_seen != NULL) {
		i915_vblank_snapshot(d, pipe, &now);
		*count_seen = now - start;
	}

	/* The pipe has no waiter from here on. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	d->vbl->waiting[pipe] = 0;

	spin_unlock_irqrestore(&d->vbl->lock, flags);

	/* The deadline passed first. */
	if (!satisfied)
		return ETIMEDOUT;

	/* Succeeded: n new vblanks came and the frame counter moved. */
	return 0;
}

/* Tells the power wells whether interrupts are enabled (the install and uninstall call this). */
static void
i915_display_irq_set_enabled(
	void *context,
	int enabled)
{
	struct i915_display_irq *d;

	d = context;

	/* The power-well post-enable path is gated on this same flag. */
	if (d->pwc != NULL)
		d->pwc->irqs_enabled = enabled;
}

/* Warns about vblank references still held when interrupts are uninstalled. */
static void
i915_display_irq_uninstall_check(
	void *context)
{
	struct i915_display_irq *d;
	unsigned pipe;

	d = context;

	/* Without vblank state no reference can be held. */
	if (d->vbl == NULL || !d->vbl->inited)
		return;

	/* Names every pipe that still holds a reference. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if (d->vbl->refs[pipe] == 0U)
			continue;

		kern_logf("i915: WARN intel_irq_uninstall: pipe %u still holds %u vblank reference(s)\n", pipe, d->vbl->refs[pipe]);
	}
}

/* Resets the display sources for the interrupt device. */
static void
i915_display_irq_reset(
	void *context)
{
	/* The context is the display interrupt state. */
	drv_i915_gen11_display_irq_reset(context);
}

/* Enables the display sources for the interrupt device. */
static void
i915_display_irq_postinstall(
	void *context)
{
	/* The context is the display interrupt state. */
	drv_i915_gen11_de_irq_postinstall(context);
}

/* Serves the display sources for the interrupt device; the display summary names them. */
static void
i915_display_irq_handle(
	void *context,
	uint32_t master_ctl)
{
	UNUSED_PARAMETER(master_ctl);

	/* The display summary, not the master control, says which blocks are pending. */
	drv_i915_gen11_display_irq_handler(context);
}

/* Hands a graphics system event to the OpRegion (intel_opregion_asle_intr()). */
static void
i915_display_irq_gse(
	void *context)
{
	struct i915_display *display;

	/* The display interrupt state is a member of its display. */
	display = container_of((struct i915_display_irq *)context, struct i915_display, irq);

	/* Enters the OpRegion service's gated event entry. */
	drv_i915_opregion_gse_entry(display);
}

/* The power-well post-enable hook: restores the pipes' interrupts. */
static void
i915_pw_irq_post_enable(
	void *context,
	unsigned pipe_mask)
{
	/* The context is the display interrupt state. */
	drv_i915_gen8_irq_power_well_post_enable(context, pipe_mask);
}

/* The power-well pre-disable hook: stops and drains the pipes' interrupts. */
static int
i915_pw_irq_pre_disable(
	void *context,
	unsigned pipe_mask)
{
	int drained;

	/* The context is the display interrupt state. */
	drained = drv_i915_gen8_irq_power_well_pre_disable(context, pipe_mask);
	if (drained != 0)
		return drained;

	/* Succeeded: the well may go off. */
	return 0;
}

/* Writes one display interrupt register and counts the write for the diagnostics. */
static void
i915_display_irq_write(
	struct i915_display_irq *d,
	uint32_t reg,
	uint32_t value,
	unsigned *counter)
{
	/* Writes the register. */
	drv_i915_write32(d->m, reg, value);

	/* Counts it in the reset or postinstall total. */
	if (counter != NULL)
		(*counter)++;
}

/* Reports whether a pipe's power domain is enabled. */
static int
i915_pipe_power_on(
	struct i915_display_irq *d,
	unsigned pipe)
{
	int enabled;

	/* Asks the power domains about PIPE_A + pipe. */
	enabled = drv_i915_display_power_is_enabled(d->pd, (enum i915_power_domain)(I915_PW_DOMAIN_PIPE_A + pipe), d->pwc);

	/* Reports the pipe's power. */
	return enabled;
}

/* Reports whether a transcoder's power domain is enabled. */
static int
i915_transcoder_power_on(
	struct i915_display_irq *d,
	unsigned transcoder)
{
	int enabled;

	/* Asks the power domains about TRANSCODER_A + transcoder. */
	enabled = drv_i915_display_power_is_enabled(d->pd, (enum i915_power_domain)(I915_PW_DOMAIN_TRANSCODER_A + transcoder), d->pwc);

	/* Reports the transcoder's power. */
	return enabled;
}

/* Reports the interrupt device's irqs_enabled (the Linux runtime_pm.irqs_enabled). */
static int
i915_irqs_enabled(
	struct i915_display_irq *d)
{
	/* An unbound display half sees interrupts as disabled. */
	if (d->irq == NULL)
		return 0;

	/* Reports the interrupt device's flag. */
	return d->irq->irqs_enabled;
}

/* Enables the south display block's GMBUS source (icp_irq_postinstall()). */
static void
i915_icp_irq_postinstall(
	struct i915_display_irq *d)
{
	uint32_t mask;

	/* Only GMBUS is unmasked; every source is enabled. */
	mask = SDE_GMBUS_ICP;
	drv_i915_gen3_irq_init(d->irq, SDEIMR, ~mask, SDEIER, 0xffffffffU, SDEIIR);
}

/* Computes the display masks and enables the pipe, port, misc and hotplug sources (gen8_de_irq_postinstall()). */
static void
i915_gen8_de_irq_postinstall(
	struct i915_display_irq *d)
{
	uint32_t de_pipe_masked;
	uint32_t de_pipe_enables;
	uint32_t de_port_masked;
	uint32_t de_port_enables;
	uint32_t de_misc_masked;
	uint32_t de_hpd_masked;
	uint32_t de_hpd_enables;
	unsigned pipe;
	unsigned transcoder;

	/* A device without a display has no display sources. */
	if (!d->has_display)
		return;

	/* The unmasked pipe, port and misc sources. */
	de_misc_masked = GEN8_DE_EDP_PSR;
	de_pipe_masked = drv_i915_gen8_de_pipe_fault_mask(d->display_ver) | GEN8_PIPE_CDCLK_CRC_DONE;
	de_port_masked = drv_i915_gen8_de_port_aux_mask(d->display_ver);

	/* Display version below 14 with an ICP or later PCH: the ICP south block. */
	if (d->pch != NULL && d->pch->type >= I915_PCH_ICP)
		i915_icp_irq_postinstall(d);

	/* Before Ice Lake the graphics system event comes through the misc register. */
	if (d->display_ver < 11)
		de_misc_masked |= GEN8_DE_MISC_GSE;

	/* Display versions 11 to 13: the DSI tearing effect, when the VBT reports a DSI port. */
	if (d->display_ver >= 11 && d->display_ver < 14) {
		if (d->dsi_present)
			de_port_masked |= DSI0_TE | DSI1_TE;
	}

	/* The enabled pipe sources add vblank, underrun and flip done to the unmasked ones. */
	de_pipe_enables = de_pipe_masked |
	    GEN8_PIPE_VBLANK |
	    drv_i915_gen8_de_pipe_underrun_mask(d->display_ver) |
	    drv_i915_gen8_de_pipe_flip_done_mask(d->display_ver);

	/* Not GLK, BXT or BDW: no extra hotplug bits on the port register. */
	de_port_enables = de_port_masked;

	/* Keeps the masks for the diagnostics. */
	d->de_pipe_masked = de_pipe_masked;
	d->de_pipe_enables = de_pipe_enables;
	d->de_port_masked = de_port_masked;
	d->de_port_enables = de_port_enables;
	d->de_misc_masked = de_misc_masked;

	/* Checks that the PSR interrupts of every powered transcoder are clear. */
	if (d->display_ver >= 12) {
		for (transcoder = 0U; transcoder < 4U; transcoder++) {
			if ((d->cpu_transcoder_mask & (1U << transcoder)) == 0U)
				continue;
			if (!i915_transcoder_power_on(d, transcoder))
				continue;

			drv_i915_gen3_assert_iir_is_zero(d->irq, TRANS_PSR_IIR(transcoder));
		}
	}

	/* Records every pipe's mask and programs the powered pipes. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if ((d->pipe_mask & (1U << pipe)) == 0U)
			continue;

		d->de_irq_mask[pipe] = ~de_pipe_masked;

		/* A pipe whose well is off is programmed by the well's post-enable. */
		if (i915_pipe_power_on(d, pipe))
			drv_i915_gen3_irq_init(d->irq, GEN8_DE_PIPE_IMR(pipe), d->de_irq_mask[pipe], GEN8_DE_PIPE_IER(pipe), de_pipe_enables, GEN8_DE_PIPE_IIR(pipe));
	}

	/* Programs the port and misc sources. */
	drv_i915_gen3_irq_init(d->irq, GEN8_DE_PORT_IMR, ~de_port_masked, GEN8_DE_PORT_IER, de_port_enables, GEN8_DE_PORT_IIR);
	drv_i915_gen3_irq_init(d->irq, GEN8_DE_MISC_IMR, ~de_misc_masked, GEN8_DE_MISC_IER, de_misc_masked, GEN8_DE_MISC_IIR);

	/* Display versions 11 to 13: the Type-C and Thunderbolt hotplug sources, all enabled, none unmasked. */
	if (d->display_ver >= 11 && d->display_ver <= 13) {
		de_hpd_masked = 0U;
		de_hpd_enables = GEN11_DE_TC_HOTPLUG_MASK | GEN11_DE_TBT_HOTPLUG_MASK;
		drv_i915_gen3_irq_init(d->irq, GEN11_DE_HPD_IMR, ~de_hpd_masked, GEN11_DE_HPD_IER, de_hpd_enables, GEN11_DE_HPD_IIR);
	}
}

/*
 * Reads and acknowledges every display source the summary names (gen8_de_irq_handler()).
 *
 * Every enabled source is acknowledged here: an IIR bit latches, and an
 * unacknowledged source keeps the master line asserted.  The vblank wakes
 * its waiter; the rest is counted.
 */
static void
i915_gen8_de_irq_handler(
	struct i915_display_irq *d,
	uint32_t master_ctl)
{
	struct i915_display *display;
	uint32_t iir;
	unsigned pipe;

	/* Acknowledges the misc sources; an empty IIR means the summary lied. */
	if ((master_ctl & GEN8_DE_MISC_IRQ) != 0U) {
		iir = drv_i915_read32(d->m, GEN8_DE_MISC_IIR);
		if (iir != 0U) {
			drv_i915_write32(d->m, GEN8_DE_MISC_IIR, iir);
			d->de_misc_acks++;
		} else {
			d->de_lied_count++;
		}
	}

	/* Acknowledges the Type-C hotplug sources. */
	if (d->display_ver >= 11 && (master_ctl & GEN11_DE_HPD_IRQ) != 0U) {
		iir = drv_i915_read32(d->m, GEN11_DE_HPD_IIR);
		if (iir != 0U) {
			drv_i915_write32(d->m, GEN11_DE_HPD_IIR, iir);
			d->de_hpd_acks++;
		} else {
			d->de_lied_count++;
		}
	}

	/* Acknowledges the port sources. */
	if ((master_ctl & GEN8_DE_PORT_IRQ) != 0U) {
		iir = drv_i915_read32(d->m, GEN8_DE_PORT_IIR);
		if (iir != 0U) {
			drv_i915_write32(d->m, GEN8_DE_PORT_IIR, iir);
			d->de_port_acks++;
		} else {
			d->de_lied_count++;
		}
	}

	/* Serves every pipe the summary names. */
	for (pipe = 0U; pipe < I915_IRQ_MAX_PIPES; pipe++) {
		if ((d->pipe_mask & (1U << pipe)) == 0U)
			continue;
		if ((master_ctl & GEN8_DE_PIPE_IRQ(pipe)) == 0U)
			continue;

		i915_gen8_de_pipe_irq_handler(d, pipe);
	}

	/*
	 * An ICP or later PCH below display version 14 has no PICA block: the
	 * south display IIR is read and acknowledged directly, and handed to
	 * the hotplug path (icp_irq_handler()).
	 */
	if (d->pch != NULL && d->pch->type >= I915_PCH_ICP && (master_ctl & GEN8_DE_PCH_IRQ) != 0U) {
		iir = drv_i915_read32(d->m, SDEIIR);
		if (iir != 0U) {
			drv_i915_write32(d->m, SDEIIR, iir);
			d->de_pch_acks++;

			/* The hotplug path drops the event until it is started. */
			display = container_of(d, struct i915_display, irq);
			drv_i915_hpd_pch_irq(display, iir);
		} else {
			d->de_lied_count++;
		}
	}
}

/* Serves one pipe's sources if the pipe is open, counting itself in the pipe's in-flight count. */
static void
i915_gen8_de_pipe_irq_handler(
	struct i915_display_irq *d,
	unsigned pipe)
{
	unsigned long flags;
	uint32_t iir;
	uint32_t fault_errors;
	int closed;

	/*
	 * Admission: the handler counts itself in first and then checks the
	 * gate; the closing side sets the gate first and then reads the count.
	 */
	(void)__atomic_add_fetch(&d->pipe_inflight[pipe], 1U, __ATOMIC_SEQ_CST);
	closed = __atomic_load_n(&d->pipe_closed[pipe], __ATOMIC_SEQ_CST);
	if (closed) {
		(void)__atomic_sub_fetch(&d->pipe_inflight[pipe], 1U, __ATOMIC_SEQ_CST);
		d->pipe_refused[pipe]++;
		return;
	}

	/* Reads the pipe's sources; an empty IIR means the summary lied. */
	iir = drv_i915_read32(d->m, GEN8_DE_PIPE_IIR(pipe));
	if (iir == 0U) {
		d->de_lied_count++;
		(void)__atomic_sub_fetch(&d->pipe_inflight[pipe], 1U, __ATOMIC_SEQ_CST);
		return;
	}

	/* Acknowledges them. */
	drv_i915_write32(d->m, GEN8_DE_PIPE_IIR(pipe), iir);
	d->de_pipe_iir_acks[pipe]++;
	d->last_de_pipe_iir[pipe] = iir;

	/*
	 * intel_handle_vblank() -> drm_handle_vblank(): the vblank is counted
	 * and the waiter woken only while the pipe's delivery is enabled.
	 */
	if ((iir & GEN8_PIPE_VBLANK) != 0U) {
		d->de_vblank_count[pipe]++;
		if (d->vbl != NULL && d->vbl->inited) {
			flags = spin_lock_irqsave(&d->vbl->lock);

			if (d->vbl->enabled[pipe]) {
				d->vbl->count[pipe]++;
				drv_i915_complete(&d->vbl->wake[pipe]);
			}

			spin_unlock_irqrestore(&d->vbl->lock, flags);
		}
	}

	/* Counts a completed flip. */
	if ((iir & drv_i915_gen8_de_pipe_flip_done_mask(d->display_ver)) != 0U)
		d->de_flip_done_count++;

	/* Counts an underrun. */
	if ((iir & drv_i915_gen8_de_pipe_underrun_mask(d->display_ver)) != 0U)
		d->de_underrun_count++;

	/* Reports plane faults. */
	fault_errors = iir & drv_i915_gen8_de_pipe_fault_mask(d->display_ver);
	if (fault_errors != 0U) {
		d->de_fault_count++;
		kern_logf("i915: Fault errors on pipe %c: 0x%08x\n", (char)('A' + pipe), fault_errors);
	}

	/* The handler has left the pipe's registers. */
	(void)__atomic_sub_fetch(&d->pipe_inflight[pipe], 1U, __ATOMIC_SEQ_CST);
}

/*
 * Updates the kept interrupt mask of a pipe and writes it when it changed
 * (bdw_update_pipe_irq()); the caller holds the IRQ lock.
 */
static void
i915_bdw_update_pipe_irq(
	struct i915_display_irq *d,
	unsigned pipe,
	uint32_t interrupt_mask,
	uint32_t enabled_irq_mask)
{
	uint32_t new_val;
	int enabled;

	/* Enabling bits outside the mask is the caller's mistake; the reference warns. */
	if ((enabled_irq_mask & ~interrupt_mask) != 0U)
		kern_logf("i915: WARN bdw_update_pipe_irq: enabled bits outside the mask\n");

	/* Nothing is written while interrupts are not enabled. */
	enabled = i915_irqs_enabled(d);
	if (!enabled) {
		kern_logf("i915: WARN bdw_update_pipe_irq: interrupts are not enabled\n");
		return;
	}

	/* Unmasks the enabled bits of the mask and masks the others. */
	new_val = d->de_irq_mask[pipe];
	new_val &= ~interrupt_mask;
	new_val |= (~enabled_irq_mask & interrupt_mask);

	/* Writes the register only when the kept mask changed. */
	if (new_val != d->de_irq_mask[pipe]) {
		d->de_irq_mask[pipe] = new_val;
		drv_i915_write32(d->m, GEN8_DE_PIPE_IMR(pipe), d->de_irq_mask[pipe]);
		drv_i915_posting_read32(d->m, GEN8_DE_PIPE_IMR(pipe));
	}
}

/* Reads a pipe's vblank count under the IRQ lock. */
static void
i915_vblank_snapshot(
	struct i915_display_irq *d,
	unsigned pipe,
	uint32_t *count)
{
	unsigned long flags;

	/* One consistent view of the count. */
	flags = spin_lock_irqsave(&d->vbl->lock);

	*count = d->vbl->count[pipe];

	spin_unlock_irqrestore(&d->vbl->lock, flags);
}
