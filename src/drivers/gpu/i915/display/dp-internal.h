/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Linux environment of the DP AUX, panel power sequencer, DP helper and
 * EDID text.
 *
 * The eDP bring-up (dp-sink.c, aux.c, panel.c and edid-read.c: the files
 * ported from the Linux intel_dp_aux.c, intel_pps.c, drm_dp_helper.c and
 * drm_edid.c) is compiled here.  This header
 * supplies what that text expects from the Linux kernel: register access,
 * waits, sleeps, a clock, power-domain references, the PPS and AUX mutexes,
 * delayed work, I2C, and the few objects the kept functions touch
 * (struct intel_dp, intel_digital_port, intel_connector and the device),
 * fixed to the one platform the driver drives (ADL-P, display version 13, PCH
 * ADP).  Everything that reaches hardware or time goes through the
 * world-neutral struct i915_dp_env of internal.h, so the same functions run
 * against the real GPU and against a register model.
 *
 * It is layered on the VBT environment (vbt.h), which it includes: the DP
 * text shares the VBT's types, platform predicates and arena allocator, and
 * replaces the world name with "dp".  Two meanings differ from the VBT
 * environment, and both have DP names here:
 *
 *   the messages      every DP message macro counts into the DP's own
 *                     counters (drv_i915_dp_note()), not the parser's;
 *   the device        struct drm_i915_private carries the PPS lock, the DP
 *                     environment, the raw clock and the work queue after
 *                     the members the VBT parser knows.
 *
 * Linux helper names whose meaning differed between the old environments are
 * not defined under their Linux name; each has an explicit name below
 * (i915_dp_intel_de_read() for intel_de_read(), I915_DP_MUTEX_LOCK() for
 * mutex_lock() and so on).  The DP text uses the VBT names for the helpers
 * whose meaning the two environments share (i915_vbt_display_ver(),
 * I915_VBT_CLAMP(), i915_vbt_intel_port_to_phy() and the others).
 *
 * Errors: the Linux text returns Linux-numbered negative errno values, and
 * those travel to the caller of the bridge unchanged in struct
 * i915_edp_result (internal.h names them I915_EDP_E*).  I915_DP_E* below are
 * the same numbers under the DP environment's own names, for the Linux text;
 * the zedBSD functions of dp-sink.c return zedBSD positive errno values at their
 * other boundaries.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DP_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_DP_INTERNAL_H

#include "internal.h"
#include <kern/kcrt.h>

#ifdef I915_DISPLAY_LINUX_WORLD
#error "display/dp-internal.h: another Linux environment is already included"
#endif

#include "vbt.h"

#ifndef I915_DISPLAY_WORLD_VBT
#error "display/dp-internal.h: the VBT environment did not define itself"
#endif

/* The Linux environment this translation unit is compiled in: DP replaces VBT. */
#undef I915_DISPLAY_LINUX_WORLD
#define I915_DISPLAY_LINUX_WORLD "dp"

/* Marks the DP environment. */
#define I915_DISPLAY_WORLD_DP 1

/*
 * The Linux errno numbers the DP text returns.
 *
 * They are the bridge's I915_EDP_E* numbers: the Linux text's negative
 * results reach the caller through struct i915_edp_result as they are.
 */
#define I915_DP_EIO I915_EDP_EIO
#define I915_DP_ENXIO I915_EDP_ENXIO
#define I915_DP_E2BIG I915_EDP_E2BIG
#define I915_DP_EBUSY I915_EDP_EBUSY
#define I915_DP_EINVAL I915_EDP_EINVAL
#define I915_DP_EPROTO I915_EDP_EPROTO
#define I915_DP_ETIMEDOUT I915_EDP_ETIMEDOUT
#define I915_DP_EREMOTEIO I915_EDP_EREMOTEIO

/*
 * The Linux language helpers the DP text uses.
 */

/* The size type the AUX transfer hooks return (the Linux ssize_t). */
typedef long i915_dp_ssize_t;

/* The Linux shorthand for unsigned int. */
typedef unsigned int uint;

/* A 16-bit little-endian value as a sink reports it. */
typedef u16 __le16;

/* Converts a little-endian 16-bit value to host order (the host is little endian). */
#define le16_to_cpu(x) ((u16)(x))

/* The Linux cache placement annotation; it means nothing here. */
#define __read_mostly

/* The Linux module export; the DP text is not a module. */
#define EXPORT_SYMBOL(x)

/*
 * Limits a value to [lo, hi] as type t (the Linux clamp_t()).
 *
 * Every argument is evaluated once, through min_t() and max_t() of
 * internal.h.
 */
#define I915_DP_CLAMP_T(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)

/* Rounds x up to a multiple of y. */
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))

/* Reads a variable and leaves zero in its place. */
#define fetch_and_zero(ptr) ({ __typeof__(*(ptr)) _v = *(ptr); *(ptr) = 0; _v; })

/*
 * Reports one DP message.
 *
 * It has the form of the parser's I915_VBT_LOG(): the format text only, the
 * arguments type-checked and never evaluated.  The message is counted in the
 * DP's own counters and shown with the DP's prefix by drv_i915_dp_note().
 */
#define I915_DP_LOG(level, fmt, ...) \
	do { \
		if (0) \
			(void)drv_i915_vbt_fmtcheck(fmt, ##__VA_ARGS__); \
		drv_i915_dp_note(level, fmt); \
	} while (0)

/*
 * The Linux message macros in the DP's meaning.
 *
 * The device argument is never evaluated.  The WARN forms evaluate their
 * condition once and report its truth value, as in Linux.
 */
#define I915_DP_DRM_DBG_KMS(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_DBG(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_INFO(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_INFO, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_NOTICE(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_INFO, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_ERR(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_ERR, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_WARN(drm, cond, fmt, ...) \
	({ int _w = !!(cond); if (_w) I915_DP_LOG(I915_VBT_LOG_ERR, "WARN: " fmt, ##__VA_ARGS__); _w; })
#define I915_DP_DRM_WARN_ON(drm, cond) \
	({ int _w = !!(cond); if (_w) I915_DP_LOG(I915_VBT_LOG_ERR, "WARN_ON(%s)\n", #cond); _w; })
#define I915_DP_WARN_ON(cond) I915_DP_DRM_WARN_ON(0, cond)

/* The Linux drm_warn(): an error-level message without a condition. */
#define I915_DP_DRM_WARN_MSG(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_ERR, fmt, ##__VA_ARGS__)

#define I915_DP_DRM_DEBUG_KMS(fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_DP_DRM_DEBUG_DRIVER(fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_DP_MISSING_CASE(x) I915_DP_LOG(I915_VBT_LOG_ERR, "Missing case (%s == %ld)\n", #x, (long)(x))

/* The DP-only message macros, which only this environment ever defined. */
#define drm_dbg_dp(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define drm_dbg_kms_ratelimited(drm, fmt, ...) I915_DP_LOG(I915_VBT_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define I915_STATE_WARN(i915, cond, fmt, ...) I915_DP_DRM_WARN(0, cond, fmt, ##__VA_ARGS__)

/*
 * Reports a PPS or AUX mutex that is not held where the Linux text requires
 * it (the Linux lockdep_assert_held()).
 *
 * The mutex expression is named in the message by its source text.
 */
#define I915_DP_LOCKDEP_ASSERT_HELD(m) \
	do { if (!(m)->held) I915_DP_LOG(I915_VBT_LOG_ERR, "lockdep: %s not held\n", #m); } while (0)

/*
 * Register helpers the DP register definitions use in addition to those of
 * internal.h.
 */

/* Picks the register of a port from the registers of ports A and B. */
#define _PORT(port, a, b) _PICK_EVEN(port, a, b)

/* The VLV/CHV display MMIO base (Linux i915_reg_defs.h); the VLV branches are never taken. */
#define VLV_DISPLAY_BASE 0x180000

/* The platform predicates only the DP text uses; the device is never evaluated. */
#define IS_IRONLAKE(i915) 0
#define HAS_PCH_IBX(i915) 0
#define HAS_PCH_CPT(i915) 0
#define HAS_DSC(i915) 1

/*
 * The south display registers the PPS unlock and clock-gating workarounds
 * write.
 *
 * From Linux drivers/gpu/drm/i915/i915_reg.h.
 */
#define SOUTH_CHICKEN1 _MMIO(0xc2000)
#define  ICP_SECOND_PPS_IO_SELECT REG_BIT(2)
#define SOUTH_DSPCLK_GATE_D _MMIO(0xc2020)
#define  PCH_DPLSUNIT_CLOCK_GATE_DISABLE (1 << 29)
#define  PCH_DPMGUNIT_CLOCK_GATE_DISABLE (1 << 15)

/*
 * A second-level wait for a register, with its fast and slow timeouts (the
 * Linux __intel_de_wait_for_register()).
 *
 * It returns 0, or the negative Linux errno the environment's wait reports
 * (-I915_DP_ETIMEDOUT), and leaves the last value read in *out when out is
 * not NULL.
 */
#define __intel_de_wait_for_register(i915, r, mask, value, fast_us, slow_ms, out) \
	i915_dp_env_of(i915)->wait_reg(i915_dp_env_of(i915)->ctx, (r).reg, mask, value, fast_us, slow_ms, out)

/* A register read the Linux text does not trace; nothing is traced here anyway. */
#define intel_de_read_notrace(i915, r) i915_dp_intel_de_read(i915, r)

/* The Linux register trace point; registers are not traced here. */
#define trace_i915_reg_rw(...) ((void)0)

/*
 * The time helpers the DP text uses (the contract of the Linux ktime and
 * jiffies, with HZ == 1000: one jiffy is one millisecond).
 */

/* The boot time now, in milliseconds. */
#define ktime_get_boottime() ((ktime_t)drv_i915_dp_now_ms())

/* The milliseconds between two ktime values. */
#define ktime_ms_delta(later, earlier) ((s64)(later) - (s64)(earlier))

/*
 * Mutex operations on the PPS and AUX mutexes (the Linux mutex_lock() and
 * mutex_unlock()).
 *
 * They are macros because the mutex expression is named, by its source text,
 * in the message a misuse reports.
 */
#define I915_DP_MUTEX_LOCK(m) drv_i915_dp_mutex_lock((m), #m)
#define I915_DP_MUTEX_UNLOCK(m) drv_i915_dp_mutex_unlock((m), #m)

/* The delayed work that holds a work item. */
#define to_delayed_work(w) container_of(w, struct delayed_work, work)

/* Cancels a delayed work without waiting for a running body. */
#define cancel_delayed_work(dw) drv_i915_dp_delayed_cancel((dw), 0)

/* The CPU latency request: no CPU idle states are entered while the attaching thread polls. */
#define cpu_latency_qos_update_request(req, v) ((void)(req))
#define PM_QOS_DEFAULT_VALUE (-1)

/*
 * The I2C message flags and adapter functionality bits (Linux
 * include/uapi/linux/i2c.h).
 */
#define I2C_M_RD 0x0001
#define I2C_M_STOP 0x8000
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_SMBUS_EMUL 0x0eff0008
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000
#define I2C_FUNC_10BIT_ADDR 0x00000002

/* MST remote AUX does not exist here: a remote DPCD access is refused. */
#define drm_dp_mst_dpcd_read(aux, offset, buffer, size) (-I915_DP_EINVAL)
#define drm_dp_mst_dpcd_write(aux, offset, buffer, size) (-I915_DP_EINVAL)

/* Type-C: the eDP port is a combo PHY, so the Type-C branches of the AUX transfer are never taken. */
#define intel_tc_port_lock(d) ((void)(d))
#define intel_tc_port_unlock(d) ((void)(d))
#define intel_tc_port_connected_locked(enc) (true)

/*
 * Quirks: QUIRK_FW_SYNC_LEN (one MTL laptop and panel) and
 * QUIRK_INCREASE_T12_DELAY (one Toshiba) do not apply.
 */
#define QUIRK_FW_SYNC_LEN 0
#define QUIRK_INCREASE_T12_DELAY 0
#define intel_has_dpcd_quirk(dp, q) (false)

/* VLV/CHV power-sequencer stealing is not ported; IS_VALLEYVIEW() and IS_CHERRYVIEW() are 0. */
#define vlv_power_sequencer_pipe(dp) (0)
#define vlv_initial_power_sequencer_setup(dp) ((void)0)

/*
 * The power domains the DP text takes references on.
 *
 * The values are those of enum i915_power_domain in internal.h, so a domain
 * passes through the environment's power hooks unchanged.
 */
enum intel_display_power_domain {
	POWER_DOMAIN_DISPLAY_CORE = 0,
	POWER_DOMAIN_AUX_A = 53,
	POWER_DOMAIN_AUX_B,
	POWER_DOMAIN_AUX_C,
};

/*
 * The pipes a PPS can be locked to on VLV/CHV.
 *
 * Only struct intel_pps names them; the VLV branches are never taken.
 */
enum pipe {
	INVALID_PIPE = -1,
	PIPE_A = 0,
	PIPE_B,
	PIPE_C,
	PIPE_D,
};

/*
 * A power-domain reference as the Linux text holds it.
 *
 * 1 is a reference taken, 0 nothing held, -1 a get that failed.
 */
typedef int intel_wakeref_t;

/* A point in time, in milliseconds since boot. */
typedef s64 ktime_t;

/*
 * One of the environment's two locks: the PPS mutex or the AUX hardware
 * mutex (I915_DP_LOCK_*).
 *
 * The lock itself is the environment's; this records whether it is held, so
 * the Linux lock assertions and a misuse report have something to test.  The
 * instance lives in the device (the PPS mutex) or in the AUX channel (the AUX
 * mutex) and is bound to the environment when the eDP begins.
 */
struct i915_dp_mutex {
	/* Set while the lock is held. */
	int held;

	/* How many times the lock was taken, for the run log. */
	unsigned acquisitions;

	/* The environment whose lock this is, or NULL before binding. */
	struct i915_dp_env *env;

	/* Which lock of the environment: I915_DP_LOCK_PPS or I915_DP_LOCK_AUX. */
	int id;
};

/*
 * The work item of a delayed work.
 *
 * It carries nothing: the body is found through the delayed work that
 * contains it.
 */
struct work_struct {
	/* A placeholder: an empty structure is not ANSI C. */
	int unused;
};

/*
 * A delayed work of the DP text: the PPS's delayed VDD-off.
 *
 * The environment's backend (a timer and a worker thread on the real GPU, the
 * model's clock in the GPU-free tests) runs the body through the bridge's work
 * entry point; the instance lives in struct intel_pps.
 */
struct delayed_work {
	/* The work item handed to the body. */
	struct work_struct work;

	/* The body, set by i915_dp_init_delayed_work(). */
	void (*fn)(struct work_struct *);

	/* Which delayed work of the environment: I915_DP_WORK_*. */
	int slot;
};

/*
 * The CPU latency request the AUX transfer updates.
 *
 * It carries nothing: CPU idle states are not entered here.
 */
struct pm_qos_request {
	/* A placeholder: an empty structure is not ANSI C. */
	int unused;
};

/*
 * One I2C message of a transfer (the Linux struct i2c_msg).
 *
 * The caller owns the buffer for the duration of the transfer.
 */
struct i2c_msg {
	/* The 7-bit slave address. */
	u16 addr;

	/* I2C_M_* flags. */
	u16 flags;

	/* The number of bytes to transfer. */
	u16 len;

	/* The bytes to write or the room to read into. */
	u8 *buf;
};

struct i2c_adapter;

/*
 * How an I2C adapter runs a transfer (the Linux struct i2c_algorithm).
 *
 * The DP helper supplies the one that runs I2C over AUX.
 */
struct i2c_algorithm {
	/* Runs num messages; returns how many were done or a negative errno. */
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

	/* Reports the I2C_FUNC_* bits the adapter supports. */
	u32 (*functionality)(struct i2c_adapter *adap);
};

/*
 * An I2C bus (the Linux struct i2c_adapter).
 *
 * The one here is the DDC channel of an AUX channel; it lives in struct
 * drm_dp_aux.
 */
struct i2c_adapter {
	/* The transfer algorithm. */
	const struct i2c_algorithm *algo;

	/* The algorithm's own data. */
	void *algo_data;

	/* How many times a transfer is retried. */
	int retries;

	/* The adapter's name. */
	char name[48];
};

/*
 * One AUX transaction (the Linux struct drm_dp_aux_msg).
 *
 * The caller owns the buffer for the duration of the transfer.
 */
struct drm_dp_aux_msg {
	/* The DPCD address or the I2C slave address. */
	unsigned int address;

	/* The DP_AUX_* request code. */
	u8 request;

	/* The DP_AUX_*_REPLY_* code the sink answered with. */
	u8 reply;

	/* The bytes to write or the room to read into; NULL for a bare address. */
	void *buffer;

	/* The number of bytes. */
	size_t size;
};

/*
 * One DP AUX channel (the Linux struct drm_dp_aux): the fields the kept
 * functions use.
 *
 * It lives in struct intel_dp; its hardware mutex is the environment's AUX
 * lock.
 */
struct drm_dp_aux {
	/* The channel's name. */
	const char *name;

	/* The I2C-over-AUX adapter the EDID is read through. */
	struct i2c_adapter ddc;

	/* The DRM device the channel belongs to. */
	struct drm_device *drm_dev;

	/* Serializes transfers on the channel. */
	struct i915_dp_mutex hw_mutex;

	/* Runs one transaction; returns the bytes transferred or a negative errno. */
	i915_dp_ssize_t (*transfer)(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg);

	/* I2C NACKs and DEFERs seen, for the result. */
	unsigned i2c_nack_count;
	unsigned i2c_defer_count;

	/* Set on an MST remote channel; never set here. */
	bool is_remote;

	/* Set while the channel is powered down. */
	bool powered_down;
};

/*
 * The quoted DisplayPort definitions: the Linux drm_dp.h DPCD addresses and
 * AUX request and reply codes, the PPS and AUX registers, and, because this is
 * the DP environment, struct intel_pps (the panel power sequencer state of one
 * DP) and the PPS and AUX entry points with with_intel_pps_lock.
 */
#include "../intel/dp.h"

/*
 * A DRM mode object (the Linux struct drm_mode_object).
 *
 * Only the id is kept; it lives in the encoder.
 */
struct drm_mode_object {
	/* The object id. */
	int id;
};

/*
 * A DRM encoder (the Linux struct drm_encoder): the fields the messages and
 * to_i915() use.
 *
 * It lives in struct intel_encoder.
 */
struct drm_encoder {
	/* The mode object. */
	struct drm_mode_object base;

	/* The encoder's name. */
	const char *name;

	/* The DRM device; &i915->drm of the eDP's device. */
	struct drm_device *dev;
};

/*
 * An i915 encoder (the Linux struct intel_encoder): the DRM encoder and its
 * port.
 *
 * It lives in struct intel_digital_port.
 */
struct intel_encoder {
	/* The DRM encoder. */
	struct drm_encoder base;

	/* The DDI port the encoder drives. */
	enum port port;
};

/*
 * An i915 connector (the Linux struct intel_connector): its panel.
 *
 * The eDP owns one for its whole life.
 */
struct intel_connector {
	/* The panel, with what the VBT says about it. */
	struct intel_panel panel;
};

/*
 * An i915 DP output (the Linux struct intel_dp): the members the kept
 * functions read.
 *
 * It lives in struct intel_digital_port.
 */
struct intel_dp {
	/* The port's DP control register. */
	i915_reg_t output_reg;

	/* The cached value of that register. */
	u32 DP;

	/* Set when the output is an eDP panel. */
	bool is_edp;

	/* The connector the output drives. */
	struct intel_connector *attached_connector;

	/* The sink's receiver capabilities. */
	u8 dpcd[DP_RECEIVER_CAP_SIZE];

	/* The sink's eDP display control capabilities. */
	u8 edp_dpcd[EDP_DISPLAY_CTL_CAP_SIZE];

	/* The AUX channel. */
	struct drm_dp_aux aux;

	/* The status of the last transfer that found the channel busy. */
	u32 aux_busy_last_status;

	/* The CPU latency request held during a transfer. */
	struct pm_qos_request pm_qos;

	/* The panel power sequencer state. */
	struct intel_pps pps;

	/* The platform's AUX clock divider for a given try. */
	u32 (*get_aux_clock_divider)(struct intel_dp *dp, int index);

	/* The platform's AUX send control word. */
	u32 (*get_aux_send_ctl)(struct intel_dp *dp, int send_bytes, u32 aux_clock_divider);

	/* The platform's AUX control and data registers. */
	i915_reg_t (*aux_ch_ctl_reg)(struct intel_dp *dp);
	i915_reg_t (*aux_ch_data_reg)(struct intel_dp *dp, int index);
};

/*
 * An i915 digital port (the Linux struct intel_digital_port): the encoder,
 * the DP output, the AUX channel and the device.
 *
 * The eDP owns one for its whole life (struct i915_dp_edp).
 */
struct intel_digital_port {
	/* The encoder. */
	struct intel_encoder base;

	/* The DP output. */
	struct intel_dp dp;

	/* The AUX channel the VBT names for the port. */
	enum aux_ch aux_ch;

	/* The device the port belongs to. */
	struct drm_i915_private *i915;
};

/*
 * The runtime display information the DP text reads (the Linux
 * DISPLAY_RUNTIME_INFO()): the raw clock.
 */
struct i915_dp_display_runtime {
	/* The raw clock, in kHz; the AUX clock divider is derived from it. */
	u32 rawclk_freq;
};

/*
 * The device as the DP text sees it.
 *
 * The first members are the VBT parser's layout (vbt.c); the DP members
 * follow at the positions the old DP translation units had them.  The eDP owns
 * one instance for its whole life (struct i915_dp_edp); it is filled when the
 * eDP begins.
 */
struct drm_i915_private {
	/* The DRM device; encoders point at it and to_i915() comes back from it. */
	struct drm_device drm;

	/* The display part of the device. */
	struct {
		/* The parsed VBT; the DP text does not read it. */
		struct intel_vbt_data vbt;

		/* The i915.edp_vswing module parameter: 0 = use the VBT. */
		struct {
			int edp_vswing;
		} params;

		/* The panel power sequencer registers and their lock. */
		struct {
			/* The PPS mutex (I915_DP_LOCK_PPS). */
			struct i915_dp_mutex mutex;

			/* The base the PP_* registers are addressed from (PCH_PPS_BASE). */
			unsigned mmio_base;
		} pps;
	} display;

	/* The environment every register access, wait, sleep and lock goes through. */
	struct i915_dp_env *dp_env;

	/* The raw clock (DISPLAY_RUNTIME_INFO()). */
	struct i915_dp_display_runtime display_runtime;

	/* The work queue the delayed VDD-off is queued on; the backend ignores it. */
	void *unordered_wq;
};

/*
 * The one live eDP: its device, port, connector and start time.
 *
 * The bridge (dp-sink.c) fills it when the eDP begins and clears live when it
 * ends; the Linux text reaches it through the device and the port pointers it
 * is handed.
 */
struct i915_dp_edp {
	/* Set between the start and the end of the eDP. */
	int live;

	/* The environment the eDP runs in; NULL when not live. */
	struct i915_dp_env *env;

	/* The device the Linux text works on. */
	struct drm_i915_private i915;

	/* The port, with its DP output and AUX channel. */
	struct intel_digital_port dig_port;

	/* The connector, with the panel's VBT data. */
	struct intel_connector connector;

	/* When the eDP began, in the environment's milliseconds. */
	u64 t0_ms;
};

/*
 * The DP environment's world state.
 *
 * It holds what the old DP translation units kept in file-scope variables.
 * The display root holds a pointer to it; dp-sink.c allocates it.  Everything in
 * it is used by the thread that begins, drives and ends the eDP, and by the
 * environment's worker when it runs the delayed VDD-off body; the old code
 * protected none of it by a lock of its own (the PPS mutex serializes the
 * Linux text's use of the port).
 */
struct i915_dp_world {
	/*
	 * The one live eDP.
	 *
	 * Cleared as a whole and filled when the eDP begins; live is cleared, and
	 * env dropped, when it ends.  A zero live means no eDP: every entry point
	 * other than the begin refuses to run.
	 */
	struct i915_dp_edp edp;

	/*
	 * The most verbose message level shown (I915_VBT_LOG_*).
	 *
	 * Set from the configuration when the eDP begins; read by every message.
	 */
	int dp_log_level;

	/*
	 * The error-level messages the DP text reported since the eDP began.
	 *
	 * Zeroed when the eDP begins, counted by every error message, copied into
	 * the result by each snapshot.
	 */
	unsigned dp_log_errors;
};

/*
 * The DP's message counters (dp-sink.c).
 *
 * drv_i915_dp_log_enabled() counts an error-level message and tells whether the
 * level is shown; drv_i915_dp_note() counts and shows one message's format text.
 */
int drv_i915_dp_log_enabled(int level);
void drv_i915_dp_note(int level, const char *fmt);

/* The environment of the live eDP, or NULL when none is live (dp-sink.c). */
struct i915_dp_env *drv_i915_dp_env_current(void);

/*
 * Time through the live eDP's environment (dp-sink.c).
 *
 * Both do nothing (report 0) when no eDP is live.
 */
void drv_i915_dp_sleep_us(unsigned us);
u64 drv_i915_dp_now_ms(void);

/*
 * Power-domain references through the environment (dp-sink.c).
 *
 * A get returns 1, or -1 when the environment refused (the transfer that
 * follows then times out and says so); a put of 0 or -1 does nothing.
 */
intel_wakeref_t drv_i915_dp_power_get(struct drm_i915_private *i915, int domain);
void drv_i915_dp_power_put(struct drm_i915_private *i915, int domain, intel_wakeref_t wakeref);
void drv_i915_dp_power_put_async(struct drm_i915_private *i915, int domain, intel_wakeref_t wakeref);

/*
 * The PPS and AUX mutexes (dp-sink.c).
 *
 * The order is the Linux text's own: intel_pps_lock() takes the DISPLAY_CORE
 * power reference, then the PPS mutex; power references taken inside (VDD)
 * nest under it; the AUX hardware mutex of drm_dp_dpcd_access() is outermost
 * around a transfer.
 */
void drv_i915_dp_mutex_lock(struct i915_dp_mutex *m, const char *name);
void drv_i915_dp_mutex_unlock(struct i915_dp_mutex *m, const char *name);

/*
 * The delayed VDD-off through the environment's backend (dp-sink.c).
 *
 * A queue reports true when the work was newly queued, a cancel when it was
 * pending; a cancel with sync set also waits for a running body.
 */
bool drv_i915_dp_delayed_cancel(struct delayed_work *dw, int sync);
bool drv_i915_dp_delayed_queue(struct delayed_work *dw, unsigned long delay_ms);

/*
 * Copies n bytes, doing nothing when n is 0.
 *
 * ISO C leaves memcpy(NULL, src, 0) undefined, and the Linux AUX transfer
 * does exactly that for a bare-address read (intel_dp_aux_transfer(): the
 * read reply copy into msg->buffer, which is NULL when msg->size is 0).
 */
static __inline void *
i915_dp_memcpy(
	void *dst,
	const void *src,
	size_t n)
{
	/* A zero-length copy touches neither pointer. */
	if (n == 0)
		return dst;

	/* Copies the bytes. */
	kern_memcpy(dst, src, n);

	/* Succeeded: reports the destination, as memcpy() does. */
	return dst;
}

/* The environment of a device. */
static __inline struct i915_dp_env *
i915_dp_env_of(
	struct drm_i915_private *i915)
{
	/* Reports the environment the eDP bound to the device. */
	return i915->dp_env;
}

/* Reads a display register through the environment (the Linux intel_de_read()). */
static __inline u32
i915_dp_intel_de_read(
	struct drm_i915_private *i915,
	i915_reg_t reg)
{
	struct i915_dp_env *env;
	u32 value;

	/* Reads the register through the device's environment. */
	env = i915_dp_env_of(i915);
	value = env->read32(env->ctx, reg.reg);

	/* Succeeded: reports the register value. */
	return value;
}

/* Writes a display register through the environment (the Linux intel_de_write()). */
static __inline void
i915_dp_intel_de_write(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 value)
{
	struct i915_dp_env *env;

	/* Writes the register through the device's environment. */
	env = i915_dp_env_of(i915);
	env->write32(env->ctx, reg.reg, value);
}

/* Reads a register back to post the writes before it (the Linux intel_de_posting_read()). */
static __inline void
i915_dp_intel_de_posting_read(
	struct drm_i915_private *i915,
	i915_reg_t reg)
{
	/* The value is not needed; the read itself posts the writes. */
	(void)i915_dp_intel_de_read(i915, reg);
}

/*
 * Clears and sets bits of a display register (the Linux intel_de_rmw()).
 *
 * As in intel_uncore_rmw(), the register is written only when the value
 * changes.  The value read is reported.
 */
static __inline u32
i915_dp_intel_de_rmw(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clear,
	u32 set)
{
	u32 old;
	u32 value;

	/* Reads the current value and computes the new one. */
	old = i915_dp_intel_de_read(i915, reg);
	value = (old & ~clear) | set;

	/* Writes the new value only when it differs. */
	if (value != old)
		i915_dp_intel_de_write(i915, reg, value);

	/* Succeeded: reports the value before the change. */
	return old;
}

/*
 * Waits until the masked register equals a value (the Linux
 * intel_de_wait_for_register()).
 *
 * The fast phase spins for 2 us, the slow phase sleeps up to timeout_ms.  It
 * returns 0, or the negative Linux errno of the environment's wait
 * (-I915_DP_ETIMEDOUT).
 */
static __inline int
i915_dp_intel_de_wait_for_register(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 mask,
	u32 value,
	unsigned int timeout_ms)
{
	struct i915_dp_env *env;
	int waited;

	/* Waits through the device's environment. */
	env = i915_dp_env_of(i915);
	waited = env->wait_reg(env->ctx, reg.reg, mask, value, 2, timeout_ms, (u32 *)0);

	/* Reports a wait that ran out. */
	if (waited != 0)
		return waited;

	/* Succeeded: the register reached the value. */
	return 0;
}

/* The runtime display information of a device (the Linux DISPLAY_RUNTIME_INFO()). */
static __inline struct i915_dp_display_runtime *
i915_dp_display_runtime_info(
	struct drm_i915_private *i915)
{
	/* Reports the raw clock the eDP stored when it began. */
	return &i915->display_runtime;
}

/* Tells whether the display version lies in [from, until] (the Linux IS_DISPLAY_VER()). */
static __inline int
i915_dp_is_display_ver(
	struct drm_i915_private *i915,
	int from,
	int until)
{
	int version;

	/* Reads the display version. */
	version = i915_vbt_display_ver(i915);

	/* A version below the range is outside it. */
	if (version < from)
		return 0;

	/* A version above the range is outside it. */
	if (version > until)
		return 0;

	/* Succeeded: the version is in the range. */
	return 1;
}

/* Sleeps for a number of milliseconds (the Linux msleep()). */
static __inline void
i915_dp_msleep(
	unsigned int ms)
{
	/* Sleeps through the live eDP's environment. */
	drv_i915_dp_sleep_us(ms * 1000u);
}

/*
 * Sleeps for at least lo microseconds (the Linux usleep_range()).
 *
 * The upper bound only allows the Linux scheduler slack; the sleep is lo.
 */
static __inline void
i915_dp_usleep_range(
	unsigned int lo,
	unsigned int hi)
{
	UNUSED_PARAMETER(hi);

	/* Sleeps through the live eDP's environment. */
	drv_i915_dp_sleep_us(lo);
}

/* The time now in jiffies, which are milliseconds here (the Linux jiffies). */
static __inline unsigned long
i915_dp_jiffies(void)
{
	u64 now;

	/* Reads the live eDP's clock. */
	now = drv_i915_dp_now_ms();

	/* Succeeded: reports the time as jiffies. */
	return (unsigned long)now;
}

/* Converts milliseconds to jiffies, which are milliseconds here (the Linux msecs_to_jiffies()). */
static __inline unsigned long
i915_dp_msecs_to_jiffies(
	unsigned long ms)
{
	/* Reports the same count. */
	return ms;
}

/*
 * Sleeps until to_wait_ms have passed since a jiffies timestamp (the Linux
 * i915_utils.h wait_remaining_ms_from_jiffies()).
 */
static __inline void
i915_wait_remaining_ms_from_jiffies(
	unsigned long timestamp_jiffies,
	int to_wait_ms)
{
	unsigned long target;
	unsigned long now;

	/* Computes the jiffy the wait ends at and reads the current one. */
	target = timestamp_jiffies + (unsigned long)to_wait_ms + 1ul;
	now = i915_dp_jiffies();

	/* Sleeps only for what remains, if anything does. */
	if (target > now)
		drv_i915_dp_sleep_us((unsigned)(target - now) * 1000u);
}

/* Takes a power-domain reference (the Linux intel_display_power_get()). */
static __inline intel_wakeref_t
i915_dp_intel_display_power_get(
	struct drm_i915_private *i915,
	int domain)
{
	intel_wakeref_t wakeref;

	/* Takes the reference through the environment. */
	wakeref = drv_i915_dp_power_get(i915, domain);

	/* Reports a get the environment refused (-1). */
	if (wakeref < 0)
		return wakeref;

	/* Succeeded: the caller holds the reference. */
	return wakeref;
}

/* Returns a power-domain reference (the Linux intel_display_power_put()). */
static __inline void
i915_dp_intel_display_power_put(
	struct drm_i915_private *i915,
	int domain,
	intel_wakeref_t wakeref)
{
	/* Returns the reference through the environment. */
	drv_i915_dp_power_put(i915, domain, wakeref);
}

/*
 * Hands a power-domain reference to the power layer, which releases it later
 * (the Linux intel_display_power_put_async()).
 */
static __inline void
i915_dp_intel_display_power_put_async(
	struct drm_i915_private *i915,
	int domain,
	intel_wakeref_t wakeref)
{
	/* Parks the reference with the power layer through the environment. */
	drv_i915_dp_power_put_async(i915, domain, wakeref);
}

/* Makes a PPS or AUX mutex free and unused (the Linux mutex_init()). */
static __inline void
i915_dp_mutex_init(
	struct i915_dp_mutex *m)
{
	/* A free mutex that was never taken; the binding to the environment is kept. */
	m->held = 0;
	m->acquisitions = 0;
}

/* Prepares a delayed work with its body (the Linux INIT_DELAYED_WORK()). */
static __inline void
i915_dp_init_delayed_work(
	struct delayed_work *dw,
	void (*fn)(struct work_struct *))
{
	/* Clears the work, which also selects slot 0 (I915_DP_WORK_VDD_OFF), and sets its body. */
	kern_memset(dw, 0, sizeof(*dw));
	dw->fn = fn;
}

/*
 * Cancels a delayed work and waits for a running body (the Linux
 * cancel_delayed_work_sync()).
 *
 * It reports whether the work was pending.
 */
static __inline bool
i915_dp_cancel_delayed_work_sync(
	struct delayed_work *dw)
{
	bool pending;

	/* Cancels through the environment's backend and waits for the body. */
	pending = drv_i915_dp_delayed_cancel(dw, 1);

	/* Reports a work that was not pending. */
	if (!pending)
		return false;

	/* Succeeded: the pending work was cancelled. */
	return true;
}

/*
 * Queues a delayed work (the Linux queue_delayed_work()).
 *
 * The backend has one queue of its own, so the work queue argument is not
 * used.  It reports whether the work was newly queued.
 */
static __inline bool
i915_dp_queue_delayed_work(
	void *wq,
	struct delayed_work *dw,
	unsigned long delay)
{
	bool queued;

	UNUSED_PARAMETER(wq);

	/* Queues the work through the environment's backend. */
	queued = drv_i915_dp_delayed_queue(dw, delay);

	/* Reports a work that was already pending. */
	if (!queued)
		return false;

	/* Succeeded: the work is newly queued. */
	return true;
}

/* Runs I2C messages on an adapter through its algorithm (the Linux i2c_transfer()). */
static __inline int
i915_i2c_transfer(
	struct i2c_adapter *adap,
	struct i2c_msg *msgs,
	int num)
{
	int transferred;

	/* Runs the messages through the adapter's algorithm. */
	transferred = adap->algo->master_xfer(adap, msgs, num);

	/* Reports a failed transfer as its negative errno. */
	if (transferred < 0)
		return transferred;

	/* Succeeded: reports how many messages were done. */
	return transferred;
}

/* The digital port a DP output belongs to (the Linux dp_to_dig_port()). */
static __inline struct intel_digital_port *
i915_dp_dp_to_dig_port(
	struct intel_dp *dp)
{
	/* The DP output is embedded in its digital port. */
	return container_of(dp, struct intel_digital_port, dp);
}

/* The device a DP output belongs to (the Linux dp_to_i915()). */
static __inline struct drm_i915_private *
i915_dp_dp_to_i915(
	struct intel_dp *dp)
{
	struct intel_digital_port *dig_port;

	/* Finds the digital port the output is embedded in. */
	dig_port = i915_dp_dp_to_dig_port(dp);

	/* Succeeded: reports the port's device. */
	return dig_port->i915;
}

/*
 * The device a DRM device belongs to (the Linux to_i915()).
 *
 * The kept functions only pass an encoder's device, which the eDP points at
 * the device's drm member.
 */
static __inline struct drm_i915_private *
i915_dp_to_i915(
	struct drm_device *dev)
{
	/* The DRM device is embedded in the i915 device. */
	return container_of(dev, struct drm_i915_private, drm);
}

/* Tells whether a DP output is an eDP panel (the Linux intel_dp_is_edp()). */
static __inline bool
i915_dp_is_edp(
	struct intel_dp *dp)
{
	/* Reports what the eDP set when it began. */
	return dp->is_edp;
}

/*
 * The power domain of a port's AUX channel (the Linux
 * intel_aux_power_domain()).
 *
 * A non-TBT AUX channel maps to POWER_DOMAIN_AUX_A + (aux_ch - AUX_CH_A).
 */
static __inline int
i915_aux_power_domain(
	struct intel_digital_port *dig_port)
{
	/* Reports the AUX domain of the port's channel. */
	return POWER_DOMAIN_AUX_A + (int)(dig_port->aux_ch - AUX_CH_A);
}

/* Tells whether a Type-C port is in TBT-alt mode (the Linux intel_tc_port_in_tbt_alt_mode()): the eDP port is not Type-C. */
static __inline bool
i915_dp_intel_tc_port_in_tbt_alt_mode(
	struct intel_digital_port *dig_port)
{
	UNUSED_PARAMETER(dig_port);

	/* Reports that the combo-PHY eDP port is never in TBT-alt mode. */
	return false;
}

/* Tells whether a device quirk applies (the Linux intel_has_quirk()): none does. */
static __inline bool
i915_dp_intel_has_quirk(
	struct drm_i915_private *i915,
	int quirk)
{
	UNUSED_PARAMETER(i915);
	UNUSED_PARAMETER(quirk);

	/* Reports that no quirk applies to this device. */
	return false;
}

/*
 * The functions the eDP files of this environment (dp-sink.c, aux.c,
 * panel.c, edid-read.c) export to one another with the environment's own
 * types.  The world-neutral ones are in dp-sink.h, aux.h and panel.h.
 */

/* Binds an eDP port's AUX channel to this platform's registers and transfer (aux.c). */
void drv_i915_dp_aux_init(struct intel_dp *intel_dp);

/* Prepares an AUX channel's mutex and I2C-over-AUX adapter (dp-sink.c, the Linux drm_dp_aux_init()). */
void drv_i915_drm_dp_aux_init(struct drm_dp_aux *aux);

/*
 * Reads the base EDID block and its extensions over a DDC adapter
 * (edid-read.c): the number of valid blocks, or a negative Linux errno.
 * hotplug-internal.h declares it for the HDMI detection as well.
 */
int drv_i915_drm_edid_read(struct i2c_adapter *ddc, u8 *buf, unsigned max_blocks, unsigned *extensions);

/*
 * The panel power sequencer (panel.c, the Linux intel_pps_*()).
 *
 * The *_unlocked functions expect the PPS lock held (drv_i915_pps_lock());
 * the others take it themselves.
 */
intel_wakeref_t drv_i915_pps_lock(struct intel_dp *intel_dp);
intel_wakeref_t drv_i915_pps_unlock(struct intel_dp *intel_dp, intel_wakeref_t wakeref);
void drv_i915_pps_check_power_unlocked(struct intel_dp *intel_dp);
bool drv_i915_pps_vdd_on_unlocked(struct intel_dp *intel_dp);
void drv_i915_pps_vdd_off_unlocked(struct intel_dp *intel_dp, bool sync);
void drv_i915_pps_on_unlocked(struct intel_dp *intel_dp);
void drv_i915_pps_off_unlocked(struct intel_dp *intel_dp);
void drv_i915_pps_wait_power_cycle(struct intel_dp *intel_dp);
void drv_i915_pps_vdd_on(struct intel_dp *intel_dp);
void drv_i915_pps_vdd_off_sync(struct intel_dp *intel_dp);
void drv_i915_pps_on(struct intel_dp *intel_dp);
void drv_i915_pps_off(struct intel_dp *intel_dp);
void drv_i915_pps_backlight_on(struct intel_dp *intel_dp);
void drv_i915_pps_backlight_off(struct intel_dp *intel_dp);
bool drv_i915_pps_have_panel_power_or_vdd(struct intel_dp *intel_dp);
void drv_i915_pps_encoder_reset(struct intel_dp *intel_dp);
bool drv_i915_pps_init(struct intel_dp *intel_dp);
void drv_i915_pps_init_late(struct intel_dp *intel_dp);

#endif /* DRIVERS_GPU_I915_DISPLAY_DP_INTERNAL_H */
