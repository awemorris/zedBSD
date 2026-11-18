/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hotplug of the display 11 to 13 code (xd-hpd.c): the hotplug pins
 * of the north display (the Type-C and Thunderbolt pins) and of the south
 * display (the DDI and Type-C pins), and the connectors behind them.
 *
 * The pins are armed from the ports the display has.  An interrupt is
 * acknowledged and sorted into pulses: a port that takes its own pulses
 * (DisplayPort) gets them through the pulse work, any other pin becomes a
 * detection event.  A pin that raises too many interrupts in a second is
 * masked and armed again two minutes later.  The detection reads a
 * connector's state: the live state and the EDID over GMBUS of an HDMI
 * connector, the live state and the sink's probe of a Type-C DisplayPort
 * connector (through the hooks of struct i915_xd_hpd_env), and asks for
 * another look when nothing changed (a sink still waking up).
 *
 * Locking: the pins' state, the pending pulses and events are the
 * caller's to protect with one spinlock, which the interrupt takes too;
 * the functions that take that lock's protection say so.  The connectors
 * are the detection's, which the caller serializes with one mutex; the
 * hooks of the detection run under that mutex and take only their own
 * locks.  No function here takes a lock.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_HPD_H
#define DRIVERS_GPU_I915_DISPLAY_XD_HPD_H

#include <stddef.h>
#include <stdint.h>

#include "xd-gmbus.h"
#include "xd-io.h"
#include "xd-platform.h"

/* The pins: DDI A to D, then Type-C 1 to 6. */
#define I915_XD_HPD_PIN_DDI(ddi)	(ddi)
#define I915_XD_HPD_PIN_TC(tc)		(4U + (tc))
#define I915_XD_HPD_PINS		10U
#define I915_XD_HPD_PIN_NONE		0xffU

/* What a pin does with its interrupts: takes them, is masked by a storm about to be, or is masked. */
#define I915_XD_HPD_PIN_ENABLED		0U
#define I915_XD_HPD_PIN_MARKED		1U
#define I915_XD_HPD_PIN_DISABLED	2U

/* The connectors a display has at most, and the EDID a connector keeps (four blocks). */
#define I915_XD_HPD_CONNECTORS		8U
#define I915_XD_HPD_EDID_BYTES		512U

/* What kind of connector: the built-in panel, HDMI, or an external DisplayPort. */
#define I915_XD_HPD_EDP			1U
#define I915_XD_HPD_HDMI		2U
#define I915_XD_HPD_DP			3U

/* A connector's state, as the display inventory numbers it. */
#define I915_XD_HPD_CONNECTED		1
#define I915_XD_HPD_DISCONNECTED	2
#define I915_XD_HPD_UNKNOWN		3

/* What a detection found: nothing changed, the connector changed, or look again later. */
#define I915_XD_HPD_UNCHANGED		0
#define I915_XD_HPD_CHANGED		1
#define I915_XD_HPD_RETRY		2

/* What an interrupt asks of the caller (drv_i915_xd_hpd_sort()). */
#define I915_XD_HPD_QUEUE_PULSES	0x1U
#define I915_XD_HPD_QUEUE_DETECT	0x2U
#define I915_XD_HPD_STORM		0x4U

/* The power a detection takes: the display core, or the GMBUS controller's. */
#define I915_XD_HPD_POWER_CORE		0U
#define I915_XD_HPD_POWER_DDC		1U

/*
 * The storm limit: a pin may raise this many in a second (a long pulse
 * counts ten), the masked pin is armed again after two minutes, and a
 * detection that found nothing is repeated after a second.
 */
#define I915_XD_HPD_STORM_LIMIT		50
#define I915_XD_HPD_STORM_PERIOD_MS	1000U
#define I915_XD_HPD_STORM_REARM_MS	(2U * 60U * 1000U)
#define I915_XD_HPD_RETRY_MS		1000U

/*
 * What the detection asks of the rest of the driver: power references,
 * and, for the Type-C DisplayPort ports whose connection and sink the
 * older Type-C and external DisplayPort code still keep, the port's live
 * state, the sink's probe, a short pulse's check, a long pulse's reset of
 * the sink's limits and the link's check after a detection.  A hook that
 * is NULL is a port that is not looked at (its connector stays unknown).
 * ctx is handed to every hook.
 */
struct i915_xd_hpd_env {
	void *ctx;

	/* Takes (on) or gives back (off) a power reference; a refused one answers nonzero. */
	int (*power)(void *ctx, unsigned what, int on);

	/* The live state of a Type-C port (by its DDI): nonzero when something the display uses is plugged in. */
	int (*tc_live)(void *ctx, unsigned ddi);

	/* Probes the sink of a DisplayPort port: its state and its EDID's bytes. */
	int (*dp_probe)(void *ctx, unsigned ddi, uint8_t *edid, size_t size, unsigned *bytes);

	/* Checks the sink after a short pulse: nonzero when it is as it was. */
	int (*dp_pulse)(void *ctx, unsigned ddi);

	/* Gives a sink its own link limits back after a long pulse. */
	void (*dp_reset)(void *ctx, unsigned ddi);

	/* Looks at a port's link after a detection (a Type-C link's reset, a lit link's retraining). */
	void (*link)(void *ctx, unsigned ddi, int hdmi);
};

/* One hotplug pin: its bits, whether a connector uses it, its state and its storm count. */
struct i915_xd_hpd_pin {
	uint32_t north;
	uint32_t south;
	uint8_t used;
	uint8_t state;
	int count;
	uint64_t period_ms;
};

/*
 * One connector: its kind, port and pin, its GMBUS pin (HDMI), its name,
 * its state and the EDID it read; the epoch moves whenever its state or
 * its EDID changes, the retries count the looks since the last event.
 */
struct i915_xd_hpd_connector {
	uint8_t kind;
	uint8_t ddi;
	uint8_t tc;
	uint8_t pin;
	uint8_t ddc_pin;
	char name[12];
	int status;
	int live;
	uint64_t epoch;
	unsigned retries;
	unsigned edid_size;
	uint8_t edid[I915_XD_HPD_EDID_BYTES];
};

/* The words the last arming left in the hotplug registers, for the log. */
struct i915_xd_hpd_words {
	uint32_t north_used;
	uint32_t north_enabled;
	uint32_t de_imr;
	uint32_t tc_ctl;
	uint32_t tbt_ctl;
	uint32_t south_used;
	uint32_t south_enabled;
	uint32_t filter;
	uint32_t sde_imr;
	uint32_t ddi_ctl;
	uint32_t tc_south_ctl;
	int sde_imr_written;
};

/*
 * The hotplug of one display: the platform and its south display, the
 * pins, the connectors, the pulses and events the interrupt left for the
 * works, the GMBUS controller's counts and what the interrupts did.
 */
struct i915_xd_hpd {
	const struct i915_xd_platform *platform;
	int south;
	int interrupts;
	struct i915_xd_hpd_pin pin[I915_XD_HPD_PINS];
	struct i915_xd_hpd_connector connector[I915_XD_HPD_CONNECTORS];
	unsigned count;
	unsigned hdmi;
	unsigned dp;
	unsigned edp;
	uint32_t long_ports;
	uint32_t short_ports;
	uint32_t events;
	uint32_t retries;
	struct i915_xd_hpd_words words;
	struct i915_xd_gmbus gmbus;
	unsigned arms;
	unsigned storms;
	unsigned unexpected;
	unsigned disabled_irqs;
	unsigned gmbus_irqs;
};

/* The display 9 DDIs whose sense the south display has: B to D. */
#define I915_XD9_HPD_FIRST_DDI		1U
#define I915_XD9_HPD_LAST_DDI		3U

void drv_i915_xd9_hpd_enable(struct i915_xd_io *io);
unsigned drv_i915_xd9_hpd_present(struct i915_xd_io *io);
unsigned drv_i915_xd9_hpd_connected(struct i915_xd_io *io);
void drv_i915_xd_hpd_init(struct i915_xd_hpd *hpd, const struct i915_xd_platform *platform, int south);
unsigned drv_i915_xd_hpd_pin_of(const struct i915_xd_platform *platform, unsigned ddi);
int drv_i915_xd_hpd_add(struct i915_xd_hpd *hpd, unsigned kind, unsigned ddi);
void drv_i915_xd_hpd_arm(struct i915_xd_hpd *hpd, struct i915_xd_io *io, int interrupts);
void drv_i915_xd_hpd_south_irq(struct i915_xd_hpd *hpd, struct i915_xd_io *io, uint32_t iir, uint32_t *pins, uint32_t *longs);
void drv_i915_xd_hpd_north_irq(struct i915_xd_hpd *hpd, struct i915_xd_io *io, uint32_t iir, uint32_t *pins, uint32_t *longs);
unsigned drv_i915_xd_hpd_sort(struct i915_xd_hpd *hpd, struct i915_xd_io *io, uint32_t pins, uint32_t longs, uint64_t now_ms);
int drv_i915_xd_hpd_storms(struct i915_xd_hpd *hpd);
void drv_i915_xd_hpd_rearm(struct i915_xd_hpd *hpd, struct i915_xd_io *io);
int drv_i915_xd_hpd_pulse(struct i915_xd_hpd *hpd, const struct i915_xd_hpd_env *env, unsigned index, int long_pulse);
int drv_i915_xd_hpd_probe(struct i915_xd_hpd *hpd, struct i915_xd_io *io, const struct i915_xd_hpd_env *env, unsigned index);
int drv_i915_xd_hpd_detect(struct i915_xd_hpd *hpd, struct i915_xd_io *io, const struct i915_xd_hpd_env *env, unsigned index, int event);
const char *drv_i915_xd_hpd_status_name(int status);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_HPD_H */
