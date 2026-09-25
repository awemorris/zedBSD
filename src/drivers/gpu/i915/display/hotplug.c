/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hotplug path (see hotplug.h).
 *
 * Two parts live here.  The hotplug part of the Linux driver probe
 * (intel_hpd_init_pins(), gen11_hpd_irq_setup() with icp_hpd_irq_setup(),
 * intel_hpd_init(), intel_hpd_poll_disable()) programs the hotplug
 * registers from a struct i915_hotplug.  The HDMI hotplug receive path runs
 * the Linux v6.8.12 chain
 *
 *   icp_irq_handler -> intel_get_hpd_pins -> intel_hpd_irq_handler ->
 *   i915_hotplug_work_func -> intel_ddi_hotplug -> drm_helper_probe_detect ->
 *   intel_hdmi_detect (hdmi.c) -> intel_digital_port_connected
 *
 * on the device of the hotplug world, together with what the chain needs
 * around it: the connectors intel_setup_outputs() would have made, the
 * interrupt entry from gen8_de_irq_handler(), the register access, the
 * work trampoline, and the records the tests read.
 *
 * Adaptations of the Linux text (recorded, not hidden):
 *
 *   - the connectors are made when the hotplug path starts (Linux makes
 *     them in intel_ddi_init()); connector ids are their index plus one,
 *     and names follow drm's "<type>-<n>";
 *   - dig_port->hpd_pulse (intel_dp_hpd_pulse()), the DP connector detect
 *     (intel_dp_detect()) and the Type-C connected check
 *     (intel_tc_port_connected()) are not ported: each call is logged as a
 *     step, and hpd_pulse answers IRQ_HANDLED, which is what Linux answers
 *     for an eDP long pulse;
 *   - polling (drm_kms_helper_poll_*()), the uevents
 *     (drm_kms_helper_*hotplug_event()) and the GMBUS wait queue are
 *     counted;
 *   - intel_hpd_irq_setup() copies stats[].state into the register
 *     programming of the driver-probe part.
 *
 * Registers are reached through the world: the MMIO BAR of the running
 * instance, or the fake registers of a model test.
 *
 * The Linux text this file follows carries these notices:
 *
 * Copyright (c) 2008, 2012, 2015, 2023 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * (intel_hotplug.c, intel_hotplug_irq.c, intel_ddi.c -- author Eugeni
 * Dodonov <eugeni.dodonov@intel.com> -- and intel_dp.c -- author Keith
 * Packard <keithp@keithp.com>), and
 *
 * Copyright (c) 2006-2008, 2016 Intel Corporation
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 *
 * (drm_probe_helper.c -- authors Keith Packard, Eric Anholt
 * <eric@anholt.net>, Dave Airlie <airlied@linux.ie>, Jesse Barnes
 * <jesse.barnes@intel.com> -- and drm_connector.c).
 */

#include "hotplug-internal.h"
#include "hotplug.h"
#include "gmbus.h"
#include "hdmi.h"
#include "power.h"
#include "../mmio.h"
#include <kern/kcrt.h>

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The HPD interrupts a pin may raise within a detection period before it counts as a storm. */
#define HPD_STORM_DEFAULT_THRESHOLD	50

/* The storm detection period, the storm re-enable delay and the retry delay, in milliseconds. */
#define HPD_STORM_DETECT_PERIOD		1000
#define HPD_STORM_REENABLE_DELAY	(2 * 60 * 1000)
#define HPD_RETRY_DELAY			1000

/* The first port with an HPD pin of its own on display version 13 (PORT_D_XELPD = PORT_TC5). */
#define I915_HPD_PORT_D_XELPD		7

/* The VBT child device type bit of a connector inside the machine (DEVICE_TYPE_INTERNAL_CONNECTOR). */
#define I915_HPD_DEVICE_TYPE_INTERNAL	0x1000u

/* The registers the hotplug world routes itself (SDEISR, SHOTPLUG_CTL_DDI/TC, GMBUS0..5). */
#define I915_HPD_REG_SDEISR		0xc4000u
#define I915_HPD_REG_SHOTPLUG_CTL_DDI	0xc4030u
#define I915_HPD_REG_SHOTPLUG_CTL_TC	0xc4034u
#define I915_HPD_REG_GMBUS0		0xc5100u
#define I915_HPD_REG_GMBUS1		0xc5104u
#define I915_HPD_REG_GMBUS2		0xc5108u
#define I915_HPD_REG_GMBUS3		0xc510cu
#define I915_HPD_REG_GMBUS5		0xc5120u

/* How often the stop polls for interrupt entries still in flight. */
#define I915_HPD_DRAIN_SPINS		1000000u

/* How long a synchronous cancel waits for a running callback, in seconds. */
#define I915_HPD_SYNC_SECONDS		5u

static struct i915_hpd_world *i915_hpd_display_world(struct i915_display *display);
static uint32_t i915_hpd_uncore_rmw(struct i915_mmio *m, uint32_t reg, uint32_t clear, uint32_t set);
static uint32_t i915_gen11_tc_hotplug(int pin);
static uint32_t i915_gen11_tbt_hotplug(int pin);
static uint32_t i915_gen11_hotplug_ctl_enable(int pin);
static uint32_t i915_sde_ddi_hotplug_icp(int pin);
static uint32_t i915_sde_tc_hotplug_icp(int pin);
static uint32_t i915_shotplug_ctl_ddi_hpd_enable(int pin);
static uint32_t i915_icp_tc_hpd_enable(int pin);
static int i915_is_tc_pin(int pin);
static int i915_is_ddi_pin(int pin);
static void i915_hpd_irqs(const struct i915_hotplug *hp, const uint32_t *table, uint32_t *enabled, uint32_t *hotplug);
static uint32_t i915_hotplug_mask(uint32_t (*fn)(int), int (*applies)(int));
static uint32_t i915_hotplug_enables(const struct i915_hotplug *hp, uint32_t (*fn)(int), int (*applies)(int));
static void i915_gen11_hpd_irq_setup(struct i915_hotplug *hp, struct i915_mmio *m, int pch_type, int intel_irqs_enabled);
static enum hpd_pin i915_connector_hpd_pin(struct intel_connector *connector);
static bool i915_hpd_irq_storm_detect(struct drm_i915_private *dev_priv, enum hpd_pin pin, bool long_hpd);
static void i915_hpd_irq_storm_switch_to_polling(struct drm_i915_private *dev_priv);
static void i915_hpd_irq_storm_reenable_work(struct work_struct *work);
static enum intel_hotplug_state i915_hotplug_detect_connector(struct intel_connector *connector);
static bool i915_encoder_has_hpd_pulse(struct intel_encoder *encoder);
static void i915_digport_work_func(struct work_struct *work);
static void i915_hotplug_work_func(struct work_struct *work);
static void i915_hpd_poll_init_work(struct work_struct *work);
static bool i915_icp_ddi_port_hotplug_long_detect(enum hpd_pin pin, u32 val);
static bool i915_icp_tc_port_hotplug_long_detect(enum hpd_pin pin, u32 val);
static void i915_get_hpd_pins(struct drm_i915_private *dev_priv, u32 *pin_mask, u32 *long_mask, u32 hotplug_trigger, u32 dig_hotplug_reg, const u32 hpd[HPD_NUM_PINS], bool long_pulse_detect(enum hpd_pin pin, u32 val));
static enum intel_hotplug_state i915_ddi_hotplug(struct intel_encoder *encoder, struct intel_connector *connector);
static bool i915_lpt_digital_port_connected(struct intel_encoder *encoder);
static int i915_hdmi_reset_link(struct intel_encoder *encoder, struct drm_modeset_acquire_ctx *ctx);
static enum connector_status i915_drm_helper_probe_detect_ctx(struct drm_connector *connector, bool force);
static const char *i915_hpd_status_name(int status);
static enum irqreturn i915_hpd_dp_pulse_step(struct intel_digital_port *dig_port, bool long_hpd);
static int i915_hpd_dp_detect_step(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx, bool force);
static bool i915_hpd_tc_connected_step(struct intel_encoder *encoder);
static enum intel_hotplug_state i915_hpd_hotplug_recorded(struct intel_encoder *encoder, struct intel_connector *connector);
static void i915_hpd_make_objects(struct i915_hpd_world *world, const struct i915_display_nogem *nogem);

/*
 * Test checkpoints.
 *
 * A model test starts the hotplug path on a register model instead of the
 * MMIO BAR (the fake argument of drv_i915_hpd_start()); the test build
 * defines these to answer the model's register reads and take its writes.
 * They are weak so that a production kernel links without them; production
 * never starts a model instance, so world->hpd.fake is NULL there and no
 * call site is reached.  A model without them reads 0 and drops writes.
 */
extern u32 drv_i915_hpd_model_read(struct i915_hpd_fake_regs *fake, u32 reg) __attribute__((weak));
extern void drv_i915_hpd_model_write(struct i915_hpd_fake_regs *fake, u32 reg, u32 val) __attribute__((weak));
extern void drv_i915_hpd_model_rmw_write(struct i915_hpd_fake_regs *fake, u32 reg, u32 val) __attribute__((weak));

/*
 * The connector callbacks of a DP or eDP connector: no forced detect.
 *
 * They never change, so every DP connector of every world shares them.
 */
static const struct drm_connector_funcs i915_hpd_dp_connector_funcs = {
	NULL
};

/*
 * The connector helper callbacks of a DP or eDP connector: the unported
 * intel_dp_detect() as a recorded step.
 */
static const struct drm_connector_helper_funcs i915_hpd_dp_helper_funcs = {
	i915_hpd_dp_detect_step
};

/*
 * The connector helper callbacks of an HDMI connector: no context-aware
 * detect, so the forced detect of its connector callbacks is used.
 */
static const struct drm_connector_helper_funcs i915_hpd_hdmi_helper_funcs = {
	NULL
};

/*
 * ==== The hotplug world ====
 */

/*
 * Allocates the display's hotplug world, zeroed.
 *
 * Returns 0, or ENOMEM.
 */
int
drv_i915_hpd_world_create(
	struct i915_display *display)
{
	struct i915_hpd_world *world;

	/* Allocates the world; a zero world has run no instance yet. */
	world = kern_calloc(1U, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* Publishes the world to the display. */
	display->hpd_world = world;

	/* Succeeded: the display has its hotplug world. */
	return 0;
}

/*
 * Frees the display's hotplug world.
 *
 * The hotplug path must have been stopped.
 */
void
drv_i915_hpd_world_destroy(
	struct i915_display *display)
{
	/* A display that never had a world has nothing to free. */
	if (display->hpd_world == NULL)
		return;

	/* Frees the world and forgets it. */
	kern_free(display->hpd_world);
	display->hpd_world = NULL;
}

/*
 * ==== The hotplug part of the Linux driver probe ====
 */

/*
 * Returns the HPD pin of a DDI port (intel_ddi_init(): xelpd_hpd_pin(),
 * tgl_hpd_pin(), intel_hpd_pin_default()).
 */
int
drv_i915_ddi_hpd_pin(
	int display_ver,
	int port)
{
	/* Display version 13 gives ports from D_XELPD on pins from D. */
	if (display_ver >= 13 && port >= I915_HPD_PORT_D_XELPD)
		return I915_HPD_PORT_D + port - I915_HPD_PORT_D_XELPD;

	/* Display version 12 gives the Type-C ports the TC pins. */
	if (display_ver >= 12 && port >= I915_PORT_TC1)
		return I915_HPD_PORT_TC1 + port - I915_PORT_TC1;

	/* Every other port has the pin of the same letter. */
	return I915_HPD_PORT_A + port - I915_PORT_A;
}

/*
 * Fills the pin tables of the hotplug state (intel_hpd_init_pins():
 * hpd_gen11 and hpd_icp).
 */
void
drv_i915_hpd_init_pins(
	struct i915_hotplug *hp,
	int display_ver,
	int pch_type)
{
	int pin;

	/* Starts with no pin raising anything. */
	kern_memset(hp->hpd, 0, sizeof(hp->hpd));
	kern_memset(hp->pch_hpd, 0, sizeof(hp->pch_hpd));

	/* hpd->hpd = hpd_gen11 (DISPLAY_VER >= 11, < 14): the TC and TBT bits of each TC pin. */
	if (display_ver >= 11 && display_ver < 14) {
		for (pin = I915_HPD_PORT_TC1; pin <= I915_HPD_PORT_TC6; pin++)
			hp->hpd[pin] = i915_gen11_tc_hotplug(pin) | i915_gen11_tbt_hotplug(pin);
	}

	/* hpd->pch_hpd = hpd_icp (PCH_ICP <= type < PCH_DG1): the SDE bits of the DDI and TC pins. */
	if (pch_type >= I915_PCH_ICP) {
		for (pin = I915_HPD_PORT_A; pin <= I915_HPD_PORT_C; pin++)
			hp->pch_hpd[pin] = i915_sde_ddi_hotplug_icp(pin);
		for (pin = I915_HPD_PORT_TC1; pin <= I915_HPD_PORT_TC6; pin++)
			hp->pch_hpd[pin] = i915_sde_tc_hotplug_icp(pin);
	}

	/* The tables are ready for the interrupt setup. */
	hp->pins_inited = 1;
}

/*
 * Programs the hotplug interrupts from the pins' state (intel_hpd_irq_setup()
 * for storm masking and re-enable).
 */
void
drv_i915_hpd_irq_setup(
	struct i915_hotplug *hp,
	struct i915_mmio *m,
	int pch_type,
	int intel_irqs_enabled)
{
	/* Counts the setup, then programs the registers. */
	hp->irq_setups++;
	i915_gen11_hpd_irq_setup(hp, m, pch_type, intel_irqs_enabled);
}

/*
 * Enables every pin and programs the hotplug interrupts (intel_hpd_init()).
 */
void
drv_i915_hpd_init(
	struct i915_hotplug *hp,
	struct i915_mmio *m,
	int display_ver,
	int pch_type,
	int display_irqs_enabled,
	int intel_irqs_enabled)
{
	int pin;

	/* Fills the pin tables the first time. */
	if (!hp->pins_inited)
		drv_i915_hpd_init_pins(hp, display_ver, pch_type);

	/* Clears every pin's statistics and enables it. */
	for (pin = I915_HPD_NONE; pin < I915_HPD_NUM_PINS; pin++) {
		hp->count[pin] = 0u;
		hp->state[pin] = I915_HPD_ENABLED;
	}

	/* intel_hpd_irq_setup(): only with display_irqs_enabled and a hotplug func. */
	if (!display_irqs_enabled || display_ver < 11) {
		hp->irq_setup_skipped = 1;
		return;
	}

	/* Counts the setup, then programs the registers. */
	hp->irq_setups++;
	i915_gen11_hpd_irq_setup(hp, m, pch_type, intel_irqs_enabled);
}

/*
 * Disables connector polling (intel_hpd_poll_disable()).
 *
 * i915_hpd_poll_init_work() runs inline where Linux queues it: with polling
 * disabled it takes a DISPLAY_CORE reference for the connector detection
 * it would run.  No DRM connectors exist here, and mode_config.poll_enabled
 * is still false (drm_kms_helper_poll_init() comes with the registration),
 * so i915_hpd_poll_detect_connectors() does nothing.
 */
void
drv_i915_hpd_poll_disable(
	struct i915_hotplug *hp,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *c)
{
	/* Polling is off. */
	hp->poll_enabled = 0;

	/* Runs the poll-init work: one DISPLAY_CORE reference taken and dropped. */
	hp->poll_init_works++;
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_DISPLAY_CORE, c);
	hp->poll_core_gets++;
	drv_i915_display_power_put(pd, I915_PW_DOMAIN_DISPLAY_CORE, c);
}

/*
 * ==== The hotplug receive path ====
 */

/*
 * Starts the hotplug receive path and opens its interrupt entry.
 *
 * Makes the connectors of the encoders the output setup found, prepares
 * the Linux hotplug state and its work queues, and enables every pin.  A
 * model test passes fake registers instead of the MMIO BAR.  Returns 0,
 * EINVAL for a started path or a missing argument, EBUSY when the other
 * kind of instance ran in this boot, or ENOMEM.
 */
int
drv_i915_hpd_start(
	struct i915_display *display,
	struct i915_hotplug *hp,
	struct i915_mmio *m,
	const struct i915_display_nogem *nogem,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *c,
	int pch_type,
	int intel_irqs_enabled,
	struct i915_hpd_fake_regs *fake)
{
	struct i915_hpd_world *world;
	struct drm_i915_private *i915;
	enum hpd_pin pin;
	int error;
	const char *hdmi_name;
	const char *backend;

	/* Refuses a display without a world. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return EINVAL;

	/* Refuses a second start and an instance with nothing to run on. */
	if (world->hpd.started)
		return EINVAL;
	if (hp == NULL || nogem == NULL)
		return EINVAL;
	if (m == NULL && fake == NULL)
		return EINVAL;

	/*
	 * The model tests must not take over the state the device's own
	 * interrupt handler reaches: while the hardware's interrupts are
	 * installed, an SDE interrupt would enter the model instance (and its
	 * locks) on any CPU.  Once a real instance has run in this boot, a
	 * model instance is refused; the model tests then say so and do not
	 * run.
	 */
	if (fake != NULL && world->i915_hpd_real_seen)
		return EBUSY;
	if (fake == NULL && world->i915_hpd_model_seen)
		return EBUSY;

	/* Clears the instance and the device of the Linux text. */
	kern_memset(&world->hpd, 0, sizeof(world->hpd));
	kern_memset(&world->hpd_i915, 0, sizeof(world->hpd_i915));
	i915 = &world->hpd_i915;

	/* Records what the instance runs on. */
	world->hpd.hp = hp;
	world->hpd.m = m;
	world->hpd.pd = pd;
	world->hpd.c = c;
	world->hpd.fake = fake;
	world->hpd.pch_type = pch_type;
	world->hpd.irqs_enabled = intel_irqs_enabled;

	/* Creates the unordered work queue. */
	error = drv_i915_workqueue_create(&world->hpd_unordered_wq.wq, "i915-unordered");
	if (error != 0)
		return ENOMEM;

	/* Creates the timer queue the delayed works of the unordered queue are armed on. */
	error = drv_i915_timer_queue_create(&world->hpd_unordered_wq.tq, &world->hpd_unordered_wq.wq, "i915-unordered-timer");
	if (error != 0) {
		drv_i915_workqueue_destroy(&world->hpd_unordered_wq.wq);
		return ENOMEM;
	}

	/* Creates the DP work queue the digital-port work runs on. */
	error = drv_i915_workqueue_create(&world->hpd_dp_wq.wq, "i915-dp");
	if (error != 0) {
		drv_i915_timer_queue_destroy(&world->hpd_unordered_wq.tq);
		drv_i915_workqueue_destroy(&world->hpd_unordered_wq.wq);
		return ENOMEM;
	}

	/* The stop destroys the queues from here on. */
	world->hpd.wq_created = 1;

	/* Prepares the device's locks. */
	spin_init(&i915->irq_lock, LOCK_RANK_DEVICE, "i915-irq");
	(void)mutex_init(&i915->drm.mode_config.mutex, LOCK_RANK_DEVICE, "drm-mode-config");

	/*
	 * Binds the queues, marks the display interrupts on, and gives the
	 * device a hotplug function (gen11_hpd_funcs: any non-NULL value) and
	 * the pin tables of intel_hpd_init_pins().
	 */
	i915->unordered_wq = &world->hpd_unordered_wq;
	i915->display.hotplug.dp_wq = &world->hpd_dp_wq;
	i915->display_irqs_enabled = true;
	i915->display.funcs.hotplug = i915;
	i915->display.hotplug.hpd = hp->hpd;
	i915->display.hotplug.pch_hpd = hp->pch_hpd;

	/* Prepares the hotplug works and the storm settings. */
	i915_hpd_intel_hpd_init_early(i915);

	/* Forgets the buses and the EDIDs of a previous instance. */
	drv_i915_hpd_gmbus_forget(world);
	drv_i915_hpd_edid_forget(world);

	/* Makes the connectors of the encoders the output setup found. */
	i915_hpd_make_objects(world, nogem);

	/* intel_hpd_init(): stats reset, every pin enabled (the registers were programmed at P7). */
	for (pin = HPD_NONE + 1; pin < HPD_NUM_PINS; pin++) {
		i915->display.hotplug.stats[pin].count = 0;
		i915->display.hotplug.stats[pin].state = HPD_ENABLED;
	}

	/* The instance runs; each kind of instance excludes the other for the rest of the boot. */
	world->hpd.started = 1;
	world->hpd.model = 0;
	if (fake != NULL) {
		world->hpd.model = 1;
		world->i915_hpd_model_seen = 1;
	} else {
		world->i915_hpd_real_seen = 1;
	}

	/* Opens the interrupt entry: an SDE interrupt now enters the Linux handler. */
	__atomic_store_n(&world->hpd.live, 1, __ATOMIC_SEQ_CST);

	/* Logs the connectors and the storm settings the path runs with. */
	hdmi_name = "-";
	if (world->hpd.hdmi >= 0)
		hdmi_name = world->hpd_conn_names[world->hpd.hdmi];
	backend = "mmio";
	if (fake != NULL)
		backend = "model";
	kern_logf("i915: hpd start: connectors=%u hdmi=%s storm_threshold=%u short_storm=%u backend=%s\n", world->hpd.num, hdmi_name, i915->display.hotplug.hpd_storm_threshold, i915->display.hotplug.hpd_short_storm_enabled, backend);

	/* Succeeded: the hotplug path runs. */
	return 0;
}

/*
 * Handles the PCH branch of gen8_de_irq_handler() (interrupt context).
 *
 * A model instance is never fed by the hardware: the interrupt is only
 * counted.
 */
void
drv_i915_hpd_pch_irq(
	struct i915_display *display,
	uint32_t sde_iir)
{
	struct i915_hpd_world *world;

	/* A display without a world has no hotplug path. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return;

	/* Counts a hardware interrupt that arrived while a model ran; its own counters stay untouched. */
	if (world->hpd.fake != NULL) {
		world->i915_hpd_hw_irqs_during_model++;
		return;
	}

	/* Enters the Linux handler. */
	drv_i915_hpd_icp_entry(world, sde_iir);
}

/*
 * Enters the Linux handler for one SDE interrupt, unless the entry is
 * closed, and records a hotplug interrupt.
 *
 * The caller runs with interrupts disabled, as the Linux handler does.
 */
void
drv_i915_hpd_icp_entry(
	struct i915_hpd_world *world,
	uint32_t sde_iir)
{
	unsigned n;
	int live;

	/* Counts the entry in flight before looking at the gate, so the stop can wait for it. */
	__atomic_add_fetch(&world->hpd.inflight, 1u, __ATOMIC_SEQ_CST);
	live = __atomic_load_n(&world->hpd.live, __ATOMIC_SEQ_CST);
	if (!live) {
		world->hpd.irq_dropped++;
		__atomic_sub_fetch(&world->hpd.inflight, 1u, __ATOMIC_SEQ_CST);
		return;
	}

	/* Counts the entry and forgets what the previous one read. */
	world->hpd.irq_entries++;
	world->hpd.cur_dig = 0u;
	if (sde_iir & SDE_DDI_HOTPLUG_MASK_ICP)
		world->hpd.ddi_triggers++;

	/* Runs the Linux handler. */
	i915_hpd_icp_irq_handler(&world->hpd_i915, sde_iir);

	/* Records a hotplug interrupt while there is room. */
	n = world->hpd.n_irq;
	if (n < I915_HPD_MAX_IRQ_RECORDS && (sde_iir & (SDE_DDI_HOTPLUG_MASK_ICP | SDE_TC_HOTPLUG_MASK_ICP))) {
		world->hpd.irq[n].tick = sched_ticks();
		world->hpd.irq[n].sde_iir = sde_iir;
		world->hpd.irq[n].shotplug_ddi = world->hpd.cur_dig;
		world->hpd.irq[n].event_bits_after = world->hpd_i915.display.hotplug.event_bits;
		world->hpd.n_irq = n + 1u;
	}

	/* The entry has left. */
	__atomic_sub_fetch(&world->hpd.inflight, 1u, __ATOMIC_SEQ_CST);
}

/*
 * Stops the hotplug receive path.
 *
 * Closes the interrupt entry, waits for the entries in flight, cancels the
 * hotplug works and destroys their queues.
 */
void
drv_i915_hpd_stop(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	unsigned spins;
	unsigned inflight;

	/* A path that is not running has nothing to stop. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return;
	if (!world->hpd.started)
		return;

	/* Closes the interrupt entry: a new interrupt is dropped. */
	__atomic_store_n(&world->hpd.live, 0, __ATOMIC_SEQ_CST);

	/* Waits, boundedly, for the entries that were already inside. */
	spins = 0u;
	for (;;) {
		inflight = __atomic_load_n(&world->hpd.inflight, __ATOMIC_SEQ_CST);
		if (inflight == 0u)
			break;
		if (spins >= I915_HPD_DRAIN_SPINS)
			break;
		spins++;
		__asm__ volatile("pause");
	}

	/* Cancels the hotplug works and waits for a running one. */
	i915_hpd_intel_hpd_cancel_work(&world->hpd_i915);

	/* Destroys the work queues. */
	if (world->hpd.wq_created) {
		drv_i915_workqueue_destroy(&world->hpd_dp_wq.wq);
		drv_i915_timer_queue_destroy(&world->hpd_unordered_wq.tq);
		drv_i915_workqueue_destroy(&world->hpd_unordered_wq.wq);
		world->hpd.wq_created = 0;
	}

	/* The path no longer runs. */
	world->hpd.started = 0;

	/* Logs what the path handled. */
	kern_logf("i915: hpd stop: irq entries=%u dropped=%u works hotplug=%u digport=%u reenable=%u events=%u warnings=%u\n", world->hpd.irq_entries, world->hpd.irq_dropped, world->hpd.hotplug_works, world->hpd.digport_works, world->hpd.reenable_works, world->hpd.hotplug_events, world->hpd.warnings);
}

/*
 * Detects a connector the way its first probe does, under
 * mode_config.mutex.
 *
 * Returns the connector status (enum connector_status, 1 to 3), or
 * EINVAL for a path that is not running or a connector it does not have.
 */
int
drv_i915_hpd_probe_connector(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;
	struct intel_connector *ic;
	int status;

	/* Refuses a path that is not running and a connector it does not have. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return EINVAL;
	if (!world->hpd.started || idx >= world->hpd.num)
		return EINVAL;

	/* Detects the connector and adopts the status unless it is forced. */
	ic = &world->hpd_conns[idx];
	mutex_lock(&world->hpd_i915.drm.mode_config.mutex);

	status = i915_hpd_drm_helper_probe_detect(&ic->base, NULL, false);
	if (!ic->base.force)
		ic->base.status = status;

	mutex_unlock(&world->hpd_i915.drm.mode_config.mutex);

	/* Succeeded: reports the connector status. */
	return status;
}

/*
 * ==== What the path saw, for the diagnostics and the tests ====
 */

/*
 * Returns a connector's name, or "-" for a connector the path does not have.
 */
const char *
drv_i915_hpd_connector_name(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;

	/* A connector outside the path has no name. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return "-";
	if (idx >= world->hpd.num)
		return "-";

	/* Succeeded: reports the name the start gave the connector. */
	return world->hpd_conn_names[idx];
}

/*
 * Copies out what the hotplug path saw and did.
 */
void
drv_i915_hpd_summary(
	struct i915_display *display,
	struct i915_hpd_summary *s)
{
	struct i915_hpd_world *world;

	/* A display without a world reports an empty summary. */
	kern_memset(s, 0, sizeof(*s));
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return;

	/* Whether the path runs, and its connectors. */
	s->started = world->hpd.started;
	s->live = world->hpd.live;
	s->num_connectors = world->hpd.num;

	/* The interrupt counters. */
	s->irq_entries = world->hpd.irq_entries;
	s->irq_dropped = world->hpd.irq_dropped;
	s->ddi_triggers = world->hpd.ddi_triggers;
	s->gmbus_irqs = world->hpd.gmbus_irqs;

	/* The work counters. */
	s->hotplug_works = world->hpd.hotplug_works;
	s->digport_works = world->hpd.digport_works;
	s->reenable_works = world->hpd.reenable_works;
	s->retries_armed = world->hpd.retries_armed;

	/* The steps and events. */
	s->hpd_pulse_steps = world->hpd.hpd_pulse_steps;
	s->hotplug_events = world->hpd.hotplug_events;
	s->irq_setups = world->hpd.irq_setups;
	s->storms = world->hpd.storms;
	s->warnings = world->hpd.warnings;

	/* How many records were filled. */
	s->irq_records = world->hpd.n_irq;
	s->hotplug_records = world->hpd.n_hot;

	/* The HDMI connector's transitions and where it stands now. */
	s->to_connected = world->hpd.to_connected;
	s->to_disconnected = world->hpd.to_disconnected;
	s->hdmi_connector = world->hpd.hdmi;
	s->hdmi_status = 0;
	s->hdmi_epoch = 0ull;
	if (world->hpd.hdmi >= 0) {
		s->hdmi_status = (int)world->hpd_conns[world->hpd.hdmi].base.status;
		s->hdmi_epoch = (unsigned long long)world->hpd_conns[world->hpd.hdmi].base.epoch_counter;
	}
}

/*
 * Returns hotplug interrupt record i, or NULL past the last one.
 */
const struct i915_hpd_irq_record *
drv_i915_hpd_irq_record(
	struct i915_display *display,
	unsigned i)
{
	struct i915_hpd_world *world;

	/* A record not filled does not exist. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return NULL;
	if (i >= world->hpd.n_irq)
		return NULL;

	/* Succeeded: reports the record. */
	return &world->hpd.irq[i];
}

/*
 * Returns encoder->hotplug() record i, or NULL past the last one.
 */
const struct i915_hpd_hotplug_record *
drv_i915_hpd_hotplug_record(
	struct i915_display *display,
	unsigned i)
{
	struct i915_hpd_world *world;

	/* A record not filled does not exist. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return NULL;
	if (i >= world->hpd.n_hot)
		return NULL;

	/* Succeeded: reports the record. */
	return &world->hpd.hot[i];
}

/*
 * Returns the pins whose hotplug event the hotplug work has not handled.
 */
uint32_t
drv_i915_hpd_event_bits(
	struct i915_display *display)
{
	struct i915_hpd_world *world;

	/* A display without a world has no events. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return 0u;

	/* Succeeded: reports display.hotplug.event_bits. */
	return world->hpd_i915.display.hotplug.event_bits;
}

/*
 * Returns the pins the hotplug work will detect again.
 */
uint32_t
drv_i915_hpd_retry_bits(
	struct i915_display *display)
{
	struct i915_hpd_world *world;

	/* A display without a world has no retries. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return 0u;

	/* Succeeded: reports display.hotplug.retry_bits. */
	return world->hpd_i915.display.hotplug.retry_bits;
}

/*
 * Returns a pin's state (display.hotplug.stats[pin].state), or -1 for a pin
 * that does not exist.
 */
int
drv_i915_hpd_pin_state(
	struct i915_display *display,
	int pin)
{
	struct i915_hpd_world *world;

	/* A pin outside the table has no state. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return -1;
	if (pin <= HPD_NONE || pin >= HPD_NUM_PINS)
		return -1;

	/* Succeeded: reports the pin's state. */
	return (int)world->hpd_i915.display.hotplug.stats[pin].state;
}

/*
 * Returns a pin's interrupt count of the current storm period, or -1 for a
 * pin that does not exist.
 */
int
drv_i915_hpd_pin_count(
	struct i915_display *display,
	int pin)
{
	struct i915_hpd_world *world;

	/* A pin outside the table has no count. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return -1;
	if (pin <= HPD_NONE || pin >= HPD_NUM_PINS)
		return -1;

	/* Succeeded: reports the pin's count. */
	return world->hpd_i915.display.hotplug.stats[pin].count;
}

/*
 * Returns a connector's polling mode (drm_connector.polled), or -1 for a
 * connector the path does not have.
 */
int
drv_i915_hpd_connector_polled(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;

	/* A connector outside the path has no polling mode. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return -1;
	if (idx >= world->hpd.num)
		return -1;

	/* Succeeded: reports the polling mode. */
	return world->hpd_conns[idx].base.polled;
}

/*
 * Returns a connector's status (enum connector_status), or -1 for a
 * connector the path does not have.
 */
int
drv_i915_hpd_connector_status(
	struct i915_display *display,
	unsigned idx)
{
	struct i915_hpd_world *world;

	/* A connector outside the path has no status. */
	world = i915_hpd_display_world(display);
	if (world == NULL)
		return -1;
	if (idx >= world->hpd.num)
		return -1;

	/* Succeeded: reports the status. */
	return (int)world->hpd_conns[idx].base.status;
}

/*
 * ==== The display lease's topology events ====
 */

/*
 * Reports the topology sequence of the display lease.
 *
 * It is the events operation of the resident display operations.
 */
int
drv_i915_hpd_events(
	void *device,
	void *session,
	uint64_t *sequence)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(session);

	/* XXX: no topology change is ever published. */
	*sequence = 1U;

	/* Succeeded: the sequence never moves. */
	return 0;
}

/*
 * ==== What the Linux text reaches in the world ====
 */

/*
 * Logs and counts a Linux warning, and reports its condition.
 */
int
drv_i915_hpd_warn(
	struct i915_hpd_world *world,
	int cond,
	const char *what,
	const char *where)
{
	/* A condition that does not hold is no warning. */
	if (!cond)
		return cond;

	/* Counts and logs the warning. */
	world->hpd.warnings++;
	kern_logf("i915: hpd WARN (%s): %s\n", where, what);

	/* Succeeded: reports the condition that held. */
	return cond;
}

/*
 * Returns the deadline a synchronous cancel of a hotplug work waits until.
 */
uint64_t
drv_i915_hpd_sync_deadline(void)
{
	uint64_t now;
	uint64_t deadline;

	/* Five seconds of scheduler ticks from now. */
	deadline = 0;
	now = sched_ticks();
	(void)kern_deadline_after(now, I915_HPD_SYNC_SECONDS * KERN_CLOCK_HZ, &deadline);

	/* Succeeded: reports the deadline. */
	return deadline;
}

/*
 * Runs a work item of the Linux text, counting the hotplug works.
 *
 * It is the driver work callback every hotplug work is prepared with; each
 * of the three counted works is recognized by its callback, and its world
 * by where the work sits in it.
 */
void
drv_i915_hpd_work_trampoline(
	void *context)
{
	struct work_struct *work;
	struct i915_hpd_world *world;

	/* The driver work hands back the Linux work item. */
	work = context;

	/* Counts the run of the hotplug, digital-port or storm re-enable work. */
	if (work->func == i915_hotplug_work_func) {
		world = container_of(work, struct i915_hpd_world, hpd_i915.display.hotplug.hotplug_work.work);
		world->hpd.hotplug_works++;
	} else if (work->func == i915_digport_work_func) {
		world = container_of(work, struct i915_hpd_world, hpd_i915.display.hotplug.dig_port_work);
		world->hpd.digport_works++;
	} else if (work->func == i915_hpd_irq_storm_reenable_work) {
		world = container_of(work, struct i915_hpd_world, hpd_i915.display.hotplug.reenable_work.work);
		world->hpd.reenable_works++;
	}

	/* Runs the Linux callback. */
	work->func(work);
}

/*
 * Reads a register of the running instance: the MMIO BAR, or a model's
 * fake registers.
 */
u32
drv_i915_hpd_read(
	struct i915_hpd_world *world,
	u32 reg)
{
	u32 value;

	/* A model answers from the test build's register model. */
	if (world->hpd.fake != NULL) {
		value = 0u;
		if (drv_i915_hpd_model_read != NULL)
			value = drv_i915_hpd_model_read(world->hpd.fake, reg);
		return value;
	}

	/* Reads the BAR. */
	value = drv_i915_read32(world->hpd.m, reg);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Writes a register of the running instance and returns the value written.
 */
u32
drv_i915_hpd_write(
	struct i915_hpd_world *world,
	u32 reg,
	u32 val)
{
	/* A model takes the write into the test build's register model. */
	if (world->hpd.fake != NULL) {
		if (drv_i915_hpd_model_write != NULL)
			drv_i915_hpd_model_write(world->hpd.fake, reg, val);
		return val;
	}

	/* Writes the BAR. */
	drv_i915_write32(world->hpd.m, reg, val);

	/* Succeeded: reports the value written. */
	return val;
}

/*
 * Read-modify-writes a register of the running instance (the Linux
 * intel_uncore_rmw()) and returns the value read.
 *
 * What the interrupt's read of SHOTPLUG_CTL_DDI found is kept for its
 * record.
 */
u32
drv_i915_hpd_rmw(
	struct i915_hpd_world *world,
	u32 reg,
	u32 clear,
	u32 set)
{
	struct i915_hpd_fake_regs *fake;
	u32 old;
	u32 val;

	/* Reads the register and computes the new value. */
	old = drv_i915_hpd_read(world, reg);
	val = (old & ~clear) | set;

	/* Keeps what the interrupt read of SHOTPLUG_CTL_DDI. */
	if (reg == I915_HPD_REG_SHOTPLUG_CTL_DDI)
		world->hpd.cur_dig = old;

	/* A model takes the write-back into the test build's register model. */
	fake = world->hpd.fake;
	if (fake != NULL) {
		if (drv_i915_hpd_model_rmw_write != NULL)
			drv_i915_hpd_model_rmw_write(fake, reg, val);
		return old;
	}

	/* Writes the new value to the BAR. */
	drv_i915_write32(world->hpd.m, reg, val);

	/* Succeeded: reports the value read. */
	return old;
}

/*
 * Returns encoder idx of the running instance, or NULL past the last one.
 */
struct intel_encoder *
drv_i915_hpd_encoder_at(
	struct i915_hpd_world *world,
	unsigned idx)
{
	/* An index past the ports the start made ends the walk. */
	if (idx >= world->hpd.num)
		return NULL;

	/* Succeeded: reports the port's encoder. */
	return &world->hpd_ports[idx].base;
}

/*
 * Returns the connector the walk stands at and moves the walk on, or NULL
 * past the last one.
 */
struct intel_connector *
drv_i915_hpd_connector_next(
	struct i915_hpd_world *world,
	struct drm_connector_list_iter *it)
{
	struct intel_connector *connector;

	/* An index past the connectors the start made ends the walk. */
	if (it->idx >= world->hpd.num)
		return NULL;

	/* Reports the connector and moves the walk on. */
	connector = &world->hpd_conns[it->idx];
	it->idx++;

	/* Succeeded: reports the connector. */
	return connector;
}

/*
 * Counts a wake-up of the GMBUS wait queue (from intel_gmbus_irq_handler()).
 */
void
drv_i915_hpd_gmbus_woken(
	struct i915_hpd_world *world)
{
	/* The GMBUS interrupt woke the queue nobody sleeps on. */
	world->hpd.gmbus_irqs++;
}

/*
 * Takes a reference on a power domain the Linux text names (the Linux
 * intel_display_power_get()).
 *
 * Returns a nonzero wakeref, or 0 when the reference could not be taken.
 */
intel_wakeref_t
drv_i915_hpd_intel_display_power_get(
	struct drm_i915_private *i915,
	enum intel_display_power_domain domain)
{
	struct i915_hpd_world *world;
	enum i915_power_domain display_domain;
	int error;

	/* The model tests have no power wells: every reference is taken. */
	world = i915_hpd_world_of(i915);
	if (world->hpd.pd == NULL)
		return 1;

	/* GMBUS maps onto the GMBUS domain, everything else onto the display core. */
	display_domain = I915_PW_DOMAIN_DISPLAY_CORE;
	if (domain == POWER_DOMAIN_GMBUS)
		display_domain = I915_PW_DOMAIN_GMBUS;

	/* Takes the reference. */
	error = drv_i915_display_power_get(world->hpd.pd, display_domain, world->hpd.c);
	if (error != 0)
		return 0;

	/* Succeeded: the reference is held. */
	return 1;
}

/*
 * Drops a reference taken by drv_i915_hpd_intel_display_power_get() (the
 * Linux intel_display_power_put()).
 */
void
drv_i915_hpd_intel_display_power_put(
	struct drm_i915_private *i915,
	enum intel_display_power_domain domain,
	intel_wakeref_t wf)
{
	struct i915_hpd_world *world;
	enum i915_power_domain display_domain;

	/* The model tests hold nothing, and a failed get holds nothing. */
	world = i915_hpd_world_of(i915);
	if (world->hpd.pd == NULL || wf == 0)
		return;

	/* GMBUS maps onto the GMBUS domain, everything else onto the display core. */
	display_domain = I915_PW_DOMAIN_DISPLAY_CORE;
	if (domain == POWER_DOMAIN_GMBUS)
		display_domain = I915_PW_DOMAIN_GMBUS;

	/* Drops the reference. */
	drv_i915_display_power_put(world->hpd.pd, display_domain, world->hpd.c);
}

/*
 * Programs the hotplug interrupts from the pins' state (the Linux
 * intel_hpd_irq_setup()).
 *
 * The state of every pin is copied into the zedBSD hotplug state (same
 * encoding) and programmed from there; a model programs nothing.
 */
void
i915_hpd_intel_hpd_irq_setup(
	struct drm_i915_private *i915)
{
	struct i915_hpd_world *world;
	struct i915_hotplug *hp;
	enum hpd_pin pin;

	/* Counts the call; without display interrupts or hotplug state there is nothing to program. */
	world = i915_hpd_world_of(i915);
	world->hpd.irq_setups++;
	hp = world->hpd.hp;
	if (!i915->display_irqs_enabled || hp == NULL)
		return;

	/* Copies every pin's state. */
	for (pin = HPD_NONE + 1; pin < HPD_NUM_PINS; pin++)
		hp->state[pin] = (int)i915->display.hotplug.stats[pin].state;

	/* Programs the registers of the hardware. */
	if (world->hpd.fake == NULL)
		drv_i915_hpd_irq_setup(hp, world->hpd.m, world->hpd.pch_type, world->hpd.irqs_enabled);
}

/*
 * Tells whether the device's interrupts are on, as the start was told.
 */
bool
i915_hpd_intel_irqs_enabled(
	struct drm_i915_private *i915)
{
	struct i915_hpd_world *world;

	/* The start recorded whether the device's interrupts are installed. */
	world = i915_hpd_world_of(i915);
	if (world->hpd.irqs_enabled == 0)
		return false;

	/* Succeeded: the interrupts are on. */
	return true;
}

/*
 * Records the unported connector polling a storm would turn on.
 */
void
i915_hpd_drm_kms_helper_poll_reschedule(
	struct drm_device *dev)
{
	struct i915_hpd_world *world;

	/* Counts the storm and names the step. */
	world = i915_hpd_world_of(i915_hpd_to_i915(dev));
	world->hpd.storms++;
	kern_logf("i915: hpd step drm_kms_helper_poll_reschedule (connector polling not ported)\n");
}

/*
 * Records the uevent of one changed connector; nobody listens.
 */
void
i915_hpd_drm_kms_helper_connector_hotplug_event(
	struct drm_connector *connector)
{
	/* Names the event. */
	kern_logf("i915: hpd uevent (connector hotplug event, no userspace listener): %s\n", connector->name);
}

/*
 * Records the uevent of several changed connectors; nobody listens.
 */
void
i915_hpd_drm_kms_helper_hotplug_event(
	struct drm_device *dev)
{
	UNUSED_PARAMETER(dev);

	/* Names the event. */
	kern_logf("i915: hpd uevent (hotplug event, no userspace listener)\n");
}

/*
 * ==== The Linux hotplug text (intel_hotplug.c) ====
 */

/*
 * The main hotplug irq handler (the Linux intel_hpd_irq_handler()).
 *
 * The platform specific irq handlers read and decode the appropriate
 * registers into bitmasks about hpd pins that have triggered (@pin_mask),
 * and which of those pins may be long pulses (@long_mask).  The @long_mask
 * is ignored if the port corresponding to the pin is not a digital port.
 *
 * Here, we do hotplug irq storm detection and mitigation, and pass further
 * processing to appropriate bottom halves.
 */
void
i915_hpd_intel_hpd_irq_handler(
	struct drm_i915_private *dev_priv,
	u32 pin_mask,
	u32 long_mask)
{
	struct i915_hpd_world *world;
	struct intel_encoder *encoder;
	unsigned index;
	bool storm_detected;
	bool queue_dig;
	bool queue_hp;
	bool has_pulse;
	bool long_hpd;
	bool storm;
	u32 long_hpd_pulse_mask;
	u32 short_hpd_pulse_mask;
	enum hpd_pin pin;
	enum port port;

	/* An interrupt that triggered no pin has nothing to do. */
	if (!pin_mask)
		return;

	/* Starts with no pulse sorted and nothing to queue. */
	world = i915_hpd_world_of(dev_priv);
	storm_detected = false;
	queue_dig = false;
	queue_hp = false;
	long_hpd_pulse_mask = 0;
	short_hpd_pulse_mask = 0;

	/* Sorts the pulses, counts the storms and records the events. */
	spin_lock(&dev_priv->irq_lock);

	/*
	 * Determine whether ->hpd_pulse() exists for each pin, and
	 * whether we have a short or a long pulse. This is needed
	 * as each pin may have up to two encoders (HDMI and DP) and
	 * only the one of them (DP) will have ->hpd_pulse().
	 */
	for (index = 0u; ; index++) {
		encoder = drv_i915_hpd_encoder_at(world, index);
		if (encoder == NULL)
			break;

		/* Skips an encoder whose pin did not trigger. */
		port = encoder->port;
		pin = encoder->hpd_pin;
		if (!(BIT(pin) & pin_mask))
			continue;

		/* Skips an encoder without a pulse handler. */
		has_pulse = i915_encoder_has_hpd_pulse(encoder);
		if (!has_pulse)
			continue;

		/* The pin's pulse is long when the long mask says so. */
		long_hpd = false;
		if ((long_mask & BIT(pin)) != 0)
			long_hpd = true;

		/* The digital-port work will deliver the pulse. */
		I915_HPD_DRM_DBG(&dev_priv->drm, "digital hpd on [ENCODER:%d:%s] - %s\n", encoder->base.base.id, encoder->base.name, long_hpd ? "long" : "short");
		queue_dig = true;

		/* The port's pulse goes to the digital-port work. */
		if (long_hpd) {
			long_hpd_pulse_mask |= BIT(pin);
			dev_priv->display.hotplug.long_port_mask |= BIT(port);
		} else {
			short_hpd_pulse_mask |= BIT(pin);
			dev_priv->display.hotplug.short_port_mask |= BIT(port);
		}
	}

	/* Now process each pin just once */
	for (pin = HPD_NONE + 1; pin < HPD_NUM_PINS; pin++) {
		/* Skips a pin that did not trigger. */
		if (!(BIT(pin) & pin_mask))
			continue;

		/* A disabled pin should not have raised an interrupt. */
		if (dev_priv->display.hotplug.stats[pin].state == HPD_DISABLED) {
			/*
			 * On GMCH platforms the interrupt mask bits only
			 * prevent irq generation, not the setting of the
			 * hotplug bits itself. So only WARN about unexpected
			 * interrupts on saner platforms.
			 */
			(void)drv_i915_hpd_warn(world, !HAS_GMCH(dev_priv), "Received HPD interrupt on pin %d although disabled\n", "intel_hpd_irq_handler");
			continue;
		}

		/* A pin marked for disabling takes no event. */
		if (dev_priv->display.hotplug.stats[pin].state != HPD_ENABLED)
			continue;

		/*
		 * Delegate to ->hpd_pulse() if one of the encoders for this
		 * pin has it, otherwise let the hotplug_work deal with this
		 * pin directly.
		 */
		if (((short_hpd_pulse_mask | long_hpd_pulse_mask) & BIT(pin))) {
			long_hpd = false;
			if ((long_hpd_pulse_mask & BIT(pin)) != 0)
				long_hpd = true;
		} else {
			dev_priv->display.hotplug.event_bits |= BIT(pin);
			long_hpd = true;
			queue_hp = true;
		}

		/* A storm takes the pin's event back and has the work switch it to polling. */
		storm = i915_hpd_irq_storm_detect(dev_priv, pin, long_hpd);
		if (storm) {
			dev_priv->display.hotplug.event_bits &= ~BIT(pin);
			storm_detected = true;
			queue_hp = true;
		}
	}

	/*
	 * Disable any IRQs that storms were detected on. Polling enablement
	 * happens later in our hotplug work.
	 */
	if (storm_detected)
		i915_hpd_intel_hpd_irq_setup(dev_priv);

	spin_unlock(&dev_priv->irq_lock);

	/*
	 * Our hotplug handler can grab modeset locks (by calling down into the
	 * fb helpers). Hence it must not be run on our own dev-priv->wq work
	 * queue for otherwise the flush_work in the pageflip code will
	 * deadlock.
	 */
	if (queue_dig)
		(void)i915_hpd_queue_work(dev_priv->display.hotplug.dp_wq, &dev_priv->display.hotplug.dig_port_work);
	if (queue_hp)
		(void)i915_hpd_queue_delayed_work(dev_priv->unordered_wq, &dev_priv->display.hotplug.hotplug_work, 0);
}

/*
 * Detects a connector for the hotplug work (the Linux
 * intel_encoder_hotplug()).
 */
enum intel_hotplug_state
i915_hpd_intel_encoder_hotplug(
	struct intel_encoder *encoder,
	struct intel_connector *connector)
{
	enum intel_hotplug_state state;

	UNUSED_PARAMETER(encoder);

	/* Detects the connector and reports whether its epoch moved. */
	state = i915_hotplug_detect_connector(connector);

	/* Succeeded: reports the hotplug state. */
	return state;
}

/*
 * Prepares the hotplug works and the storm settings (the Linux
 * intel_hpd_init_early()).
 */
void
i915_hpd_intel_hpd_init_early(
	struct drm_i915_private *i915)
{
	/* Prepares the hotplug, digital-port, poll-init and storm re-enable works. */
	i915_hpd_init_delayed_work(&i915->display.hotplug.hotplug_work, i915_hotplug_work_func);
	i915_hpd_init_work(&i915->display.hotplug.dig_port_work, i915_digport_work_func);
	i915_hpd_init_work(&i915->display.hotplug.poll_init_work, i915_hpd_poll_init_work);
	i915_hpd_init_delayed_work(&i915->display.hotplug.reenable_work, i915_hpd_irq_storm_reenable_work);

	/* Counts a storm after HPD_STORM_DEFAULT_THRESHOLD interrupts in a period. */
	i915->display.hotplug.hpd_storm_threshold = HPD_STORM_DEFAULT_THRESHOLD;

	/*
	 * If we have MST support, we want to avoid doing short HPD IRQ storm
	 * detection, as short HPD storms will occur as a natural part of
	 * sideband messaging with MST.
	 * On older platforms however, IRQ storms can occur with both long and
	 * short pulses, as seen on some G4x systems.
	 */
	i915->display.hotplug.hpd_short_storm_enabled = !HAS_DP_MST(i915);
}

/*
 * Forgets the pending hotplug events and cancels the hotplug works (the
 * Linux intel_hpd_cancel_work()).
 */
void
i915_hpd_intel_hpd_cancel_work(
	struct drm_i915_private *dev_priv)
{
	struct i915_hpd_world *world;

	/* A device without a display has no hotplug works. */
	if (!HAS_DISPLAY(dev_priv))
		return;

	/* Finds the world whose saved interrupt state the lock uses. */
	world = i915_hpd_world_of(dev_priv);

	/* Forgets every pending pulse, event and retry. */
	i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	dev_priv->display.hotplug.long_port_mask = 0;
	dev_priv->display.hotplug.short_port_mask = 0;
	dev_priv->display.hotplug.event_bits = 0;
	dev_priv->display.hotplug.retry_bits = 0;

	i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	/* Cancels every work and waits for a running one. */
	(void)i915_hpd_cancel_work_sync(&dev_priv->display.hotplug.dig_port_work);
	(void)i915_hpd_cancel_delayed_work_sync(&dev_priv->display.hotplug.hotplug_work);
	(void)i915_hpd_cancel_work_sync(&dev_priv->display.hotplug.poll_init_work);
	(void)i915_hpd_cancel_delayed_work_sync(&dev_priv->display.hotplug.reenable_work);
}

/*
 * ==== The Linux hotplug interrupt text (intel_hotplug_irq.c) ====
 */

/*
 * Decodes the ICP south display hotplug interrupt (the Linux
 * icp_irq_handler()).
 */
void
i915_hpd_icp_irq_handler(
	struct drm_i915_private *dev_priv,
	u32 pch_iir)
{
	struct i915_hpd_world *world;
	u32 ddi_hotplug_trigger;
	u32 tc_hotplug_trigger;
	u32 pin_mask;
	u32 long_mask;
	u32 dig_hotplug_reg;

	/* Splits the interrupt into its DDI and Type-C triggers. */
	world = i915_hpd_world_of(dev_priv);
	ddi_hotplug_trigger = pch_iir & SDE_DDI_HOTPLUG_MASK_ICP;
	tc_hotplug_trigger = pch_iir & SDE_TC_HOTPLUG_MASK_ICP;
	pin_mask = 0;
	long_mask = 0;

	/* Acknowledges the DDI pulses and learns which were long. */
	if (ddi_hotplug_trigger) {
		/* Locking due to DSI native GPIO sequences */
		spin_lock(&dev_priv->irq_lock);

		dig_hotplug_reg = drv_i915_hpd_rmw(world, SHOTPLUG_CTL_DDI.reg, 0, 0);

		spin_unlock(&dev_priv->irq_lock);

		i915_get_hpd_pins(dev_priv, &pin_mask, &long_mask, ddi_hotplug_trigger, dig_hotplug_reg, dev_priv->display.hotplug.pch_hpd, i915_icp_ddi_port_hotplug_long_detect);
	}

	/* Acknowledges the Type-C pulses and learns which were long. */
	if (tc_hotplug_trigger) {
		dig_hotplug_reg = drv_i915_hpd_rmw(world, SHOTPLUG_CTL_TC.reg, 0, 0);

		i915_get_hpd_pins(dev_priv, &pin_mask, &long_mask, tc_hotplug_trigger, dig_hotplug_reg, dev_priv->display.hotplug.pch_hpd, i915_icp_tc_port_hotplug_long_detect);
	}

	/* Hands the triggered pins to the main handler. */
	if (pin_mask)
		i915_hpd_intel_hpd_irq_handler(dev_priv, pin_mask, long_mask);

	/* Wakes a GMBUS transfer. */
	if (pch_iir & SDE_GMBUS_ICP)
		i915_hpd_intel_gmbus_irq_handler(dev_priv);
}

/*
 * ==== The Linux DDI and DP text (intel_ddi.c, intel_dp.c) ====
 */

/*
 * Tells whether a digital port has something plugged in, under a display
 * core power reference (the Linux intel_digital_port_connected()).
 *
 * In cases where there's a connector physically connected but it can't be
 * used by our hardware we also return false, since the rest of the driver
 * should pretty much treat the port as disconnected.
 */
bool
drv_i915_hpd_intel_digital_port_connected(
	struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	bool is_connected;
	intel_wakeref_t wakeref;

	/* Resolves the device and the digital port. */
	dev_priv = i915_hpd_to_i915(encoder->base.dev);
	dig_port = i915_hpd_enc_to_dig_port(encoder);
	is_connected = false;

	/* Asks the port's live status while the display core is powered (with_intel_display_power()). */
	wakeref = drv_i915_hpd_intel_display_power_get(dev_priv, POWER_DOMAIN_DISPLAY_CORE);
	if (wakeref) {
		is_connected = dig_port->connected(encoder);
		i915_hpd_intel_display_power_put_async(dev_priv, POWER_DOMAIN_DISPLAY_CORE, wakeref);
	}

	/* Reports the live status. */
	return is_connected;
}

/*
 * Maps a port onto its PHY on ADL-P: the TC ports from D map onto PHY_F on.
 */
enum phy
drv_i915_hpd_intel_port_to_phy(
	struct drm_i915_private *i915,
	enum port port)
{
	UNUSED_PARAMETER(i915);

	/* The Type-C ports follow PHY_E. */
	if (port >= PORT_TC1)
		return (enum phy)(PHY_F + port - PORT_TC1);

	/* The combo ports have the PHY of the same letter. */
	return (enum phy)(PHY_A + port - PORT_A);
}

/*
 * Tells whether a PHY is a Type-C PHY on ADL-P (PHY_F to PHY_I).
 */
bool
drv_i915_hpd_intel_phy_is_tc(
	struct drm_i915_private *i915,
	enum phy phy)
{
	UNUSED_PARAMETER(i915);

	/* Only PHY_F to PHY_I are Type-C. */
	if (phy < PHY_F || phy > PHY_I)
		return false;

	/* Succeeded: the PHY is Type-C. */
	return true;
}

/*
 * Resets a Type-C port's link after a hotplug (the Linux
 * intel_tc_port_link_reset()).
 *
 * False unless the port is Type-C; the Type-C link reset itself is not
 * ported (a recorded step answering false).
 */
bool
i915_hpd_intel_tc_port_link_reset(
	struct intel_digital_port *dig_port)
{
	enum phy phy;
	bool is_tc;

	/* A port that is not Type-C has no link to reset. */
	phy = drv_i915_hpd_intel_port_to_phy(NULL, dig_port->base.port);
	is_tc = drv_i915_hpd_intel_phy_is_tc(NULL, phy);
	if (!is_tc)
		return false;

	/* Names the unported step. */
	kern_logf("i915: hpd step intel_tc_port_link_reset (unported): %s -> false\n", dig_port->base.base.name);
	return false;
}

/*
 * Records the unported DP PHY compliance test.
 */
void
i915_hpd_intel_dp_phy_test(
	struct intel_encoder *encoder)
{
	/* Names the step. */
	kern_logf("i915: hpd step intel_dp_phy_test (unported): %s\n", encoder->base.name);
}

/*
 * Records the unported DP link retraining; answers success.
 */
int
i915_hpd_intel_dp_retrain_link(
	struct intel_encoder *encoder,
	struct drm_modeset_acquire_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Names the step. */
	kern_logf("i915: hpd step intel_dp_retrain_link (unported): %s -> 0\n", encoder->base.name);

	/* Succeeded: nothing needed retraining. */
	return 0;
}

/*
 * Returns the DDI hotplug handler, for the encoders the start makes.
 */
enum intel_hotplug_state
(*i915_hpd_ddi_hotplug_fn(void))(struct intel_encoder *, struct intel_connector *)
{
	/* Succeeded: the handler is intel_ddi_hotplug(). */
	return i915_ddi_hotplug;
}

/*
 * Returns the live-status check of a non-Type-C port, for the digital
 * ports the start makes.
 */
bool
(*i915_hpd_lpt_connected_fn(void))(struct intel_encoder *)
{
	/* Succeeded: the check is lpt_digital_port_connected(). */
	return i915_lpt_digital_port_connected;
}

/*
 * ==== The Linux probe helper text (drm_probe_helper.c, drm_connector.c) ====
 */

/*
 * Probes a connector's status (the Linux drm_helper_probe_detect()).
 *
 * This function calls the detect callbacks of the connector.  It returns
 * enum connector_status, or if @ctx is set, it might also return
 * -EDEADLK.
 */
int
i915_hpd_drm_helper_probe_detect(
	struct drm_connector *connector,
	struct drm_modeset_acquire_ctx *ctx,
	bool force)
{
	const struct drm_connector_helper_funcs *funcs;
	struct drm_device *dev;
	enum connector_status status;
	int ret;

	/* Resolves the connector's callbacks and device. */
	funcs = connector->helper_private;
	dev = connector->dev;

	/* Without a context the probe takes its own locks. */
	if (!ctx) {
		status = i915_drm_helper_probe_detect_ctx(connector, force);
		return status;
	}

	/* Takes the connection lock in the caller's context. */
	ret = i915_hpd_drm_modeset_lock(&dev->mode_config.connection_mutex, ctx);
	if (ret)
		return ret;

	/* Asks the context-aware detect, else the forced detect; a connector without either is connected. */
	if (funcs->detect_ctx) {
		ret = funcs->detect_ctx(connector, ctx, force);
	} else if (connector->funcs->detect) {
		ret = connector->funcs->detect(connector, force);
	} else {
		ret = connector_status_connected;
	}

	/* A changed status is a new epoch of the connector. */
	if (ret != (int)connector->status)
		connector->epoch_counter += 1;

	/* Succeeded: reports the connector status. */
	return ret;
}

/*
 * Returns the name of a connector status (the Linux
 * drm_get_connector_status_name()).
 */
const char *
i915_hpd_drm_get_connector_status_name(
	enum connector_status status)
{
	/* Names the two definite states; everything else is unknown. */
	if (status == connector_status_connected)
		return "connected";
	if (status == connector_status_disconnected)
		return "disconnected";

	/* Succeeded: the status is not definite. */
	return "unknown";
}

/* Returns the hotplug world of a display, or NULL when it has none. */
static struct i915_hpd_world *
i915_hpd_display_world(
	struct i915_display *display)
{
	/* The display owns the world between its create and its destroy. */
	return display->hpd_world;
}

/* Read-modify-writes a register, writing only a changed value; returns the new value. */
static uint32_t
i915_hpd_uncore_rmw(
	struct i915_mmio *m,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old;
	uint32_t val;

	/* Reads the register and computes the new value. */
	old = drv_i915_read32(m, reg);
	val = (old & ~clear) | set;

	/* Writes only a value that changes the register. */
	if (val != old)
		drv_i915_write32(m, reg, val);

	/* Succeeded: reports the value the register now holds. */
	return val;
}

/* Returns the TC hotplug bit of a pin in the DE HPD registers (GEN11_TC_HOTPLUG()). */
static uint32_t
i915_gen11_tc_hotplug(
	int pin)
{
	/* Bits 16 on, one per TC pin. */
	return 1u << (16 + (pin - I915_HPD_PORT_TC1));
}

/* Returns the TBT hotplug bit of a pin in the DE HPD registers (GEN11_TBT_HOTPLUG()). */
static uint32_t
i915_gen11_tbt_hotplug(
	int pin)
{
	/* Bits 0 on, one per TC pin. */
	return 1u << (pin - I915_HPD_PORT_TC1);
}

/* Returns the enable bit of a pin in the TC/TBT hotplug control (GEN11_HOTPLUG_CTL_ENABLE()). */
static uint32_t
i915_gen11_hotplug_ctl_enable(
	int pin)
{
	/* Bit 3 of the pin's nibble. */
	return 8u << ((pin - I915_HPD_PORT_TC1) * 4);
}

/* Returns the DDI hotplug bit of a pin in the SDE registers (SDE_DDI_HOTPLUG_ICP()). */
static uint32_t
i915_sde_ddi_hotplug_icp(
	int pin)
{
	/* Bits 16 on, one per DDI pin. */
	return 1u << (16 + (pin - I915_HPD_PORT_A));
}

/* Returns the TC hotplug bit of a pin in the SDE registers (SDE_TC_HOTPLUG_ICP()). */
static uint32_t
i915_sde_tc_hotplug_icp(
	int pin)
{
	/* Bits 24 on, one per TC pin. */
	return 1u << (24 + (pin - I915_HPD_PORT_TC1));
}

/* Returns the enable bit of a DDI pin in SHOTPLUG_CTL_DDI (SHOTPLUG_CTL_DDI_HPD_ENABLE()). */
static uint32_t
i915_shotplug_ctl_ddi_hpd_enable(
	int pin)
{
	/* Bit 3 of the pin's nibble. */
	return 0x8u << ((pin - I915_HPD_PORT_A) * 4);
}

/* Returns the enable bit of a TC pin in SHOTPLUG_CTL_TC (ICP_TC_HPD_ENABLE()). */
static uint32_t
i915_icp_tc_hpd_enable(
	int pin)
{
	/* Bit 3 of the pin's nibble. */
	return 8u << ((pin - I915_HPD_PORT_TC1) * 4);
}

/* Tells whether a pin is a Type-C pin. */
static int
i915_is_tc_pin(
	int pin)
{
	/* TC1 to TC6. */
	if (pin < I915_HPD_PORT_TC1 || pin > I915_HPD_PORT_TC6)
		return 0;

	/* Succeeded: the pin is Type-C. */
	return 1;
}

/* Tells whether a pin is a DDI pin. */
static int
i915_is_ddi_pin(
	int pin)
{
	/* A to D. */
	if (pin < I915_HPD_PORT_A || pin > I915_HPD_PORT_D)
		return 0;

	/* Succeeded: the pin is a DDI pin. */
	return 1;
}

/* Collects the enabled and the hotplug interrupt bits of the encoders' pins (intel_hpd_enabled_irqs(), intel_hpd_hotplug_irqs()). */
static void
i915_hpd_irqs(
	const struct i915_hotplug *hp,
	const uint32_t *table,
	uint32_t *enabled,
	uint32_t *hotplug)
{
	unsigned i;
	int pin;

	/* Starts with no bits. */
	*enabled = 0u;
	*hotplug = 0u;

	/* Adds every encoder's pin; only an enabled pin is unmasked. */
	for (i = 0u; i < hp->n_encoders; i++) {
		pin = hp->encoder_pin[i];
		if (pin <= I915_HPD_NONE || pin >= I915_HPD_NUM_PINS)
			continue;

		/* Every pin raises hotplug; only an enabled pin is unmasked. */
		if (hp->state[pin] == I915_HPD_ENABLED)
			*enabled |= table[pin];
		*hotplug |= table[pin];
	}
}

/* Collects a control bit over every pin it applies to (intel_hpd_hotplug_mask()). */
static uint32_t
i915_hotplug_mask(
	uint32_t (*fn)(int),
	int (*applies)(int))
{
	uint32_t v;
	int pin;
	int applied;

	/* Adds the bit of every pin of the kind. */
	v = 0u;
	for (pin = I915_HPD_NONE; pin < I915_HPD_NUM_PINS; pin++) {
		applied = applies(pin);
		if (applied)
			v |= fn(pin);
	}

	/* Reports the mask. */
	return v;
}

/* Collects a control bit over the encoders' pins it applies to (intel_hpd_hotplug_enables()). */
static uint32_t
i915_hotplug_enables(
	const struct i915_hotplug *hp,
	uint32_t (*fn)(int),
	int (*applies)(int))
{
	uint32_t v;
	unsigned i;
	int applied;

	/* Adds the bit of every encoder whose pin is of the kind. */
	v = 0u;
	for (i = 0u; i < hp->n_encoders; i++) {
		applied = applies(hp->encoder_pin[i]);
		if (applied)
			v |= fn(hp->encoder_pin[i]);
	}

	/* Reports the enables. */
	return v;
}

/* Programs the hotplug interrupts and detection (gen11_hpd_irq_setup(), with icp_hpd_irq_setup() on PCH >= ICP). */
static void
i915_gen11_hpd_irq_setup(
	struct i915_hotplug *hp,
	struct i915_mmio *m,
	int pch_type,
	int intel_irqs_enabled)
{
	uint32_t enabled_irqs;
	uint32_t hotplug_irqs;
	uint32_t sdeimr;
	uint32_t mask;
	uint32_t enables;

	/* Unmasks the enabled TC/TBT pins in GEN11_DE_HPD_IMR. */
	i915_hpd_irqs(hp, hp->hpd, &enabled_irqs, &hotplug_irqs);
	hp->de_enabled_irqs = enabled_irqs;
	hp->de_hotplug_irqs = hotplug_irqs;
	hp->de_hpd_imr = i915_hpd_uncore_rmw(m, I915_GEN11_DE_HPD_IMR, hotplug_irqs, ~enabled_irqs & hotplug_irqs);
	(void)drv_i915_read32(m, I915_GEN11_DE_HPD_IMR);

	/* gen11_tc_hpd_detection_setup / gen11_tbt_hpd_detection_setup */
	mask = i915_hotplug_mask(i915_gen11_hotplug_ctl_enable, i915_is_tc_pin);
	enables = i915_hotplug_enables(hp, i915_gen11_hotplug_ctl_enable, i915_is_tc_pin);
	hp->tc_ctl = i915_hpd_uncore_rmw(m, I915_GEN11_TC_HOTPLUG_CTL, mask, enables);
	mask = i915_hotplug_mask(i915_gen11_hotplug_ctl_enable, i915_is_tc_pin);
	enables = i915_hotplug_enables(hp, i915_gen11_hotplug_ctl_enable, i915_is_tc_pin);
	hp->tbt_ctl = i915_hpd_uncore_rmw(m, I915_GEN11_TBT_HOTPLUG_CTL, mask, enables);

	/* A PCH before ICP has no south hotplug of this kind. */
	if (pch_type < I915_PCH_ICP)
		return;

	/* icp_hpd_irq_setup(): the south pins, and the filter count of the PCH. */
	i915_hpd_irqs(hp, hp->pch_hpd, &enabled_irqs, &hotplug_irqs);
	hp->pch_enabled_irqs = enabled_irqs;
	hp->pch_hotplug_irqs = hotplug_irqs;
	if (pch_type <= I915_PCH_TGP) {
		hp->shpd_filter = I915_SHPD_FILTER_CNT_500_ADJ;
	} else {
		hp->shpd_filter = I915_SHPD_FILTER_CNT_250;
	}
	drv_i915_write32(m, I915_SHPD_FILTER_CNT, hp->shpd_filter);

	/* ibx_display_interrupt_update(): only while intel_irqs_enabled(). */
	sdeimr = drv_i915_read32(m, I915_SDEIMR);
	sdeimr &= ~hotplug_irqs;
	sdeimr |= (~enabled_irqs & hotplug_irqs);
	if (intel_irqs_enabled) {
		drv_i915_write32(m, I915_SDEIMR, sdeimr);
		(void)drv_i915_read32(m, I915_SDEIMR);
		hp->sdeimr = sdeimr;
	} else {
		hp->sdeimr_skipped = 1;
		hp->sdeimr = drv_i915_read32(m, I915_SDEIMR);
	}

	/* icp_ddi_hpd_detection_setup / icp_tc_hpd_detection_setup */
	mask = i915_hotplug_mask(i915_shotplug_ctl_ddi_hpd_enable, i915_is_ddi_pin);
	enables = i915_hotplug_enables(hp, i915_shotplug_ctl_ddi_hpd_enable, i915_is_ddi_pin);
	hp->shotplug_ddi = i915_hpd_uncore_rmw(m, I915_SHOTPLUG_CTL_DDI, mask, enables);
	mask = i915_hotplug_mask(i915_icp_tc_hpd_enable, i915_is_tc_pin);
	enables = i915_hotplug_enables(hp, i915_icp_tc_hpd_enable, i915_is_tc_pin);
	hp->shotplug_tc = i915_hpd_uncore_rmw(m, I915_SHOTPLUG_CTL_TC, mask, enables);
}

/* Returns the HPD pin of a connector's encoder, or HPD_NONE without one (intel_connector_hpd_pin()). */
static enum hpd_pin
i915_connector_hpd_pin(
	struct intel_connector *connector)
{
	struct intel_encoder *encoder;

	/* Finds the encoder the connector is attached to. */
	encoder = i915_hpd_intel_attached_encoder(connector);

	/*
	 * MST connectors get their encoder attached dynamically
	 * so need to make sure we have an encoder here. But since
	 * MST encoders have their hpd_pin set to HPD_NONE we don't
	 * have to special case them beyond that.
	 */
	if (encoder == NULL)
		return HPD_NONE;

	/* Succeeded: reports the encoder's pin. */
	return encoder->hpd_pin;
}

/*
 * Gathers the statistics of a pin and reports an HPD interrupt storm on it
 * (intel_hpd_irq_storm_detect()).
 *
 * The number of IRQs that are allowed within HPD_STORM_DETECT_PERIOD is
 * hpd_storm_threshold.  Long IRQs count as +10 to this threshold, and short
 * IRQs count as +1.  If this threshold is exceeded, it's considered an IRQ
 * storm and the IRQ state is set to HPD_MARK_DISABLED.  Short IRQ detection
 * is only enabled for systems without DP MST support.
 */
static bool
i915_hpd_irq_storm_detect(
	struct drm_i915_private *dev_priv,
	enum hpd_pin pin,
	bool long_hpd)
{
	struct intel_hotplug *hpd;
	unsigned long start;
	unsigned long end;
	unsigned long now;
	int increment;
	int threshold;
	bool in_period;

	/* The pin's detection period, and what this pulse counts. */
	hpd = &dev_priv->display.hotplug;
	start = hpd->stats[pin].last_jiffies;
	end = start + i915_hpd_msecs_to_jiffies(HPD_STORM_DETECT_PERIOD);
	increment = 1;
	if (long_hpd)
		increment = 10;
	threshold = hpd->hpd_storm_threshold;

	/* No threshold detects nothing; a short pulse counts only where short storms are tracked. */
	if (!threshold)
		return false;
	if (!long_hpd && !dev_priv->display.hotplug.hpd_short_storm_enabled)
		return false;

	/* An interrupt after the period starts a new one (the Linux time_in_range() reads jiffies twice). */
	in_period = false;
	now = i915_hpd_jiffies();
	if (time_after_eq(now, start)) {
		now = i915_hpd_jiffies();
		if (time_after_eq(end, now))
			in_period = true;
	}
	if (!in_period) {
		hpd->stats[pin].last_jiffies = i915_hpd_jiffies();
		hpd->stats[pin].count = 0;
	}

	/* Counts the interrupt; above the threshold the pin is marked for disabling. */
	hpd->stats[pin].count += increment;
	if (hpd->stats[pin].count > threshold) {
		hpd->stats[pin].state = HPD_MARK_DISABLED;
		I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "HPD interrupt storm detected on PIN %d\n", pin);
		return true;
	}

	/* Logs the pin's count so far. */
	I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "Received HPD interrupt on PIN %d - cnt: %d\n", pin, hpd->stats[pin].count);

	/* Succeeded: no storm on the pin. */
	return false;
}

/*
 * Switches the connectors of storming pins from hotplug detection to
 * polling (intel_hpd_irq_storm_switch_to_polling()); runs under irq_lock.
 */
static void
i915_hpd_irq_storm_switch_to_polling(
	struct drm_i915_private *dev_priv)
{
	struct i915_hpd_world *world;
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	bool hpd_disabled;
	enum hpd_pin pin;

	I915_HPD_LOCKDEP_ASSERT_HELD(&dev_priv->irq_lock);

	/* Starts with no connector switched. */
	world = i915_hpd_world_of(dev_priv);
	hpd_disabled = false;

	/* Disables HPD on every hotplug-detected connector whose pin was marked. */
	i915_hpd_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	for (;;) {
		connector = drv_i915_hpd_connector_next(world, &conn_iter);
		if (connector == NULL)
			break;

		/* Only a hotplug-detected connector is switched. */
		if (connector->base.polled != CONNECTOR_POLL_HPD)
			continue;

		/* Only a connector whose pin was marked is switched. */
		pin = i915_connector_hpd_pin(connector);
		if (pin == HPD_NONE)
			continue;
		if (dev_priv->display.hotplug.stats[pin].state != HPD_MARK_DISABLED)
			continue;

		/* Disables the pin and has the connector polled for connect and disconnect. */
		I915_HPD_DRM_INFO(&dev_priv->drm, "HPD interrupt storm detected on connector %s: switching from hotplug detection to polling\n", connector->base.name);
		dev_priv->display.hotplug.stats[pin].state = HPD_DISABLED;
		connector->base.polled = CONNECTOR_POLL_CONNECT | CONNECTOR_POLL_DISCONNECT;
		hpd_disabled = true;
	}
	i915_hpd_drm_connector_list_iter_end(&conn_iter);

	/* Enable polling and queue hotplug re-enabling. */
	if (hpd_disabled) {
		i915_hpd_drm_kms_helper_poll_reschedule(&dev_priv->drm);
		(void)i915_hpd_mod_delayed_work(dev_priv->unordered_wq, &dev_priv->display.hotplug.reenable_work, i915_hpd_msecs_to_jiffies(HPD_STORM_REENABLE_DELAY));
	}
}

/* Re-enables HPD on the pins a storm disabled (intel_hpd_irq_storm_reenable_work()). */
static void
i915_hpd_irq_storm_reenable_work(
	struct work_struct *work)
{
	struct drm_i915_private *dev_priv;
	struct i915_hpd_world *world;
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	intel_wakeref_t wakeref;
	enum hpd_pin pin;

	/* Resolves the device the work belongs to. */
	dev_priv = container_of(work, struct drm_i915_private, display.hotplug.reenable_work.work);
	world = i915_hpd_world_of(dev_priv);

	/* Keeps the device awake across the work. */
	wakeref = intel_runtime_pm_get(&dev_priv->runtime_pm);

	/* Gives the connectors their polling mode back and enables their pins. */
	i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	i915_hpd_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	for (;;) {
		connector = drv_i915_hpd_connector_next(world, &conn_iter);
		if (connector == NULL)
			break;

		/* Only a connector whose pin a storm disabled is restored. */
		pin = i915_connector_hpd_pin(connector);
		if (pin == HPD_NONE)
			continue;
		if (dev_priv->display.hotplug.stats[pin].state != HPD_DISABLED)
			continue;

		/* Restores the connector's own polling mode. */
		if (connector->base.polled != connector->polled)
			I915_HPD_DRM_DBG(&dev_priv->drm, "Reenabling HPD on connector %s\n", connector->base.name);
		connector->base.polled = connector->polled;
	}
	i915_hpd_drm_connector_list_iter_end(&conn_iter);

	/* Enables every disabled pin. */
	for (pin = HPD_NONE + 1; pin < HPD_NUM_PINS; pin++) {
		if (dev_priv->display.hotplug.stats[pin].state == HPD_DISABLED)
			dev_priv->display.hotplug.stats[pin].state = HPD_ENABLED;
	}

	/* Unmasks the re-enabled pins. */
	i915_hpd_intel_hpd_irq_setup(dev_priv);

	i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	/* Lets the device sleep again. */
	intel_runtime_pm_put(&dev_priv->runtime_pm, wakeref);
}

/* Detects a connector and reports whether its epoch moved (intel_hotplug_detect_connector()). */
static enum intel_hotplug_state
i915_hotplug_detect_connector(
	struct intel_connector *connector)
{
	struct drm_device *dev;
	struct i915_hpd_world *world;
	enum connector_status old_status;
	u64 old_epoch_counter;
	int status;
	bool locked;
	bool ret;

	/* Resolves the device and its world. */
	dev = connector->base.dev;
	world = i915_hpd_world_of(i915_hpd_to_i915(dev));
	ret = false;

	/*
	 * The hotplug work holds mode_config.mutex.
	 *
	 * XXX: the Linux mutex_is_locked() asks whether anyone holds the
	 * mutex; the kernel's mutex_owned() used here asks whether the caller
	 * does.
	 */
	locked = mutex_owned(&dev->mode_config.mutex);
	(void)drv_i915_hpd_warn(world, !locked, "!mutex_is_locked(&dev->mode_config.mutex)", "intel_hotplug_detect_connector");
	old_status = connector->base.status;
	old_epoch_counter = connector->base.epoch_counter;

	/* Detects the connector and adopts the status unless it is forced. */
	status = i915_hpd_drm_helper_probe_detect(&connector->base, NULL, false);
	if (!connector->base.force)
		connector->base.status = status;

	/* A detection that moved the epoch changed the connector. */
	if (old_epoch_counter != connector->base.epoch_counter)
		ret = true;

	/* Logs the change and reports it. */
	if (ret) {
		I915_HPD_DRM_DBG_KMS(dev, "[CONNECTOR:%d:%s] status updated from %s to %s (epoch counter %llu->%llu)\n", connector->base.base.id, connector->base.name, i915_hpd_drm_get_connector_status_name(old_status), i915_hpd_drm_get_connector_status_name(connector->base.status), (unsigned long long)old_epoch_counter, (unsigned long long)connector->base.epoch_counter);
		return INTEL_HOTPLUG_CHANGED;
	}

	/* Succeeded: the connector is unchanged. */
	return INTEL_HOTPLUG_UNCHANGED;
}

/* Tells whether an encoder is a digital port with a pulse handler (intel_encoder_has_hpd_pulse()). */
static bool
i915_encoder_has_hpd_pulse(
	struct intel_encoder *encoder)
{
	struct intel_digital_port *dig_port;
	bool is_dig_port;

	/* Only a digital port has a pulse handler. */
	is_dig_port = i915_intel_encoder_is_dig_port(encoder);
	if (!is_dig_port)
		return false;

	/* The port may still have none. */
	dig_port = i915_hpd_enc_to_dig_port(encoder);
	if (dig_port->hpd_pulse == NULL)
		return false;

	/* Succeeded: the port handles its pulses. */
	return true;
}

/* Hands the ports' pulses to their pulse handlers (i915_digport_work_func()). */
static void
i915_digport_work_func(
	struct work_struct *work)
{
	struct drm_i915_private *dev_priv;
	struct i915_hpd_world *world;
	u32 long_port_mask;
	u32 short_port_mask;
	struct intel_encoder *encoder;
	struct intel_digital_port *dig_port;
	unsigned index;
	u32 old_bits;
	enum port port;
	bool long_hpd;
	bool short_hpd;
	bool has_pulse;
	enum irqreturn ret;

	/* Resolves the device the work belongs to. */
	dev_priv = container_of(work, struct drm_i915_private, display.hotplug.dig_port_work);
	world = i915_hpd_world_of(dev_priv);
	old_bits = 0;

	/* Takes the pulses the interrupt collected. */
	i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	long_port_mask = dev_priv->display.hotplug.long_port_mask;
	dev_priv->display.hotplug.long_port_mask = 0;
	short_port_mask = dev_priv->display.hotplug.short_port_mask;
	dev_priv->display.hotplug.short_port_mask = 0;

	i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	/* Delivers each port's pulse; a port that does not take it falls back to the hotplug work. */
	for (index = 0u; ; index++) {
		encoder = drv_i915_hpd_encoder_at(world, index);
		if (encoder == NULL)
			break;

		/* Skips an encoder without a pulse handler. */
		port = encoder->port;
		has_pulse = i915_encoder_has_hpd_pulse(encoder);
		if (!has_pulse)
			continue;

		/* Which pulses the port had. */
		long_hpd = false;
		if ((long_port_mask & BIT(port)) != 0)
			long_hpd = true;
		short_hpd = false;
		if ((short_port_mask & BIT(port)) != 0)
			short_hpd = true;

		/* A port without a pulse has nothing to deliver. */
		if (!long_hpd && !short_hpd)
			continue;

		/* Delivers the pulse to the port. */
		dig_port = i915_hpd_enc_to_dig_port(encoder);

		ret = dig_port->hpd_pulse(dig_port, long_hpd);
		if (ret == IRQ_NONE) {
			/* fall back to old school hpd */
			old_bits |= BIT(encoder->hpd_pin);
		}
	}

	/* Hands the pins no pulse handler took to the hotplug work. */
	if (old_bits) {
		i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

		dev_priv->display.hotplug.event_bits |= old_bits;

		i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

		(void)i915_hpd_queue_delayed_work(dev_priv->unordered_wq, &dev_priv->display.hotplug.hotplug_work, 0);
	}
}

/* Handles hotplug events outside the interrupt handler proper (i915_hotplug_work_func()). */
static void
i915_hotplug_work_func(
	struct work_struct *work)
{
	struct drm_i915_private *dev_priv;
	struct i915_hpd_world *world;
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	struct intel_encoder *encoder;
	u32 changed;
	u32 retry;
	u32 hpd_event_bits;
	u32 hpd_retry_bits;
	u32 hpd_bit;
	struct drm_connector *first_changed_connector;
	int changed_connectors;
	enum hpd_pin pin;
	enum intel_hotplug_state state;

	/* Resolves the device the work belongs to; nothing has changed yet. */
	dev_priv = container_of(work, struct drm_i915_private, display.hotplug.hotplug_work.work);
	world = i915_hpd_world_of(dev_priv);
	changed = 0;
	retry = 0;
	first_changed_connector = NULL;
	changed_connectors = 0;

	/* Detects the connectors of the pins that raised an event, under mode_config.mutex. */
	mutex_lock(&dev_priv->drm.mode_config.mutex);

	I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "running encoder hotplug functions\n");

	/* Takes the events and retries, and switches storming pins to polling. */
	i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	hpd_event_bits = dev_priv->display.hotplug.event_bits;
	dev_priv->display.hotplug.event_bits = 0;
	hpd_retry_bits = dev_priv->display.hotplug.retry_bits;
	dev_priv->display.hotplug.retry_bits = 0;

	/* Enable polling for connectors which had HPD IRQ storms */
	i915_hpd_irq_storm_switch_to_polling(dev_priv);

	i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

	/* Skip calling encode hotplug handlers if ignore long HPD set*/
	if (dev_priv->display.hotplug.ignore_long_hpd) {
		I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "Ignore HPD flag on - skip encoder hotplug handlers\n");
		mutex_unlock(&dev_priv->drm.mode_config.mutex);
		return;
	}

	/* Runs the hotplug handler of every connector whose pin has an event or a retry. */
	i915_hpd_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	for (;;) {
		connector = drv_i915_hpd_connector_next(world, &conn_iter);
		if (connector == NULL)
			break;

		/* A connector without a pin has no events. */
		pin = i915_connector_hpd_pin(connector);
		if (pin == HPD_NONE)
			continue;

		/* Only a pin with an event or a retry is detected. */
		hpd_bit = BIT(pin);
		if (((hpd_event_bits | hpd_retry_bits) & hpd_bit) == 0)
			continue;

		/* The encoder's hotplug handler detects the connector. */
		encoder = i915_hpd_intel_attached_encoder(connector);

		/* A new event restarts the retries; a retry counts one more. */
		if (hpd_event_bits & hpd_bit) {
			connector->hotplug_retries = 0;
		} else {
			connector->hotplug_retries++;
		}

		/* Logs the event. */
		I915_HPD_DRM_DBG_KMS(&dev_priv->drm, "Connector %s (pin %i) received hotplug event. (retry %d)\n", connector->base.name, pin, connector->hotplug_retries);

		/* Sorts the connector by what its handler found. */
		state = encoder->hotplug(encoder, connector);
		switch (state) {
		case INTEL_HOTPLUG_UNCHANGED:
			break;
		case INTEL_HOTPLUG_CHANGED:
			changed |= hpd_bit;
			changed_connectors++;
			if (!first_changed_connector) {
				i915_hpd_drm_connector_get(&connector->base);
				first_changed_connector = &connector->base;
			}
			break;
		case INTEL_HOTPLUG_RETRY:
			retry |= hpd_bit;
			break;
		}
	}
	i915_hpd_drm_connector_list_iter_end(&conn_iter);

	mutex_unlock(&dev_priv->drm.mode_config.mutex);

	/* Tells userspace what changed: one connector, or the whole device. */
	if (changed_connectors == 1) {
		i915_hpd_drm_kms_helper_connector_hotplug_event(first_changed_connector);
	} else if (changed_connectors > 0) {
		i915_hpd_drm_kms_helper_hotplug_event(&dev_priv->drm);
	}

	/* Drops the reference on the first changed connector. */
	if (first_changed_connector)
		i915_hpd_drm_connector_put(first_changed_connector);

	/* Remove shared HPD pins that have changed */
	retry &= ~changed;
	if (retry) {
		i915_hpd_spin_lock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

		dev_priv->display.hotplug.retry_bits |= retry;

		i915_hpd_spin_unlock_irq(&dev_priv->irq_lock, &world->i915_hpd_irq_saved);

		(void)i915_hpd_mod_delayed_work(dev_priv->unordered_wq, &dev_priv->display.hotplug.hotplug_work, i915_hpd_msecs_to_jiffies(HPD_RETRY_DELAY));
	}
}

/* The poll-init work: run inline by drv_i915_hpd_poll_disable() at probe, never queued here. */
static void
i915_hpd_poll_init_work(
	struct work_struct *work)
{
	UNUSED_PARAMETER(work);
}

/* Tells whether a DDI pin's pulse was long (icp_ddi_port_hotplug_long_detect()). */
static bool
i915_icp_ddi_port_hotplug_long_detect(
	enum hpd_pin pin,
	u32 val)
{
	/* Only the DDI pins have a long-detect field in SHOTPLUG_CTL_DDI. */
	switch (pin) {
	case HPD_PORT_A:
	case HPD_PORT_B:
	case HPD_PORT_C:
	case HPD_PORT_D:
		if ((val & SHOTPLUG_CTL_DDI_HPD_LONG_DETECT(pin)) == 0)
			return false;
		return true;
	default:
		return false;
	}
}

/* Tells whether a TC pin's pulse was long (icp_tc_port_hotplug_long_detect()). */
static bool
i915_icp_tc_port_hotplug_long_detect(
	enum hpd_pin pin,
	u32 val)
{
	/* Only the TC pins have a long-detect field in SHOTPLUG_CTL_TC. */
	switch (pin) {
	case HPD_PORT_TC1:
	case HPD_PORT_TC2:
	case HPD_PORT_TC3:
	case HPD_PORT_TC4:
	case HPD_PORT_TC5:
	case HPD_PORT_TC6:
		if ((val & ICP_TC_HPD_LONG_DETECT(pin)) == 0)
			return false;
		return true;
	default:
		return false;
	}
}

/*
 * Adds the pins that triggered, and which ones may be long, to the masks
 * (intel_get_hpd_pins()).
 *
 * This can be called multiple times with the same masks to accumulate
 * hotplug detection results from several registers.  Note that the caller
 * is expected to zero out the masks initially.
 */
static void
i915_get_hpd_pins(
	struct drm_i915_private *dev_priv,
	u32 *pin_mask,
	u32 *long_mask,
	u32 hotplug_trigger,
	u32 dig_hotplug_reg,
	const u32 hpd[HPD_NUM_PINS],
	bool long_pulse_detect(enum hpd_pin pin, u32 val))
{
	enum hpd_pin pin;
	bool long_pulse;

	BUILD_BUG_ON(BITS_PER_TYPE(*pin_mask) < HPD_NUM_PINS);

	/* The device is named only by the debug message, which does not evaluate it. */
	UNUSED_PARAMETER(dev_priv);

	/* Adds every pin whose trigger bit is set, and its pulse length. */
	for (pin = HPD_NONE + 1; pin < HPD_NUM_PINS; pin++) {
		if ((hpd[pin] & hotplug_trigger) == 0)
			continue;

		/* The pin triggered. */
		*pin_mask |= BIT(pin);

		/* The control register tells whether its pulse was long. */
		long_pulse = long_pulse_detect(pin, dig_hotplug_reg);
		if (long_pulse)
			*long_mask |= BIT(pin);
	}

	/* Logs what the register held and what was decoded. */
	I915_HPD_DRM_DBG(&dev_priv->drm, "hotplug event received, stat 0x%08x, dig 0x%08x, pins 0x%08x, long 0x%08x\n", hotplug_trigger, dig_hotplug_reg, *pin_mask, *long_mask);
}

/* Detects a DDI connector after a hotplug and decides on a retry (intel_ddi_hotplug()). */
static enum intel_hotplug_state
i915_ddi_hotplug(
	struct intel_encoder *encoder,
	struct intel_connector *connector)
{
	struct drm_i915_private *i915;
	struct i915_hpd_world *world;
	struct intel_digital_port *dig_port;
	struct intel_dp *intel_dp;
	struct drm_modeset_acquire_ctx ctx;
	enum intel_hotplug_state state;
	enum phy phy;
	bool is_tc;
	bool link_reset;
	int retry_limit;
	int ret;

	/* Resolves the device, the port and whether its PHY is Type-C. */
	i915 = i915_hpd_to_i915(encoder->base.dev);
	world = i915_hpd_world_of(i915);
	dig_port = i915_hpd_enc_to_dig_port(encoder);
	intel_dp = &dig_port->dp;
	phy = drv_i915_hpd_intel_port_to_phy(i915, encoder->port);
	is_tc = drv_i915_hpd_intel_phy_is_tc(i915, phy);
	ctx.unused = 0;

	/* A PHY compliance test runs the test and nothing else. */
	if (intel_dp->compliance.test_active &&
	    intel_dp->compliance.test_type == DP_TEST_LINK_PHY_TEST_PATTERN) {
		i915_hpd_intel_dp_phy_test(encoder);
		/* just do the PHY test and nothing else */
		return INTEL_HOTPLUG_UNCHANGED;
	}

	/* Detects the connector. */
	state = i915_hpd_intel_encoder_hotplug(encoder, connector);

	/* Without a Type-C link reset, the link is checked (intel_modeset_lock_ctx_retry(): one pass). */
	link_reset = i915_hpd_intel_tc_port_link_reset(dig_port);
	if (!link_reset) {
		ret = 0;
		if (connector->base.connector_type == DRM_MODE_CONNECTOR_HDMIA) {
			ret = i915_hdmi_reset_link(encoder, &ctx);
		} else {
			ret = i915_hpd_intel_dp_retrain_link(encoder, &ctx);
		}

		/* A failed link check is a bug. */
		(void)drv_i915_hpd_warn(world, ret != 0, "ret", "intel_ddi_hotplug");
	}

	/*
	 * Unpowered type-c dongles can take some time to boot and be
	 * responsible, so here giving some time to those dongles to power up
	 * and then retrying the probe.
	 *
	 * On many platforms the HDMI live state signal is known to be
	 * unreliable, so we can't use it to detect if a sink is connected or
	 * not. Instead we detect if it's connected based on whether we can
	 * read the EDID or not. That in turn has a problem during disconnect,
	 * since the HPD interrupt may be raised before the DDC lines get
	 * disconnected (due to how the required length of DDC vs. HPD
	 * connector pins are specified) and so we'll still be able to get a
	 * valid EDID. To solve this schedule another detection cycle if this
	 * time around we didn't detect any change in the sink's connection
	 * status.
	 *
	 * Type-c connectors which get their HPD signal deasserted then
	 * reasserted, without unplugging/replugging the sink from the
	 * connector, introduce a delay until the AUX channel communication
	 * becomes functional. Retry the detection for 5 seconds on type-c
	 * connectors to account for this delay.
	 */
	retry_limit = 1;
	if (is_tc)
		retry_limit = 5;
	if (state == INTEL_HOTPLUG_UNCHANGED &&
	    connector->hotplug_retries < retry_limit &&
	    !dig_port->dp.is_mst)
		state = INTEL_HOTPLUG_RETRY;

	/* Succeeded: reports the hotplug state. */
	return state;
}

/* Reads the live status of a non-Type-C port in SDEISR (lpt_digital_port_connected()). */
static bool
i915_lpt_digital_port_connected(
	struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv;
	u32 bit;
	u32 isr;

	/* The pin's SDE bit. */
	dev_priv = i915_hpd_to_i915(encoder->base.dev);
	bit = dev_priv->display.hotplug.pch_hpd[encoder->hpd_pin];

	/* The pin's SDE bit is set while something is plugged in. */
	isr = i915_hpd_intel_de_read(dev_priv, SDEISR);
	if ((isr & bit) == 0)
		return false;

	/* Succeeded: the port is connected. */
	return true;
}

/*
 * Checks an HDMI link after a hotplug (intel_hdmi_reset_link()).
 *
 * The Linux early returns (the sink is not connected; no crtc drives the
 * connector) are kept; past them the SCDC TMDS-config comparison and the
 * modeset are a recorded step (they need an active HDMI crtc, which comes
 * with the external-display modeset).
 */
static int
i915_hdmi_reset_link(
	struct intel_encoder *encoder,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct intel_hdmi *hdmi;
	struct intel_connector *connector;

	UNUSED_PARAMETER(ctx);

	/* Finds the port's HDMI connector. */
	hdmi = &i915_hpd_enc_to_dig_port(encoder)->hdmi;
	connector = hdmi->attached_connector;

	/* A sink that is not connected needs no reset. */
	if (connector->base.status != connector_status_connected)
		return 0;

	/* A connector no crtc drives has no link to reset. */
	if (connector->base.state == NULL || connector->base.state->crtc == NULL)
		return 0;

	/* Names the unported step. */
	kern_logf("i915: hpd step intel_hdmi_reset_link SCDC check (unported): %s\n", connector->base.name);
	return 0;
}

/* Probes a connector's status, taking the locks itself (drm_helper_probe_detect_ctx()). */
static enum connector_status
i915_drm_helper_probe_detect_ctx(
	struct drm_connector *connector,
	bool force)
{
	const struct drm_connector_helper_funcs *funcs;
	struct drm_modeset_acquire_ctx ctx;
	struct i915_hpd_world *world;
	int ret;
	int warned;

	/* Resolves the connector's callbacks and its world. */
	funcs = connector->helper_private;
	world = i915_hpd_world_of(i915_hpd_to_i915(connector->dev));

	/* Starts a lock context (nothing is acquired). */
	drm_modeset_acquire_init(&ctx, 0);
	ctx.unused = 0;

	/* Detects under the connection lock, backing off and retrying on a deadlock. */
	for (;;) {
		ret = i915_hpd_drm_modeset_lock(&connector->dev->mode_config.connection_mutex, &ctx);
		if (!ret) {
			if (funcs->detect_ctx) {
				ret = funcs->detect_ctx(connector, &ctx, force);
			} else if (connector->funcs->detect) {
				ret = connector->funcs->detect(connector, force);
			} else {
				ret = connector_status_connected;
			}
		}

		/* Anything but a deadlock ends the detection. */
		if (ret != -I915_HPD_EDEADLK)
			break;

		/* Backs off and tries again. */
		drm_modeset_backoff(&ctx);
	}

	/* A negative answer is a bug of the detect; the status is unknown. */
	warned = drv_i915_hpd_warn(world, ret < 0, "ret < 0", "drm_helper_probe_detect_ctx");
	if (warned)
		ret = connector_status_unknown;

	/* A changed status is a new epoch of the connector. */
	if (ret != (int)connector->status)
		connector->epoch_counter += 1;

	/* Ends the lock context. */
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	/* Succeeded: reports the connector status. */
	return (enum connector_status)ret;
}

/* Returns the name of a connector status given as an integer. */
static const char *
i915_hpd_status_name(
	int status)
{
	const char *name;

	/* Names the status. */
	name = i915_hpd_drm_get_connector_status_name((enum connector_status)status);

	/* Succeeded: reports the name. */
	return name;
}

/* Records a pulse of a DP port: intel_dp_hpd_pulse() is not ported and answers IRQ_HANDLED. */
static enum irqreturn
i915_hpd_dp_pulse_step(
	struct intel_digital_port *dig_port,
	bool long_hpd)
{
	struct i915_hpd_world *world;

	/* Counts the pulse and names the step. */
	world = i915_hpd_world_of(i915_hpd_to_i915(dig_port->base.base.dev));
	world->hpd.hpd_pulse_steps++;
	kern_logf("i915: hpd step intel_dp_hpd_pulse (unported): %s %s pulse -> IRQ_HANDLED\n", dig_port->base.base.name, long_hpd ? "long" : "short");

	/* Succeeded: the pulse counts as handled, as Linux answers for an eDP long pulse. */
	return IRQ_HANDLED;
}

/* Records a DP connector detect: intel_dp_detect() is not ported and answers unknown. */
static int
i915_hpd_dp_detect_step(
	struct drm_connector *connector,
	struct drm_modeset_acquire_ctx *ctx,
	bool force)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(force);

	/* Names the step. */
	kern_logf("i915: hpd step intel_dp_detect (unported): %s -> unknown\n", connector->name);

	/* Succeeded: the status is unknown. */
	return connector_status_unknown;
}

/* Records a Type-C live-status check: intel_tc_port_connected() is not ported and answers false. */
static bool
i915_hpd_tc_connected_step(
	struct intel_encoder *encoder)
{
	/* Names the step. */
	kern_logf("i915: hpd step intel_tc_port_connected (unported): %s -> false\n", encoder->base.name);

	/* Succeeded: the port counts as not connected. */
	return false;
}

/* Runs intel_ddi_hotplug() for the hotplug work and records the call. */
static enum intel_hotplug_state
i915_hpd_hotplug_recorded(
	struct intel_encoder *encoder,
	struct intel_connector *connector)
{
	enum intel_hotplug_state (*hotplug)(struct intel_encoder *, struct intel_connector *);
	struct i915_hpd_world *world;
	struct i915_hpd_hotplug_record *r;
	struct i915_hpd_edid_info ei;
	enum intel_hotplug_state st;
	unsigned idx;
	int old;
	int live;
	u32 isr;
	const char *state_name;

	/* Finds the connector's index and the status before the detection. */
	world = i915_hpd_world_of(i915_hpd_to_i915(encoder->base.dev));
	idx = (unsigned)(connector - world->hpd_conns);
	old = connector->base.status;

	/* Runs the DDI hotplug handler. */
	hotplug = i915_hpd_ddi_hotplug_fn();
	st = hotplug(encoder, connector);

	/* Samples the pin's live status after the detection. */
	isr = drv_i915_hpd_read(world, I915_HPD_REG_SDEISR);
	live = 0;
	if ((isr & world->hpd_i915.display.hotplug.pch_hpd[encoder->hpd_pin]) != 0u)
		live = 1;

	/* Counts the event, and a retry it armed. */
	world->hpd.hotplug_events++;
	if (st == INTEL_HOTPLUG_RETRY)
		world->hpd.retries_armed++;

	/* Records the call while there is room. */
	if (world->hpd.n_hot < I915_HPD_MAX_HOTPLUG_RECORDS) {
		r = &world->hpd.hot[world->hpd.n_hot];
		world->hpd.n_hot++;
		r->tick = sched_ticks();
		r->connector = idx;
		r->pin = encoder->hpd_pin;
		r->retries = connector->hotplug_retries;
		r->old_status = old;
		r->new_status = connector->base.status;
		r->state = st;
		r->live = live;
		drv_i915_hpd_edid_info(world, &ei);
		r->edid_rc = ei.rc;
		r->edid_blocks = ei.blocks;
	}

	/* Counts the HDMI connector's transitions. */
	if ((int)idx == world->hpd.hdmi && old != (int)connector->base.status) {
		if (connector->base.status == connector_status_connected) {
			world->hpd.to_connected++;
		} else if (connector->base.status == connector_status_disconnected) {
			world->hpd.to_disconnected++;
		}
	}

	/* Names the outcome. */
	if (st == INTEL_HOTPLUG_CHANGED) {
		state_name = "CHANGED";
	} else if (st == INTEL_HOTPLUG_RETRY) {
		state_name = "RETRY";
	} else {
		state_name = "UNCHANGED";
	}

	/* Logs the status the connector adopted, and what it was decided from: the live status and the EDID read. */
	drv_i915_hpd_edid_info(world, &ei);
	kern_logf("i915: HPD-EVENT %s pin=%d retry=%d status %s -> %s (%s) | live=%d EDID rc=%d blocks=%u digital=%u | epoch=%llu t=%llu\n",
		  connector->base.name,
		  encoder->hpd_pin,
		  connector->hotplug_retries,
		  i915_hpd_status_name(old),
		  i915_hpd_status_name(connector->base.status),
		  state_name,
		  live,
		  ei.rc,
		  ei.blocks,
		  ei.digital,
		  (unsigned long long)connector->base.epoch_counter,
		  (unsigned long long)sched_ticks());

	/* Succeeded: reports the hotplug state. */
	return st;
}

/*
 * Makes one digital port and one connector per encoder the output setup
 * found, as intel_setup_outputs() would have.
 */
static void
i915_hpd_make_objects(
	struct i915_hpd_world *world,
	const struct i915_display_nogem *nogem)
{
	const struct i915_encoder *pe;
	struct intel_digital_port *dp;
	struct intel_connector *ic;
	char *enc_name;
	char *conn_name;
	unsigned i;
	unsigned n_dp;
	unsigned n_hdmi;
	unsigned n_edp;
	unsigned ddc_pin;
	int is_edp;

	/* Starts with no connector of any kind and no HDMI connector watched. */
	n_dp = 0u;
	n_hdmi = 0u;
	n_edp = 0u;
	world->hpd.num = 0u;
	world->hpd.hdmi = -1;

	/* Makes the objects of each encoder while there is room. */
	for (i = 0u; i < nogem->num_encoders && world->hpd.num < I915_HPD_MAX_CONNECTORS; i++) {
		pe = &nogem->encoders[i];
		dp = &world->hpd_ports[world->hpd.num];
		ic = &world->hpd_conns[world->hpd.num];
		enc_name = world->hpd_enc_names[world->hpd.num];
		conn_name = world->hpd_conn_names[world->hpd.num];

		/* An eDP encoder is a DP encoder of an internal connector on port A. */
		is_edp = 0;
		if (pe->init_dp && (pe->device_type & I915_HPD_DEVICE_TYPE_INTERNAL) != 0u && pe->port == I915_PORT_A)
			is_edp = 1;

		/* Starts both objects empty. */
		kern_memset(dp, 0, sizeof(*dp));
		kern_memset(ic, 0, sizeof(*ic));

		/* The encoder is named "DDI <port letter>". */
		enc_name[0] = 'D';
		enc_name[1] = 'D';
		enc_name[2] = 'I';
		enc_name[3] = ' ';
		enc_name[4] = (char)('A' + pe->port);
		enc_name[5] = '\0';

		/*
		 * The encoder: its id, name, output type, port and HPD pin, and
		 * intel_ddi_hotplug() as its hotplug handler, recorded.
		 *
		 * XXX: the HPD pin is computed for display version 13 whatever
		 * the device's version is.
		 */
		dp->base.base.dev = &world->hpd_i915.drm;
		dp->base.base.base.id = (int)world->hpd.num + 1;
		dp->base.base.name = enc_name;
		dp->base.type = INTEL_OUTPUT_DDI;
		if (is_edp)
			dp->base.type = INTEL_OUTPUT_EDP;
		dp->base.port = (enum port)pe->port;
		dp->base.hpd_pin = (enum hpd_pin)drv_i915_ddi_hpd_pin(13, pe->port);
		dp->base.hotplug = i915_hpd_hotplug_recorded;

		/* The live-status check: SDEISR, or the unported Type-C check. */
		if (pe->is_tc) {
			dp->connected = i915_hpd_tc_connected_step;
		} else {
			dp->connected = i915_hpd_lpt_connected_fn();
		}

		/* A DP encoder takes pulses (intel_dp_hpd_pulse(): an unported step). */
		if (pe->init_dp)
			dp->hpd_pulse = i915_hpd_dp_pulse_step;

		/* The connector, as drm_connector_init() leaves it: attached, unknown, driven by no crtc. */
		ic->encoder = &dp->base;
		ic->base.dev = &world->hpd_i915.drm;
		ic->base.base.id = (int)world->hpd.num + 1;
		ic->base.status = connector_status_unknown;
		ic->base.state = &world->hpd_conn_states[world->hpd.num];
		world->hpd_conn_states[world->hpd.num].crtc = NULL;

		/* The connector's kind: HDMI, eDP, or DP. */
		if (pe->init_hdmi && !pe->init_dp) {
			ic->base.connector_type = DRM_MODE_CONNECTOR_HDMIA;
			ic->base.funcs = drv_i915_hpd_hdmi_connector_funcs();
			ic->base.helper_private = &i915_hpd_hdmi_helper_funcs;

			/*
			 * intel_hdmi_init_connector(): ddc = intel_gmbus_get_adapter(ddc_pin);
			 * port B -> GMBUS_PIN_2_BXT (the VBT's ddc_pin 2 maps to the same pin).
			 */
			ddc_pin = (unsigned)(1 + pe->port);
			if (pe->port == I915_PORT_B)
				ddc_pin = GMBUS_PIN_2_BXT;
			ic->base.ddc = drv_i915_hpd_gmbus_adapter(&world->hpd_i915, ddc_pin);
			ic->polled = CONNECTOR_POLL_HPD;
			dp->hdmi.attached_connector = ic;

			/* Named "HDMI-A-<n>"; the first HDMI connector is the one the path watches. */
			kern_memcpy(conn_name, "HDMI-A-", 7);
			conn_name[7] = (char)('1' + n_hdmi);
			conn_name[8] = '\0';
			n_hdmi++;
			if (world->hpd.hdmi < 0)
				world->hpd.hdmi = (int)world->hpd.num;
		} else if (is_edp) {
			ic->base.connector_type = DRM_MODE_CONNECTOR_eDP;
			ic->base.funcs = &i915_hpd_dp_connector_funcs;
			ic->base.helper_private = &i915_hpd_dp_helper_funcs;
			dp->dp.attached_connector = ic;

			/* Named "eDP-<n>". */
			kern_memcpy(conn_name, "eDP-", 4);
			conn_name[4] = (char)('1' + n_edp);
			conn_name[5] = '\0';
			n_edp++;
		} else {
			ic->base.connector_type = DRM_MODE_CONNECTOR_DisplayPort;
			ic->base.funcs = &i915_hpd_dp_connector_funcs;
			ic->base.helper_private = &i915_hpd_dp_helper_funcs;
			ic->polled = CONNECTOR_POLL_HPD;
			dp->dp.attached_connector = ic;

			/* Named "DP-<n>". */
			kern_memcpy(conn_name, "DP-", 3);
			conn_name[3] = (char)('1' + n_dp);
			conn_name[4] = '\0';
			n_dp++;
		}

		/* i915_hpd_poll_init_work(): HPD enabled, so the connector polls as its kind does. */
		ic->base.polled = ic->polled;
		ic->base.name = conn_name;
		world->hpd_last_status[world->hpd.num] = connector_status_unknown;
		world->hpd.num++;
	}
}
