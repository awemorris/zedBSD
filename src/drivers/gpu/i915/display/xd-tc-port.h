/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C ports of the display 11 to 13 code (xd-tc-port.c): which
 * mode each port is held in, and when the display takes its PHY and
 * gives it back.
 *
 * A port is declared once, with whether a connector is wired to it
 * (legacy) and its AUX channel.  The port is then held in a mode that
 * follows what is plugged in: a legacy connector over a DisplayPort
 * alternate mode partner over a Thunderbolt one; with nothing plugged in
 * a legacy port stays legacy and a receptacle is left to Thunderbolt.
 * The connection itself (the PHY taken, TC cold blocked, the lanes
 * checked) is the platform's flow in xd-tc.c.
 *
 * Every output that drives a port holds a link on it; while one does,
 * the mode stays.  A step that uses the PHY without an output locks the
 * port, which brings its mode up to date first, and the unlock of a port
 * no link holds gives the PHY back at once (the firmware does not update
 * the other Type-C ports' hotplug state while one is held without its
 * sink).
 *
 * What the ports need from the device -- a lock per port, the power
 * wells by function, a clock and a sleep, the log -- comes through
 * struct i915_xd_tc_port_env and struct i915_xd_tc_power.  The port lock
 * is taken before any power well's lock and a thread may sleep under it.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_TC_PORT_H
#define DRIVERS_GPU_I915_DISPLAY_XD_TC_PORT_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"
#include "xd-tc.h"

/*
 * What the ports ask the device for, besides the power wells.
 *
 * lock and unlock serialize the steps of one port (numbered from 0, TC1);
 * now_us reads a monotonic clock and answers 0, or nonzero when the time
 * base failed; sleep_us waits at least the time asked, giving the CPU to
 * other threads, and answers 0 or nonzero likewise; log writes one line.
 */
struct i915_xd_tc_port_env {
	void *ctx;
	void (*lock)(void *ctx, unsigned tc);
	void (*unlock)(void *ctx, unsigned tc);
	int (*now_us)(void *ctx, uint64_t *now);
	int (*sleep_us)(void *ctx, unsigned us);
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
};

/*
 * One Type-C port.
 *
 * It lives in struct i915_xd_tc_ports for the display's life.  Everything
 * but index changes only under the port's lock.  present is cleared by
 * the stop, so a step already on its way finds the port retired.  links
 * counts the outputs on the port: while it is nonzero the connection
 * stays, and the last one gone gives the PHY back.  The connection's mode
 * is the mode the port is held in (I915_XD_TC_MODE_*).
 */
struct i915_xd_tc_port {
	unsigned index;
	int present;
	int legacy;
	unsigned aux;
	const struct i915_xd_port_desc *desc;
	struct i915_xd_tc_connection connection;
	unsigned links;
	unsigned connects;
	unsigned connect_failures;
	unsigned disconnects;
};

/*
 * The Type-C ports of one display.
 *
 * The display owns it for the device's life.  live is zero until
 * drv_i915_xd_tc_ports_init() bound the device, and again after
 * drv_i915_xd_tc_ports_stop(): ports that are not live answer every
 * question with "nothing" and take nothing.
 */
struct i915_xd_tc_ports {
	struct i915_xd_io *io;
	const struct i915_xd_platform *platform;
	struct i915_xd_tc_power power;
	struct i915_xd_tc_port_env env;
	int live;
	struct i915_xd_tc_port port[I915_XD_TC_PORTS];
};

/*
 * What a port has of DisplayPort for the Type-C connector layer: whether
 * a DisplayPort alternate mode partner is live, and while the display
 * holds the port in that mode, the pin assignment (3 is C) and the lanes
 * the FIA gave (both 0 otherwise).
 */
struct i915_xd_tc_dp_sample {
	int hpd;
	unsigned pin;
	unsigned lanes;
};

void drv_i915_xd_tc_ports_init(struct i915_xd_tc_ports *ports, struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_tc_power *power, const struct i915_xd_tc_port_env *env);
int drv_i915_xd_tc_ports_declare(struct i915_xd_tc_ports *ports, unsigned ddi, int legacy, unsigned aux);
int drv_i915_xd_tc_ports_index(const struct i915_xd_tc_ports *ports, unsigned ddi);
void drv_i915_xd_tc_ports_readout(struct i915_xd_tc_ports *ports);
void drv_i915_xd_tc_ports_suspend(struct i915_xd_tc_ports *ports);
void drv_i915_xd_tc_ports_resume(struct i915_xd_tc_ports *ports);
void drv_i915_xd_tc_ports_stop(struct i915_xd_tc_ports *ports);
unsigned drv_i915_xd_tc_ports_live(struct i915_xd_tc_ports *ports, unsigned tc);
int drv_i915_xd_tc_ports_connected(struct i915_xd_tc_ports *ports, unsigned tc);
int drv_i915_xd_tc_ports_connected_locked(struct i915_xd_tc_ports *ports, unsigned tc);
int drv_i915_xd_tc_ports_link_needs_reset(struct i915_xd_tc_ports *ports, unsigned tc);
unsigned drv_i915_xd_tc_ports_lock(struct i915_xd_tc_ports *ports, unsigned tc, unsigned lanes);
void drv_i915_xd_tc_ports_unlock(struct i915_xd_tc_ports *ports, unsigned tc);
void drv_i915_xd_tc_ports_get_link(struct i915_xd_tc_ports *ports, unsigned tc, unsigned lanes);
void drv_i915_xd_tc_ports_put_link(struct i915_xd_tc_ports *ports, unsigned tc);
unsigned drv_i915_xd_tc_ports_mode(struct i915_xd_tc_ports *ports, unsigned tc);
unsigned drv_i915_xd_tc_ports_max_lanes(struct i915_xd_tc_ports *ports, unsigned tc);
unsigned drv_i915_xd_tc_ports_pin(struct i915_xd_tc_ports *ports, unsigned tc);
void drv_i915_xd_tc_ports_set_lanes(struct i915_xd_tc_ports *ports, unsigned tc, unsigned lanes, int reversed);
void drv_i915_xd_tc_ports_dp_sample(struct i915_xd_tc_ports *ports, unsigned tc, struct i915_xd_tc_dp_sample *sample);
void drv_i915_xd_tc_ports_log_state(struct i915_xd_tc_ports *ports, unsigned tc, const char *why);
const char *drv_i915_xd_tc_mode_name(unsigned mode);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_TC_PORT_H */
