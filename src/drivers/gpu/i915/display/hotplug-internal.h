/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Type and constant definitions derived from the Linux kernel (drivers/gpu/drm/i915/display/intel_display_types.h),
 * which carries the following notice.
 *
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
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
 */

/*
 * The hotplug environment: what the Linux hotplug text needs.
 *
 * The HDMI hotplug receive path is the Linux v6.8.12 text of
 * intel_hotplug.c, intel_hotplug_irq.c, intel_ddi.c (intel_ddi_hotplug),
 * intel_dp.c (intel_digital_port_connected), intel_gmbus.c, intel_hdmi.c
 * (the detect path), drm_probe_helper.c and drm_connector.c (the status
 * names).  The chain it runs is
 *
 *   icp_irq_handler -> intel_get_hpd_pins -> intel_hpd_irq_handler ->
 *   i915_hotplug_work_func -> intel_ddi_hotplug -> drm_helper_probe_detect ->
 *   intel_hdmi_detect -> intel_digital_port_connected.
 *
 * This header maps the Linux types that text touches onto zedBSD: the kernel
 * spinlock and mutex (same names and contracts), the driver's work queue and
 * delayed work (../workqueue.h), the scheduler tick as jiffies, and a device
 * that holds only the hotplug state.  The encoders and connectors are the
 * subset of fields the text reads; the owner of struct i915_hpd_world makes
 * them from the encoders intel_setup_outputs() would have made.
 *
 * Every piece of state this environment keeps is in struct i915_hpd_world.
 * The device the Linux text passes around, struct drm_i915_private, is the
 * world's hpd_i915 member, so a helper that is handed the device finds the
 * world with i915_hpd_world_of() instead of reading a global.
 *
 * Errors: functions return zedBSD positive errno at their boundaries.  The
 * I915_HPD_E* constants carry the Linux numbers the Linux text compares and
 * returns internally (a GMBUS -ENXIO crosses into the EDID reader of the DP
 * environment, which compares Linux numbers); they exist only where a Linux
 * numeric value is observable.
 *
 * A translation unit includes this header or another Linux environment,
 * never two (see internal.h).
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_HOTPLUG_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_HOTPLUG_INTERNAL_H

#include "internal.h"

#ifdef I915_DISPLAY_LINUX_WORLD
#error "hotplug-internal.h cannot be included together with another Linux display environment"
#endif
#define I915_DISPLAY_LINUX_WORLD "hotplug"

/* This translation unit is compiled in the hotplug environment. */
#define I915_DISPLAY_WORLD_HOTPLUG 1

#include <uapi/errno.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>

/* The Linux spelling of a packed structure, which the DP SDP definitions use. */
#define __packed __attribute__((packed))

#include "../intel/ddi.h"
#include "../intel/hotplug.h"
#include "../intel/connector.h"
#include "../intel/dp.h"

/*
 * Linux errno numbers of the hotplug text.
 *
 * The GMBUS transfer returns -ENXIO for a NAK, -ETIMEDOUT for a stuck bus and
 * -EAGAIN to ask for a retry; drm_helper_probe_detect() compares with
 * -EDEADLK; the bit-banging step answers -EIO.  Every other errno of the
 * environment is zedBSD's own.
 */
#define I915_HPD_EIO 5
#define I915_HPD_ENXIO 6
#define I915_HPD_EAGAIN 11
#define I915_HPD_EDEADLK 35
#define I915_HPD_ETIMEDOUT 110

/* Marks a parameter a helper does not read (as i915.h spells it). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* The number of bits of a type (the Linux BITS_PER_TYPE()). */
#define BITS_PER_TYPE(t) (8u * (unsigned)sizeof(t))

/* Fails the build when the condition holds (the Linux BUILD_BUG_ON()). */
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON")

/* The PCH display register base (i915_reg.h). */
#define PCH_DISPLAY_BASE 0xc0000u

/*
 * How many EDID blocks one connector keeps.
 *
 * drm_edid_read_ddc() reads the base block and at most three extensions
 * into the connector's slot of struct i915_hpd_world.
 */
#define I915_HPD_EDID_MAX_BLOCKS 4u

/*
 * Diagnostics: the Linux messages, printed.
 *
 * Hotplug events are rare, so every level is logged, the ones raised in
 * interrupt context included.  The device argument is not evaluated.
 */
#define I915_HPD_DRM_DBG(dev, fmt, ...) kern_logf("i915: hpd (drm_dbg) " fmt, ##__VA_ARGS__)
#define I915_HPD_DRM_DBG_KMS(dev, fmt, ...) kern_logf("i915: hpd (drm_dbg_kms) " fmt, ##__VA_ARGS__)
#define I915_HPD_DRM_INFO(dev, fmt, ...) kern_logf("i915: hpd (drm_info) " fmt, ##__VA_ARGS__)
#define I915_HPD_DRM_ERR(dev, fmt, ...) kern_logf("i915: hpd (drm_err) " fmt, ##__VA_ARGS__)

/*
 * Linux WARN_ON(): logs and counts a warning in the world, and reports the
 * condition.
 *
 * The Linux form names no device; this one takes the world whose warning
 * counter it moves (the old form reached a global instance).
 */
#define I915_HPD_WARN_ON(world, x) drv_i915_hpd_warn((world), !!(x), #x, __func__)

/* Linux drm_WARN_ON(): the same, for the world the drm device belongs to. */
#define I915_HPD_DRM_WARN_ON(dev, x) \
	drv_i915_hpd_warn(i915_hpd_world_of(i915_hpd_to_i915(dev)), !!(x), #x, __func__)

/* Linux drm_WARN_ONCE(): logs the format text itself; every occurrence is logged. */
#define drm_WARN_ONCE(dev, cond, fmt, ...) \
	drv_i915_hpd_warn(i915_hpd_world_of(i915_hpd_to_i915(dev)), !!(cond), fmt, __func__)

/* Linux lockdep_assert_held(): no lock dependency checker; the lock is not checked. */
#define I915_HPD_LOCKDEP_ASSERT_HELD(l) ((void)(l))

/* Linux time_after_eq() on jiffies: a is at or after b, across a wrap. */
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)

/* Linux time_in_range(): a lies in [b, c]; a is evaluated twice. */
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_after_eq(c, a))

/* Linux mutex_is_locked(): whether the calling thread owns the kernel mutex. */
#define mutex_is_locked(m) mutex_owned(m)

/*
 * Linux wait_for(): polls a condition every 10-50 microseconds until it holds
 * or MS milliseconds of scheduler ticks have passed.
 *
 * The condition is evaluated once more after the deadline expired, so a
 * condition that became true while the poller slept still succeeds.
 * Reports 0 or -I915_HPD_ETIMEDOUT.
 */
#define I915_HPD_WAIT_FOR(COND, MS) ({ \
	uint64_t _wf_dl = sched_ticks() + ((uint64_t)(MS) * KERN_CLOCK_HZ + 999u) / 1000u + 1u; \
	int _wf_ret; \
	for (;;) { \
		bool _wf_expired = sched_ticks() > _wf_dl; \
		if (COND) { \
			_wf_ret = 0; \
			break; \
		} \
		if (_wf_expired) { \
			_wf_ret = -I915_HPD_ETIMEDOUT; \
			break; \
		} \
		kern_usleep_range(10u, 50u); \
	} \
	_wf_ret; \
})

/*
 * Linux wait_for_us(): polls a condition US + 1 times, one microsecond apart.
 *
 * Reports 0 or -I915_HPD_ETIMEDOUT.
 */
#define I915_HPD_WAIT_FOR_US(COND, US) ({ \
	int _wu_ret = -I915_HPD_ETIMEDOUT; \
	unsigned _wu_i; \
	for (_wu_i = 0u; _wu_i <= (unsigned)(US); _wu_i++) { \
		if (COND) { \
			_wu_ret = 0; \
			break; \
		} \
		kern_usleep_range(1u, 1u); \
	} \
	_wu_ret; \
})

/*
 * Linux intel_de_wait_for_register_fw(): waits until the masked register
 * reads the value, polling like I915_HPD_WAIT_FOR().
 */
#define intel_de_wait_for_register_fw(i915, r, mask, value, ms) \
	I915_HPD_WAIT_FOR((drv_i915_hpd_read(i915_hpd_world_of(i915), (r).reg) & (mask)) == (value), (ms))

/*
 * Linux intel_uncore_rmw(): read-modify-write of a register through the
 * world the uncore belongs to; reports the value read.
 */
#define intel_uncore_rmw(uncore, r, clear, set) \
	drv_i915_hpd_rmw(i915_hpd_world_of(container_of((uncore), struct drm_i915_private, uncore)), (r).reg, (clear), (set))

/*
 * The GMBUS wait queue.
 *
 * The Linux text polls GMBUS2 in wait_for(); the queue would only wake the
 * poller early, so waiting on it is nothing, and a wake-up is counted in the
 * world the queue belongs to.
 */
#define DEFINE_WAIT(w) int w = 0
#define add_wait_queue(q, w) ((void)(q), (void)(w))
#define remove_wait_queue(q, w) ((void)(q), (void)(w))
#define wake_up_all(q) \
	drv_i915_hpd_gmbus_woken(i915_hpd_world_of(container_of((q), struct drm_i915_private, display.gmbus.wait_queue)))

/* The platform checks of the GMBUS text: display version 4+ has the GMBUS interrupt, 10+ burst reads. */
#define HAS_GMBUS_IRQ(i915) (i915_hpd_display_ver(i915) >= 4)
#define HAS_GMBUS_BURST_READ(i915) (i915_hpd_display_ver(i915) >= 10)
#define IS_GEMINILAKE(i915) (0)
#define IS_BROXTON(i915) (0)
#define HAS_PCH_SPT(i915) (0)
#define HAS_PCH_CNP(i915) (0)

/*
 * The modeset lock graph.
 *
 * The hotplug work is the only connector user and holds mode_config.mutex
 * around every detection, so the connection mutex and the acquire context
 * take nothing, and no -EDEADLK back-off happens.
 */
#define drm_modeset_acquire_init(ctx, flags) ((void)(ctx))
#define drm_modeset_backoff(ctx) ((void)(ctx))
#define drm_modeset_drop_locks(ctx) ((void)(ctx))
#define drm_modeset_acquire_fini(ctx) ((void)(ctx))

/* Linux intel_modeset_lock_ctx_retry(): one pass, with ret cleared. */
#define intel_modeset_lock_ctx_retry(ctx, state, flags, ret) \
	for (int _hpd_once = ((ret) = 0, 1); _hpd_once; _hpd_once = 0)

/* DRM_MODE_CONNECTOR_* (include/uapi/drm/drm_mode.h, the UAPI values). */
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define DRM_MODE_CONNECTOR_HDMIA 11
#define DRM_MODE_CONNECTOR_eDP 14

/* The I2C read flag of struct i2c_msg (include/uapi/linux/i2c.h). */
#define I2C_M_RD 0x0001

/* intel_display_types.h accessors that are plain member walks. */
#define hdmi_to_dig_port(h) container_of((h), struct intel_digital_port, hdmi)
#define intel_attached_hdmi(c) (&i915_hpd_enc_to_dig_port(i915_hpd_intel_attached_encoder(c))->hdmi)

/* The platform: ADL-P class, display present, no GMCH, DP MST available. */
#define HAS_DISPLAY(i915) (1)
#define HAS_GMCH(i915) (0)
#define HAS_DP_MST(i915) (1)
#define intel_display_device_enabled(i915) (true)

/*
 * Linux with_intel_display_power(): runs the body once with a reference on
 * the power domain, when the reference could be taken.
 */
#define with_intel_display_power(i915, domain, wf) \
	for ((wf) = drv_i915_hpd_intel_display_power_get((i915), (domain)); \
	     (wf); \
	     i915_hpd_intel_display_power_put_async((i915), (domain), (wf)), (wf) = 0)

/* Linux runtime PM: always awake; the wakeref is a nonzero token. */
#define intel_runtime_pm_get(rpm) ((intel_wakeref_t)1)
#define intel_runtime_pm_put(rpm, wf) ((void)(wf))

/* The CEC notifier: not ported; the physical address is not published. */
#define cec_notifier_set_phys_addr(n, pa) ((void)(n), (void)(pa))
#define cec_notifier_phys_addr_invalidate(n) ((void)(n))

/*
 * Link names.
 *
 * Every global function of the hotplug text is linked under an i915_hpd_
 * name, so it cannot collide with the same Linux function of another
 * environment.  The Linux names whose meaning differs in another environment
 * (intel_port_to_phy, intel_phy_is_tc, intel_digital_port_connected,
 * intel_display_power_get, intel_display_power_put) are not defined here;
 * their callers use the i915_hpd_ names declared below.
 */
#define intel_tc_port_link_reset i915_hpd_intel_tc_port_link_reset
#define intel_dp_phy_test i915_hpd_intel_dp_phy_test
#define intel_dp_retrain_link i915_hpd_intel_dp_retrain_link
#define drm_edid_free i915_hpd_drm_edid_free
#define intel_hpd_irq_setup i915_hpd_intel_hpd_irq_setup
#define intel_gmbus_irq_handler i915_hpd_intel_gmbus_irq_handler
#define drm_kms_helper_poll_reschedule i915_hpd_drm_kms_helper_poll_reschedule
#define drm_kms_helper_connector_hotplug_event i915_hpd_drm_kms_helper_connector_hotplug_event
#define drm_kms_helper_hotplug_event i915_hpd_drm_kms_helper_hotplug_event
#define intel_hpd_irq_handler i915_hpd_intel_hpd_irq_handler
#define icp_irq_handler i915_hpd_icp_irq_handler
#define intel_encoder_hotplug i915_hpd_intel_encoder_hotplug
#define intel_hpd_init_early i915_hpd_intel_hpd_init_early
#define intel_hpd_cancel_work i915_hpd_intel_hpd_cancel_work
#define drm_helper_probe_detect i915_hpd_drm_helper_probe_detect
#define drm_get_connector_status_name i915_hpd_drm_get_connector_status_name
#define intel_gmbus_reset i915_hpd_intel_gmbus_reset
#define intel_gmbus_force_bit i915_hpd_intel_gmbus_force_bit
#define intel_gmbus_is_forced_bit i915_hpd_intel_gmbus_is_forced_bit
#define drm_edid_read_ddc i915_hpd_drm_edid_read_ddc
#define drm_edid_connector_update i915_hpd_drm_edid_connector_update
#define drm_edid_is_digital i915_hpd_drm_edid_is_digital
#define intel_irqs_enabled i915_hpd_intel_irqs_enabled

/* Linux mod_delayed_work(): the new delay replaces a pending one. */
#define mod_delayed_work(q, d, delay) i915_hpd_mod_delayed_work((q), (d), (delay))

/*
 * The Linux IRQ handler result (include/linux/irqreturn.h).
 */
enum irqreturn {
	IRQ_NONE = 0,
	IRQ_HANDLED = 1,
	IRQ_WAKE_THREAD = 2
};

/*
 * The DP dual-mode adaptor type (drm_dp_dual_mode_helper.h).
 *
 * Only the value intel_hdmi_unset_edid() stores exists here.
 */
enum drm_dp_dual_mode_type {
	DRM_DP_DUAL_MODE_NONE = 0
};

/*
 * The power domains the hotplug text names.
 *
 * The owner maps them onto the display power domains: GMBUS onto the GMBUS
 * domain, everything else onto the display core.
 */
enum intel_display_power_domain {
	POWER_DOMAIN_DISPLAY_CORE,
	POWER_DOMAIN_GMBUS
};

/*
 * A power-domain reference, as intel_display_power_get() hands it out.
 *
 * Zero means no reference was taken.
 */
typedef unsigned long intel_wakeref_t;

struct drm_i915_private;
struct drm_connector;
struct drm_crtc;
struct drm_edid;
struct i2c_adapter;
struct intel_connector;
struct work_struct;

/*
 * A Linux work queue.
 *
 * One driver work queue runs the works; the timer queue on top of it arms
 * the delayed works queued here.  It lives in struct i915_hpd_world from the
 * start of the hotplug path to its stop.
 */
struct workqueue_struct {
	struct i915_workqueue wq;
	struct i915_timer_queue tq;
};

/*
 * A Linux work item.
 *
 * It lives inside its owner (struct intel_hotplug); the work queue calls the
 * world's trampoline with it, which runs func.  q is the queue it was last
 * queued on, which a synchronous cancel needs.
 */
struct work_struct {
	struct i915_work kwork;
	void (*func)(struct work_struct *work);
	struct workqueue_struct *q;
};

/*
 * A Linux delayed work item.
 *
 * dw is armed on the timer queue of work.q and runs work.func when due; the
 * kwork of work is not used for it.
 */
struct delayed_work {
	struct work_struct work;
	struct i915_delayed_work dw;
};

#include "../intel/hotplug-types.h"

/*
 * One I2C transfer segment (include/uapi/linux/i2c.h).
 *
 * The same type as the DP environment's, so the EDID reader there takes the
 * adapter this environment makes.
 */
struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

/*
 * The operations of an I2C adapter.
 *
 * GMBUS adapters carry the locked GMBUS transfer; the bit-banging algorithm
 * is not ported.
 */
struct i2c_algorithm {
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	u32 (*functionality)(struct i2c_adapter *adap);
};

/*
 * One I2C bus, as the EDID reader sees it.
 *
 * It is embedded in struct intel_gmbus and lives as long as the world.
 */
struct i2c_adapter {
	const struct i2c_algorithm *algo;
	void *algo_data;
	int retries;
	char name[48];
};

/*
 * The bit-banging state of an adapter; unused, the algorithm is not ported.
 */
struct i2c_algo_bit_data {
	int unused;
};

/*
 * One GMBUS pin's I2C bus (intel_gmbus.c).
 *
 * The world keeps one per GMBUS pin; a bus is made on the first request for
 * its pin and forgotten when the hotplug path starts again.
 */
struct intel_gmbus {
	struct i2c_adapter adapter;
	u32 force_bit;
	u32 reg0;
	i915_reg_t gpio_reg;
	struct i2c_algo_bit_data bit_algo;
	struct drm_i915_private *i915;
};

/*
 * The modeset acquire context; nothing is acquired (see drm_modeset_lock()).
 */
struct drm_modeset_acquire_ctx {
	int unused;
};

/*
 * A modeset lock; nothing is locked.
 */
struct drm_modeset_lock {
	int unused;
};

/*
 * The drm device: the mode configuration lock the hotplug work holds around
 * every detection, and the connection mutex of the Linux text.
 *
 * It is the drm member of the world's device.
 */
struct drm_device {
	struct {
		struct mutex mutex;
		struct drm_modeset_lock connection_mutex;
	} mode_config;
};

/*
 * A connector's atomic state: only the crtc that drives it.
 *
 * The world keeps one per connector; no crtc drives any of them.
 */
struct drm_connector_state {
	struct drm_crtc *crtc;
};

/*
 * The connector callbacks: the forced detect of an HDMI connector.
 */
struct drm_connector_funcs {
	enum connector_status (*detect)(struct drm_connector *connector, bool force);
};

/*
 * The connector helper callbacks: the context-aware detect of a DP connector.
 */
struct drm_connector_helper_funcs {
	int (*detect_ctx)(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx, bool force);
};

/*
 * A drm connector: the fields the hotplug text reads.
 *
 * It is the base of an intel_connector of the world.
 */
struct drm_connector {
	struct drm_device *dev;
	struct {
		int id;
	} base;
	const char *name;
	int connector_type;
	enum connector_status status;
	u64 epoch_counter;
	int force;                      /* enum drm_connector_force: 0 = DRM_FORCE_UNSPECIFIED */
	u8 polled;
	const struct drm_connector_funcs *funcs;
	const struct drm_connector_helper_funcs *helper_private;
	struct i2c_adapter *ddc;
	struct drm_connector_state *state;
	struct {
		u16 source_physical_address;
	} display_info;
};

/*
 * A drm encoder: its device, id and name.
 */
struct drm_encoder {
	struct drm_device *dev;
	struct {
		int id;
	} base;
	const char *name;
};

/*
 * An i915 encoder: its output type, port, hotplug pin and hotplug handler.
 *
 * It is the base of an intel_digital_port of the world.
 */
struct intel_encoder {
	struct drm_encoder base;
	enum intel_output_type type;
	enum port port;
	enum hpd_pin hpd_pin;
	enum intel_hotplug_state (*hotplug)(struct intel_encoder *encoder, struct intel_connector *connector);
};

/*
 * An i915 connector: the encoder it is attached to, its polling mode, its
 * hotplug retry count and the EDID of its last detection.
 *
 * The world keeps one per encoder.
 */
struct intel_connector {
	struct drm_connector base;
	struct intel_encoder *encoder;
	u8 polled;
	int hotplug_retries;
	const struct drm_edid *detect_edid;
};

/*
 * A DP port's hotplug state: the compliance test and the connector.
 */
struct intel_dp {
	struct {
		bool test_active;
		int test_type;
	} compliance;
	bool is_mst;
	struct intel_connector *attached_connector;
};

/*
 * An HDMI port's hotplug state: the connector, the dual-mode adaptor and the
 * CEC notifier.
 */
struct intel_hdmi {
	struct intel_connector *attached_connector;
	struct {
		enum drm_dp_dual_mode_type type;
		int max_tmds_clock;
	} dp_dual_mode;
	void *cec_notifier;
};

/*
 * A digital port: the encoder, its DP and HDMI state, and the port's live
 * status and pulse handlers.
 *
 * The world keeps one per encoder intel_setup_outputs() would have made.
 */
struct intel_digital_port {
	struct intel_encoder base;
	struct intel_dp dp;
	struct intel_hdmi hdmi;
	bool (*connected)(struct intel_encoder *encoder);
	enum irqreturn (*hpd_pulse)(struct intel_digital_port *dig_port, bool long_hpd);
};

/*
 * The uncore; register access goes through the world instead.
 */
struct intel_uncore {
	int unused;
};

/*
 * The device of the hotplug text: only the hotplug state.
 *
 * It is the hpd_i915 member of struct i915_hpd_world; irq_lock protects the
 * hotplug statistics and bits, as in Linux.
 */
struct drm_i915_private {
	struct drm_device drm;
	struct intel_uncore uncore;
	struct spinlock irq_lock;
	bool display_irqs_enabled;
	struct workqueue_struct *unordered_wq;
	struct {
		int unused;
	} runtime_pm;
	struct {
		struct intel_hotplug hotplug;
		struct {
			const void *hotplug;
		} funcs;
		struct {
			u32 mmio_base;
			struct mutex mutex;
			int wait_queue;
		} gmbus;
	} display;
};

/*
 * The connector walk of the Linux text: the next index into the world's
 * connectors.
 */
struct drm_connector_list_iter {
	unsigned idx;
};

/*
 * One read EDID, as the Linux text holds it.
 *
 * The bytes belong to the connector's slot in the world; drm_edid_free()
 * does not free them.
 */
struct drm_edid {
	const u8 *edid;
	size_t size;
};

/*
 * The EDID a connector read last, and the one it adopted.
 *
 * The world keeps one per connector.  buf holds the last read and e points
 * into it; stored is the connector's EDID property, which a changed EDID
 * replaces while moving the connector's epoch counter.
 */
struct i915_hpd_edid_slot {
	/* The blocks drm_edid_read_ddc() read last. */
	u8 buf[I915_HPD_EDID_MAX_BLOCKS * 128u];

	/* The EDID handed to the Linux text: buf and the bytes read. */
	struct drm_edid e;

	/* The connector's EDID property (what the epoch compares). */
	u8 stored[I915_HPD_EDID_MAX_BLOCKS * 128u];

	/* How many bytes of stored are valid. */
	size_t stored_size;

	/* Nonzero once drm_edid_connector_update() stored an EDID. */
	int have_stored;
};

/*
 * One running hotplug path: what it was started with, its liveness, its
 * counters and the records the tests read.
 *
 * It is the hpd member of struct i915_hpd_world, cleared by every start.
 */
struct i915_hpd_instance {
	/* The zedBSD hotplug state intel_hpd_irq_setup() programs. */
	struct i915_hotplug *hp;

	/* The MMIO BAR the registers are read through (NULL for a model). */
	struct i915_mmio *m;

	/* The power domains GMBUS and the display core are taken from (NULL for a model). */
	struct i915_power_domains *pd;
	struct i915_pw_ctx *c;

	/* The fake registers of a model test (NULL for the hardware). */
	struct i915_hpd_fake_regs *fake;

	/* The PCH type and whether the device's interrupts are on, as the start was told. */
	int pch_type;
	int irqs_enabled;

	/* Nonzero from a successful start to the stop. */
	int started;

	/* Nonzero while the unordered and DP work queues exist. */
	int wq_created;

	/* Unused. */
	int mutex_inited;

	/* This instance drives fake registers (a model test). */
	int model;

	/* The IRQ entry is open: an SDE interrupt enters the Linux handler. */
	volatile int live;

	/* IRQ entries running now; the stop waits for zero after closing the entry. */
	volatile unsigned inflight;

	/* How many connectors (and digital ports) the start made. */
	unsigned num;

	/* The index of the first HDMI connector, or -1. */
	int hdmi;

	/* What the IRQ's read-modify-write of SHOTPLUG_CTL_DDI read. */
	u32 cur_dig;

	/* IRQ counters. */
	unsigned irq_entries;
	unsigned irq_dropped;
	unsigned ddi_triggers;
	unsigned gmbus_irqs;

	/* Work counters. */
	unsigned hotplug_works;
	unsigned digport_works;
	unsigned reenable_works;
	unsigned retries_armed;

	/* Steps and events. */
	unsigned hpd_pulse_steps;
	unsigned hotplug_events;
	unsigned irq_setups;
	unsigned storms;
	unsigned warnings;

	/* HDMI connector status transitions. */
	unsigned to_connected;
	unsigned to_disconnected;

	/* How many records of irq[] and hot[] are filled. */
	unsigned n_irq;
	unsigned n_hot;

	/* The hotplug interrupts, in order. */
	struct i915_hpd_irq_record irq[I915_HPD_MAX_IRQ_RECORDS];

	/* The encoder->hotplug() calls of the hotplug work, in order. */
	struct i915_hpd_hotplug_record hot[I915_HPD_MAX_HOTPLUG_RECORDS];
};

/*
 * Everything the hotplug environment keeps.
 *
 * The display root holds a pointer to it; its owner allocates it zeroed
 * before the first start.  Unless a field says otherwise it is written by
 * the start and the stop (the probe thread, with the IRQ entry closed) and
 * read by the hotplug works and the IRQ entry while the path runs.
 */
struct i915_hpd_world {
	/*
	 * The running path.  Cleared by every start; live and inflight are
	 * accessed atomically by the IRQ entry and the stop.
	 */
	struct i915_hpd_instance hpd;

	/*
	 * The device the Linux text works on.  Cleared by every start; its
	 * hotplug state is protected by its irq_lock as in Linux, its
	 * connectors by drm.mode_config.mutex.
	 */
	struct drm_i915_private hpd_i915;

	/* The unordered work queue, created by the start and destroyed by the stop. */
	struct workqueue_struct hpd_unordered_wq;

	/* The DP work queue (display.hotplug.dp_wq), created by the start and destroyed by the stop. */
	struct workqueue_struct hpd_dp_wq;

	/*
	 * The digital ports, one per encoder of the probe; the first hpd.num
	 * are made by the start.  Read by the hotplug works under
	 * drm.mode_config.mutex.
	 */
	struct intel_digital_port hpd_ports[I915_HPD_MAX_CONNECTORS];

	/* The connectors, one per digital port, made with them. */
	struct intel_connector hpd_conns[I915_HPD_MAX_CONNECTORS];

	/* The connectors' atomic states; no crtc drives any. */
	struct drm_connector_state hpd_conn_states[I915_HPD_MAX_CONNECTORS];

	/* The encoder names ("DDI A") the ports point at. */
	char hpd_enc_names[I915_HPD_MAX_CONNECTORS][16];

	/* The connector names ("HDMI-A-1") the connectors point at. */
	char hpd_conn_names[I915_HPD_MAX_CONNECTORS][16];

	/* The status each connector was made with; written by the start only, never read. */
	int hpd_last_status[I915_HPD_MAX_CONNECTORS];

	/*
	 * The interrupt state spin_lock_irq() saved for the irq_lock of
	 * hpd_i915; written under that lock by the holder and read by its
	 * spin_unlock_irq().
	 */
	unsigned long i915_hpd_irq_saved;

	/* Nonzero once a hardware instance has run in this boot; a model is refused after it. */
	int i915_hpd_real_seen;

	/* Nonzero once a model instance has run in this boot; a hardware instance is refused after it. */
	int i915_hpd_model_seen;

	/*
	 * Hardware SDE interrupts that arrived while a model instance ran and
	 * were not fed to it.  Model tests only; written in IRQ context.
	 */
	unsigned i915_hpd_hw_irqs_during_model;

	/*
	 * The GMBUS buses, one per pin, made on first use and forgotten by
	 * every start.  A transfer holds hpd_i915.display.gmbus.mutex.
	 */
	struct intel_gmbus hpd_gmbus_bus[GMBUS_NUM_PINS];

	/* Nonzero once hpd_i915.display.gmbus.mutex is initialised; cleared with the buses. */
	int hpd_gmbus_mutex_inited;

	/*
	 * The EDID slots, one per connector.  Written by the detection of
	 * the hotplug work (under drm.mode_config.mutex) and cleared by every
	 * start.
	 */
	struct i915_hpd_edid_slot hpd_edid[I915_HPD_MAX_CONNECTORS];

	/* What the last EDID read found; written with the slots. */
	struct i915_hpd_edid_info hpd_edid_last;

	/* EDID reads attempted and failed since the start. */
	unsigned hpd_edid_reads;
	unsigned hpd_edid_fails;
};

/*
 * The world's functions the Linux text reaches (the owner defines them).
 */
int drv_i915_hpd_warn(struct i915_hpd_world *world, int cond, const char *what, const char *where);
uint64_t drv_i915_hpd_sync_deadline(void);

/* Enters the Linux handler for one SDE interrupt (interrupts disabled by the caller). */
void drv_i915_hpd_icp_entry(struct i915_hpd_world *world, uint32_t sde_iir);
void drv_i915_hpd_work_trampoline(void *context);
u32 drv_i915_hpd_read(struct i915_hpd_world *world, u32 reg);
u32 drv_i915_hpd_write(struct i915_hpd_world *world, u32 reg, u32 val);
u32 drv_i915_hpd_rmw(struct i915_hpd_world *world, u32 reg, u32 clear, u32 set);
void drv_i915_hpd_gmbus_woken(struct i915_hpd_world *world);
struct intel_encoder *drv_i915_hpd_encoder_at(struct i915_hpd_world *world, unsigned idx);
struct intel_connector *drv_i915_hpd_connector_next(struct i915_hpd_world *world, struct drm_connector_list_iter *it);
struct i2c_adapter *drv_i915_hpd_gmbus_adapter(struct drm_i915_private *i915, unsigned int pin);
void drv_i915_hpd_gmbus_forget(struct i915_hpd_world *world);

/*
 * The display version of the device the probe found (12 = Tiger Lake,
 * 13 = ADL-P class), from the modeset side.
 */
int drv_i915_lcd_display_ver(void);

/* The EDID reader of the DP environment (the Linux drm_do_probe_ddc_edid()). */
int drv_i915_drm_edid_read(struct i2c_adapter *ddc, u8 *buf, unsigned max_blocks, unsigned *extensions);

/* Bit-banging over GPIO: not ported (a recorded step answering -EIO). */
extern const struct i2c_algorithm i2c_bit_algo;

/*
 * The Linux functions of this environment under their link names, and the
 * surroundings the owner provides (recorded as steps where Linux does more).
 */
enum phy drv_i915_hpd_intel_port_to_phy(struct drm_i915_private *i915, enum port port);
bool drv_i915_hpd_intel_phy_is_tc(struct drm_i915_private *i915, enum phy phy);
bool drv_i915_hpd_intel_digital_port_connected(struct intel_encoder *encoder);
intel_wakeref_t drv_i915_hpd_intel_display_power_get(struct drm_i915_private *i915, enum intel_display_power_domain domain);
void drv_i915_hpd_intel_display_power_put(struct drm_i915_private *i915, enum intel_display_power_domain domain, intel_wakeref_t wf);
void intel_hpd_irq_setup(struct drm_i915_private *i915);
void intel_gmbus_irq_handler(struct drm_i915_private *i915);
void drm_kms_helper_poll_reschedule(struct drm_device *dev);
void drm_kms_helper_connector_hotplug_event(struct drm_connector *connector);
void drm_kms_helper_hotplug_event(struct drm_device *dev);
bool intel_tc_port_link_reset(struct intel_digital_port *dig_port);
void intel_dp_phy_test(struct intel_encoder *encoder);
int intel_dp_retrain_link(struct intel_encoder *encoder, struct drm_modeset_acquire_ctx *ctx);
const struct drm_edid *drm_edid_read_ddc(struct drm_connector *connector, struct i2c_adapter *adapter);
void drm_edid_connector_update(struct drm_connector *connector, const struct drm_edid *drm_edid);
bool drm_edid_is_digital(const struct drm_edid *drm_edid);
void drm_edid_free(const struct drm_edid *drm_edid);
void intel_gmbus_force_bit(struct i2c_adapter *adapter, bool force_bit);
bool intel_gmbus_is_forced_bit(struct i2c_adapter *adapter);
void intel_gmbus_reset(struct drm_i915_private *i915);
bool intel_irqs_enabled(struct drm_i915_private *i915);
void intel_hpd_irq_handler(struct drm_i915_private *dev_priv, u32 pin_mask, u32 long_mask);
void icp_irq_handler(struct drm_i915_private *dev_priv, u32 pch_iir);
enum intel_hotplug_state intel_encoder_hotplug(struct intel_encoder *encoder, struct intel_connector *connector);
void intel_hpd_init_early(struct drm_i915_private *i915);
void intel_hpd_cancel_work(struct drm_i915_private *dev_priv);
int drm_helper_probe_detect(struct drm_connector *connector, struct drm_modeset_acquire_ctx *ctx, bool force);
const char *drm_get_connector_status_name(enum connector_status status);

/* The static callbacks of the Linux text, for the objects the owner makes. */
enum intel_hotplug_state (*i915_hpd_ddi_hotplug_fn(void))(struct intel_encoder *, struct intel_connector *);
bool (*i915_hpd_lpt_connected_fn(void))(struct intel_encoder *);
const struct drm_connector_funcs *drv_i915_hpd_hdmi_connector_funcs(void);

/* The world the device of the Linux text belongs to. */
static __inline struct i915_hpd_world *
i915_hpd_world_of(
	struct drm_i915_private *i915)
{
	/* The device is the world's hpd_i915 member. */
	return container_of(i915, struct i915_hpd_world, hpd_i915);
}

/* The device a drm device belongs to (the Linux to_i915() of this environment). */
static __inline struct drm_i915_private *
i915_hpd_to_i915(
	struct drm_device *dev)
{
	/* The drm device is the device's drm member. */
	return container_of(dev, struct drm_i915_private, drm);
}

/* The display version of the device (the Linux DISPLAY_VER() of this environment). */
static __inline int
i915_hpd_display_ver(
	struct drm_i915_private *i915)
{
	int version;

	UNUSED_PARAMETER(i915);

	/* Asks the modeset side for the version of the device the probe found. */
	version = drv_i915_lcd_display_ver();

	/* Succeeded: reports the display version. */
	return version;
}

/* The scheduler tick, which stands for the Linux jiffies. */
static __inline unsigned long
i915_hpd_jiffies(void)
{
	uint64_t ticks;

	/* Samples the scheduler tick. */
	ticks = sched_ticks();

	/* Succeeded: reports the tick as jiffies. */
	return (unsigned long)ticks;
}

/* Converts milliseconds to scheduler ticks, rounding up (the Linux msecs_to_jiffies()). */
static __inline unsigned long
i915_hpd_msecs_to_jiffies(
	unsigned long ms)
{
	/* Reports the ticks that cover the milliseconds. */
	return (unsigned long)((ms * KERN_CLOCK_HZ + 999u) / 1000u);
}

/* Converts scheduler ticks back to milliseconds, rounding down. */
static __inline unsigned
i915_hpd_jiffies_to_ms(
	unsigned long jiffies)
{
	/* Reports the milliseconds the ticks span. */
	return (unsigned)(jiffies * 1000u / KERN_CLOCK_HZ);
}

/*
 * Takes a spinlock with interrupts disabled (the Linux spin_lock_irq()).
 *
 * Linux restores interrupts unconditionally on unlock; here the state they
 * were in is kept in *saved (the old form kept it in one global), which the
 * matching unlock reads.
 */
static __inline void
i915_hpd_spin_lock_irq(
	struct spinlock *lock,
	unsigned long *saved)
{
	unsigned long enabled;

	/* Takes the lock and learns whether interrupts were on. */
	enabled = spin_lock_irqsave(lock);

	/* Keeps the interrupt state for the unlock; the lock now protects it. */
	*saved = enabled;
}

/* Releases a spinlock taken by i915_hpd_spin_lock_irq() (the Linux spin_unlock_irq()). */
static __inline void
i915_hpd_spin_unlock_irq(
	struct spinlock *lock,
	unsigned long *saved)
{
	/* Releases the lock and restores the interrupt state the lock saved. */
	spin_unlock_irqrestore(lock, *saved);
}

/* Reads a display register of the world's device (the Linux intel_de_read()). */
static __inline u32
i915_hpd_intel_de_read(
	struct drm_i915_private *i915,
	i915_reg_t reg)
{
	u32 value;

	/* Reads through the world, which routes to the BAR or to a model's registers. */
	value = drv_i915_hpd_read(i915_hpd_world_of(i915), reg.reg);

	/* Succeeded: reports the register value. */
	return value;
}

/* Reads a display register without forcewake bookkeeping (the Linux intel_de_read_fw()). */
static __inline u32
i915_hpd_intel_de_read_fw(
	struct drm_i915_private *i915,
	i915_reg_t reg)
{
	u32 value;

	/* Reads through the world, like intel_de_read(). */
	value = drv_i915_hpd_read(i915_hpd_world_of(i915), reg.reg);

	/* Succeeded: reports the register value. */
	return value;
}

/* Writes a display register of the world's device (the Linux intel_de_write()). */
static __inline void
i915_hpd_intel_de_write(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 value)
{
	/* Writes through the world, which routes to the BAR or to a model's registers. */
	(void)drv_i915_hpd_write(i915_hpd_world_of(i915), reg.reg, value);
}

/* Writes a display register without forcewake bookkeeping (the Linux intel_de_write_fw()). */
static __inline void
i915_hpd_intel_de_write_fw(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 value)
{
	/* Writes through the world, like intel_de_write(). */
	(void)drv_i915_hpd_write(i915_hpd_world_of(i915), reg.reg, value);
}

/* Drops a power-domain reference (the Linux intel_display_power_put_async(), put at once). */
static __inline void
i915_hpd_intel_display_power_put_async(
	struct drm_i915_private *i915,
	enum intel_display_power_domain domain,
	intel_wakeref_t wf)
{
	/* Puts the reference synchronously; nothing is parked. */
	drv_i915_hpd_intel_display_power_put(i915, domain, wf);
}

/*
 * Prepares a work item on the world's trampoline (the Linux INIT_WORK()).
 *
 * The item is not bound to a queue until it is queued.
 */
static __inline void
i915_hpd_init_work(
	struct work_struct *work,
	void (*function)(struct work_struct *work))
{
	/* Records the Linux callback and forgets any queue. */
	work->func = function;
	work->q = NULL;

	/* Prepares the driver work, whose trampoline runs the callback. */
	drv_i915_work_init(&work->kwork, drv_i915_hpd_work_trampoline, work);
}

/*
 * Prepares a delayed work item on the world's trampoline (the Linux
 * INIT_DELAYED_WORK()).
 */
static __inline void
i915_hpd_init_delayed_work(
	struct delayed_work *delayed,
	void (*function)(struct work_struct *work))
{
	/* Records the Linux callback and forgets any queue. */
	delayed->work.func = function;
	delayed->work.q = NULL;

	/* Prepares the delayed driver work, whose trampoline runs the callback. */
	drv_i915_delayed_work_init(&delayed->dw, drv_i915_hpd_work_trampoline, &delayed->work);
}

/*
 * Queues a work item (the Linux queue_work()).
 *
 * Reports true when the item was newly queued, false when it was pending.
 */
static __inline bool
i915_hpd_queue_work(
	struct workqueue_struct *queue,
	struct work_struct *work)
{
	int queued;

	/* Binds the item to the queue a synchronous cancel will use. */
	work->q = queue;

	/* Queues the driver work. */
	queued = drv_i915_queue_work(&queue->wq, &work->kwork);
	if (queued != 1)
		return false;

	/* Succeeded: the item was newly queued. */
	return true;
}

/*
 * Arms a delayed work item (the Linux queue_delayed_work()).
 *
 * Reports true when the item was newly armed, false when it was armed or
 * pending already.
 */
static __inline bool
i915_hpd_queue_delayed_work(
	struct workqueue_struct *queue,
	struct delayed_work *delayed,
	unsigned long delay)
{
	int armed;

	/* Binds the item to the queue a synchronous cancel will use. */
	delayed->work.q = queue;

	/* Arms the delayed driver work for the delay in milliseconds. */
	armed = drv_i915_delayed_queue(&queue->tq, &delayed->dw, i915_hpd_jiffies_to_ms(delay));
	if (armed != 1)
		return false;

	/* Succeeded: the item was newly armed. */
	return true;
}

/*
 * Re-arms a delayed work item with a new delay (the Linux mod_delayed_work()).
 *
 * A pending arming is disarmed first, so the new delay replaces it.
 */
static __inline bool
i915_hpd_mod_delayed_work(
	struct workqueue_struct *queue,
	struct delayed_work *delayed,
	unsigned long delay)
{
	int armed;

	/* Binds the item to the queue a synchronous cancel will use. */
	delayed->work.q = queue;

	/* Disarms a pending arming; a running callback is not waited for. */
	(void)drv_i915_delayed_cancel(&queue->tq, &delayed->dw);

	/* Arms the delayed driver work for the new delay. */
	armed = drv_i915_delayed_queue(&queue->tq, &delayed->dw, i915_hpd_jiffies_to_ms(delay));
	if (armed != 1)
		return false;

	/* Succeeded: the item was newly armed. */
	return true;
}

/*
 * Cancels a work item and waits for its callback (the Linux
 * cancel_work_sync()).
 *
 * Reports true when a pending item was cancelled; an item never queued is
 * not pending.
 */
static __inline bool
i915_hpd_cancel_work_sync(
	struct work_struct *work)
{
	uint64_t deadline;
	int cancelled;

	/* An item never queued has nothing to cancel. */
	if (work->q == NULL)
		return false;

	/* Gives the running callback a bounded time to finish. */
	deadline = drv_i915_hpd_sync_deadline();

	/* Cancels on the queue the item was last queued on. */
	cancelled = drv_i915_cancel_work_sync(&work->q->wq, &work->kwork, deadline);
	if (cancelled != 1)
		return false;

	/* Succeeded: a pending item was cancelled. */
	return true;
}

/*
 * Cancels a delayed work item and waits for its callback (the Linux
 * cancel_delayed_work_sync()).
 */
static __inline bool
i915_hpd_cancel_delayed_work_sync(
	struct delayed_work *delayed)
{
	uint64_t deadline;
	int cancelled;

	/* An item never armed has nothing to cancel. */
	if (delayed->work.q == NULL)
		return false;

	/* Gives the running callback a bounded time to finish. */
	deadline = drv_i915_hpd_sync_deadline();

	/* Cancels on the timer queue the item was last armed on. */
	cancelled = drv_i915_delayed_cancel_sync(&delayed->work.q->tq, &delayed->dw, deadline);
	if (cancelled != 1)
		return false;

	/* Succeeded: an armed or pending item was cancelled. */
	return true;
}

/* Starts a connector walk (the Linux drm_connector_list_iter_begin()). */
static __inline void
i915_hpd_drm_connector_list_iter_begin(
	struct drm_device *dev,
	struct drm_connector_list_iter *iter)
{
	UNUSED_PARAMETER(dev);

	/* The walk starts at the world's first connector. */
	iter->idx = 0u;
}

/* Ends a connector walk (the Linux drm_connector_list_iter_end()); nothing is held. */
static __inline void
i915_hpd_drm_connector_list_iter_end(
	struct drm_connector_list_iter *iter)
{
	UNUSED_PARAMETER(iter);
}

/*
 * Walks the world's encoders (the Linux for_each_intel_encoder()).
 *
 * The Linux form found them through the drm device; this one takes the world
 * and an unsigned index variable the caller declares.
 */
#define I915_HPD_FOR_EACH_INTEL_ENCODER(world, index, encoder) \
	for ((index) = 0u; \
	     ((encoder) = drv_i915_hpd_encoder_at((world), (index))) != NULL; \
	     (index)++)

/*
 * Walks the world's connectors from where the iterator stands (the Linux
 * for_each_intel_connector_iter()).
 */
#define I915_HPD_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, iter) \
	while (((connector) = drv_i915_hpd_connector_next((world), (iter))) != NULL)

/* Takes a connector reference (the Linux drm_connector_get()); connectors live as long as the world. */
static __inline void
i915_hpd_drm_connector_get(
	struct drm_connector *connector)
{
	UNUSED_PARAMETER(connector);
}

/* Drops a connector reference (the Linux drm_connector_put()). */
static __inline void
i915_hpd_drm_connector_put(
	struct drm_connector *connector)
{
	UNUSED_PARAMETER(connector);
}

/* Takes a modeset lock (the Linux drm_modeset_lock()); nothing to take, never -EDEADLK. */
static __inline int
i915_hpd_drm_modeset_lock(
	struct drm_modeset_lock *lock,
	struct drm_modeset_acquire_ctx *ctx)
{
	UNUSED_PARAMETER(lock);
	UNUSED_PARAMETER(ctx);

	/* Succeeded: the lock counts as held. */
	return 0;
}

/* The i915 connector of a drm connector (the Linux to_intel_connector()). */
static __inline struct intel_connector *
i915_hpd_to_intel_connector(
	struct drm_connector *connector)
{
	/* The drm connector is the base of the i915 connector. */
	return container_of(connector, struct intel_connector, base);
}

/* The digital port of an encoder (the Linux enc_to_dig_port()). */
static __inline struct intel_digital_port *
i915_hpd_enc_to_dig_port(
	struct intel_encoder *encoder)
{
	/* The encoder is the base of the digital port. */
	return container_of(encoder, struct intel_digital_port, base);
}

/* The encoder a connector is attached to (the Linux intel_attached_encoder()). */
static __inline struct intel_encoder *
i915_hpd_intel_attached_encoder(
	struct intel_connector *connector)
{
	/* Reports the encoder the start attached. */
	return connector->encoder;
}

/* Tells whether an encoder is a digital port (the Linux intel_encoder_is_dig_port()). */
static __inline bool
i915_intel_encoder_is_dig_port(
	struct intel_encoder *encoder)
{
	/* DDI, DP, eDP and HDMI encoders sit on a digital port. */
	switch (encoder->type) {
	case INTEL_OUTPUT_DDI:
	case INTEL_OUTPUT_DP:
	case INTEL_OUTPUT_EDP:
	case INTEL_OUTPUT_HDMI:
		return true;
	default:
		return false;
	}
}

/* Gates the GMBUS clock on Broxton-class parts (the Linux bxt_gmbus_clock_gating()); not this platform. */
static __inline void
i915_bxt_gmbus_clock_gating(
	struct drm_i915_private *i915,
	bool enable)
{
	UNUSED_PARAMETER(i915);
	UNUSED_PARAMETER(enable);
}

/* Gates the GMBUS clock on SPT/CNP PCHs (the Linux pch_gmbus_clock_gating()); not this platform. */
static __inline void
i915_pch_gmbus_clock_gating(
	struct drm_i915_private *i915,
	bool enable)
{
	UNUSED_PARAMETER(i915);
	UNUSED_PARAMETER(enable);
}

#endif
