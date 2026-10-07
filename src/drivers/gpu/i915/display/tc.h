/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C ports of the display (tc.c).
 *
 * A Type-C port's PHY is shared between the display, USB and Thunderbolt.
 * The PD controller's firmware enters DisplayPort Alternate Mode and the
 * Type-C subsystem's firmware (IOM) assigns the lanes; the display only
 * reads the result and takes the PHY when it is going to use the port:
 *
 *   - the live status says what is plugged in (DP-alt, Thunderbolt, or a
 *     legacy DP/HDMI connector wired to the port);
 *   - the PHY is ready when the Type-C subsystem lets the display take it;
 *   - the display owns the PHY by a bit of the port's DDI buffer control,
 *     and keeps the Type-C subsystem out of TC cold by holding the port's
 *     AUX_USBC power domain;
 *   - the FIA says which of the four lanes carry DisplayPort and which pin
 *     assignment the partner chose.
 *
 * This is display version 13 (Alder Lake-P) only.  The register layout is
 * the hardware's as Linux v6.8.12 describes it (MIT); the code is written
 * for this driver.  The core is free of the kernel: whatever it needs from
 * the device -- registers, power domains, a lock per port, a delay and the
 * log -- it asks through struct i915_tc_env, so the host tests run it over
 * fake registers.
 *
 * Thunderbolt alt mode is outside the driver's scope: a port whose PHY the
 * display does not own is in I915_TC_MODE_TBT, which is never used for an
 * output and holds no power.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_TC_H
#define DRIVERS_GPU_I915_DISPLAY_TC_H

#include <stdint.h>

/* The Type-C ports of display version 13 (TC1 to TC4). */
#define I915_TC_PORTS			4u

/* The live-status bits of a port: what the hardware says is plugged in. */
#define I915_TC_LIVE_DP_ALT		(1u << 0)
#define I915_TC_LIVE_TBT		(1u << 1)
#define I915_TC_LIVE_LEGACY		(1u << 2)

/*
 * How the display holds a Type-C port.
 *
 * NONE: the display holds nothing of the port.  TBT: the port is left to
 * Thunderbolt or to nothing; the display does not own the PHY.  DP_ALT and
 * LEGACY: the display owns the PHY and blocks TC cold, for a DP-alt partner
 * or for a connector wired to the port.
 */
enum i915_tc_mode {
	I915_TC_MODE_NONE = 0,
	I915_TC_MODE_TBT,
	I915_TC_MODE_DP_ALT,
	I915_TC_MODE_LEGACY
};

/*
 * The power a Type-C port's steps need, which the environment maps onto
 * the display's power domains.
 *
 * CORE: the display core (the hotplug status and the Type-C subsystem's
 * status registers).  PORT: the port's DDI lanes (its DDI buffer control,
 * where the PHY ownership lives).  COLD: the port's AUX_USBC domain, which
 * keeps the Type-C subsystem out of TC cold while the display uses the PHY.
 */
enum i915_tc_power {
	I915_TC_POWER_CORE = 0,
	I915_TC_POWER_PORT,
	I915_TC_POWER_COLD
};

/*
 * What the Type-C core needs from the device.
 *
 * The device binds one for the display's lifetime.  power_get returns 0
 * once the power is on, or an error; every successful get is matched by
 * one put.  lock and unlock serialize the steps of one port; the power
 * domains' lock is taken inside it, and the port's lock is one a thread may
 * sleep under.  now_us reads a monotonic clock in microseconds and returns
 * 0, or nonzero when the time base failed.  sleep_us waits at least the
 * time asked for, giving the CPU to other threads (it may wait longer, to
 * the scheduler's next tick), and returns 0, or nonzero when the time base
 * failed; the core calls it only from a step that may sleep.
 */
struct i915_tc_env {
	/* What every callback is given. */
	void *ctx;

	/* The display registers. */
	uint32_t (*read32)(void *ctx, uint32_t reg);
	void (*write32)(void *ctx, uint32_t reg, uint32_t value);

	/* A port's power. */
	int (*power_get)(void *ctx, enum i915_tc_power power, unsigned port);
	void (*power_put)(void *ctx, enum i915_tc_power power, unsigned port);

	/* The lock of one port. */
	void (*lock)(void *ctx, unsigned port);
	void (*unlock)(void *ctx, unsigned port);

	/* The time now, and a wait that sleeps. */
	int (*now_us)(void *ctx, uint64_t *now);
	int (*sleep_us)(void *ctx, unsigned us);

	/* A line of the driver's log. */
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
};

/*
 * One Type-C port of the display.
 *
 * It lives in struct i915_tc for the display's lifetime.  Everything below
 * index, fia and fia_slot changes only under the port's lock.  links counts
 * the outputs that use the port; while it is nonzero the mode does not
 * change, and the last one gone gives the PHY back at once.
 */
struct i915_tc_port {
	/* The port's number from 0 (TC1). */
	unsigned index;

	/*
	 * Nonzero when the VBT declares the port; an undeclared port is never
	 * touched.  drv_i915_tc_stop() clears it under the port's lock once the
	 * PHY is given back, so a step that was already on its way finds the
	 * port retired and takes nothing.
	 */
	int present;

	/*
	 * Nonzero when a connector is wired to the port (the VBT supports neither
	 * USB-C nor Thunderbolt on it).  The live status corrects it when the two
	 * disagree.
	 */
	int legacy;

	/* The FIA instance that serves the port, and the port's slot in it. */
	unsigned fia;
	unsigned fia_slot;

	/* How the display holds the port. */
	enum i915_tc_mode mode;

	/* Nonzero while the port holds its COLD power (it blocks TC cold). */
	int cold_held;

	/* The outputs that use the port. */
	unsigned links;

	/* How many times the PHY was taken, refused, and given back (diagnostics). */
	unsigned connects;
	unsigned connect_failures;
	unsigned disconnects;
};

/*
 * The Type-C ports of one display.
 *
 * It lives in the display for the device's lifetime.  live is zero until
 * drv_i915_tc_init() binds the environment, and again once
 * drv_i915_tc_stop() gave every port back; a display that is not live has
 * no Type-C port and answers every question with "nothing".
 */
struct i915_tc {
	/* What the core asks the device for. */
	struct i915_tc_env env;

	/* The display version the ports were prepared for. */
	unsigned display_ver;

	/* Nonzero once the environment is bound. */
	int live;

	/* The ports, TC1 first. */
	struct i915_tc_port port[I915_TC_PORTS];
};

/*
 * What the display reads of DisplayPort on a Type-C port, for the Type-C
 * connector layer: whether a DP-alt partner's hot plug detect is live,
 * and, while the display holds the port in DP-alt, the pin assignment the
 * FIA records (3 is pin C, 4 is D, 5 is E) and the lanes it assigned
 * (both 0 otherwise: the FIA is not read in TC cold).
 */
struct i915_tc_dp_sample {
	int hpd;
	unsigned pin;
	int lanes;
};

void drv_i915_tc_init(struct i915_tc *tc, const struct i915_tc_env *env, unsigned display_ver);
void drv_i915_tc_declare(struct i915_tc *tc, unsigned port, int legacy);
void drv_i915_tc_readout(struct i915_tc *tc);
void drv_i915_tc_stop(struct i915_tc *tc);
uint32_t drv_i915_tc_live_status(struct i915_tc *tc, unsigned port);
int drv_i915_tc_connected(struct i915_tc *tc, unsigned port);
int drv_i915_tc_connected_locked(struct i915_tc *tc, unsigned port);
int drv_i915_tc_link_needs_reset(struct i915_tc *tc, unsigned port);
enum i915_tc_mode drv_i915_tc_lock(struct i915_tc *tc, unsigned port, int required_lanes);
void drv_i915_tc_unlock(struct i915_tc *tc, unsigned port);
void drv_i915_tc_get_link(struct i915_tc *tc, unsigned port, int required_lanes);
void drv_i915_tc_put_link(struct i915_tc *tc, unsigned port);
enum i915_tc_mode drv_i915_tc_mode(struct i915_tc *tc, unsigned port);
int drv_i915_tc_max_lanes(struct i915_tc *tc, unsigned port);
unsigned drv_i915_tc_pin_assignment(struct i915_tc *tc, unsigned port);
void drv_i915_tc_set_fia_lane_count(struct i915_tc *tc, unsigned port, int required_lanes, int lane_reversal);
void drv_i915_tc_dp_sample(struct i915_tc *tc, unsigned port, struct i915_tc_dp_sample *sample);
void drv_i915_tc_log_state(struct i915_tc *tc, unsigned port, const char *why);
const char *drv_i915_tc_mode_name(enum i915_tc_mode mode);

#endif
