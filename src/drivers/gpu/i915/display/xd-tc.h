/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C PHYs of the display 12 and 13 code (xd-tc.c): the register
 * side of the Dekel PHY of each Type-C port.
 *
 * A Dekel PHY's registers are larger than the 4 KiB MMIO window each port
 * has: the upper bits of a PHY offset name a bank, which an index register
 * in front of the windows selects.  One index register serves four ports,
 * so the index write and the access through the window it selects are
 * made under one lock that every port shares, the innermost lock of the
 * xd display.  The display owns one struct i915_xd_dkl with that lock.
 *
 * Every PHY access of the xd code goes through the struct: the PHY
 * microcontroller's health after a Type-C AUX well came up, the lanes'
 * DisplayPort mode for the pin assignment, the transmit swing of the
 * lanes from the platform's Dekel table, the release of the lanes' PCS
 * soft reset, and the words of the port's PLL (xd-clock.c).  A caller
 * that has its own route to the PHY (a host test's model, or the older
 * modeset while it still records its writes) gives the struct hooks
 * instead, and the hooks' owner keeps the lock.
 *
 * The connection of a port is made with the platform's flow: the FIA
 * flow of display 12 (Tiger Lake: TC cold blocked through the firmware,
 * the live state, ready and ownership in the FIA's words, which a
 * modular FIA keeps for two ports each) or the TCSS flow of display 13
 * (the live state in the hotplug interrupt words, ready in the TCSS
 * status, the ownership in the DDI buffer word under the port's lane
 * well, TC cold blocked by the port's AUX well except in Thunderbolt
 * mode).  The connect takes and the disconnect gives back what the flow
 * needs, through the owner's power wells; the state machine around them
 * (when to connect, the delayed disconnect) is the caller's.
 *
 * A Type-C port is numbered here from 0 (TC1), except in a port
 * description (struct i915_xd_port_desc), whose tc counts from 1.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_TC_H
#define DRIVERS_GPU_I915_DISPLAY_XD_TC_H

#include <stdint.h>

#include <kern/lock.h>

#include "xd-io.h"
#include "xd-platform.h"

/* The most Type-C ports a platform has. */
#define I915_XD_TC_PORTS		6U

/* The lanes of a Dekel PHY, each with two transmitters. */
#define I915_XD_DKL_LANES		2U

/* How long a Type-C AUX well's enable waits for the PHY microcontroller's health. */
#define I915_XD_DKL_HEALTH_US		1000U

/*
 * The pin assignments of a Type-C port as the FIA reports them: none (a
 * legacy port wired straight to its connector), then A to F.
 */
#define I915_XD_TC_PIN_LEGACY		0x0U
#define I915_XD_TC_PIN_A		0x1U
#define I915_XD_TC_PIN_B		0x2U
#define I915_XD_TC_PIN_C		0x3U
#define I915_XD_TC_PIN_D		0x4U
#define I915_XD_TC_PIN_E		0x5U
#define I915_XD_TC_PIN_F		0x6U

/*
 * Another route to the PHY registers: read, write and change bits of the
 * register at a PHY offset of a Type-C port, each one access whole, under
 * the owner's own lock.  NULL hooks leave the windows in place.
 */
struct i915_xd_dkl_hooks {
	uint32_t (*read)(void *context, unsigned tc, uint32_t phy_offset);
	void (*write)(void *context, unsigned tc, uint32_t phy_offset, uint32_t value);
	void (*rmw)(void *context, unsigned tc, uint32_t phy_offset, uint32_t clear, uint32_t set);
	void *context;
};

/*
 * The Dekel PHY windows of one display.
 *
 * The display owns it for the device's life.  The lock is taken around
 * each index write and the access it selects, by any thread, never from
 * an interrupt; nothing else is taken under it.  ready is zero until
 * drv_i915_xd_dkl_init() ran: a struct that is not ready reads all-ones
 * (a PHY that does not answer) and writes nothing.
 */
struct i915_xd_dkl {
	/* The register window of the display. */
	struct i915_xd_io *io;

	/* Serializes the bank index registers and the accesses through the windows. */
	struct spinlock lock;

	/* Nonzero once the windows may be used. */
	int ready;

	/* Another route to the PHY; all NULL on the device. */
	struct i915_xd_dkl_hooks hooks;
};

/* The bits of a Type-C port's live state: what its sink put it in. */
#define I915_XD_TC_LIVE_DP_ALT		0x1U
#define I915_XD_TC_LIVE_TBT		0x2U
#define I915_XD_TC_LIVE_LEGACY		0x4U

/* The mode a Type-C port is connected in. */
#define I915_XD_TC_MODE_NONE		0U
#define I915_XD_TC_MODE_DP_ALT		1U
#define I915_XD_TC_MODE_TBT		2U
#define I915_XD_TC_MODE_LEGACY		3U

/*
 * Where a Type-C port's FIA words are: the FIA's base and the port's
 * place in it.
 */
struct i915_xd_tc_fia {
	uint32_t base;
	unsigned index;
};

/*
 * The owner's power wells, by what they power (I915_XD_POWERS_*): get
 * brings up the wells of a set and returns 0 or an error; put gives them
 * back.
 */
struct i915_xd_tc_power {
	int (*get)(void *context, uint64_t functions);
	void (*put)(void *context, uint64_t functions);
	void *context;
};

/*
 * A Type-C port's connection: the mode, the functions held to block TC
 * cold, the port's FIA, whether the display owns the PHY, the lanes the
 * sink gives (4 outside DisplayPort alternate mode) and the pin
 * assignment.
 */
struct i915_xd_tc_connection {
	unsigned mode;
	uint64_t blocked;
	struct i915_xd_tc_fia fia;
	int owned;
	unsigned max_lanes;
	unsigned pin;
};

void drv_i915_xd_dkl_init(struct i915_xd_dkl *dkl, struct i915_xd_io *io);
void drv_i915_xd_dkl_set_hooks(struct i915_xd_dkl *dkl, const struct i915_xd_dkl_hooks *hooks);
uint32_t drv_i915_xd_dkl_window(unsigned tc, uint32_t phy_offset, uint32_t *index_reg, uint32_t *index_value);
uint32_t drv_i915_xd_dkl_read(struct i915_xd_dkl *dkl, unsigned tc, uint32_t phy_offset);
void drv_i915_xd_dkl_write(struct i915_xd_dkl *dkl, unsigned tc, uint32_t phy_offset, uint32_t value);
void drv_i915_xd_dkl_rmw(struct i915_xd_dkl *dkl, unsigned tc, uint32_t phy_offset, uint32_t clear, uint32_t set);
int drv_i915_xd_tc_wait_health(struct i915_xd_dkl *dkl, unsigned tc, unsigned timeout_us);
int drv_i915_xd_tc_dp_mode(struct i915_xd_dkl *dkl, unsigned tc, unsigned pin_assignment, unsigned lanes);
int drv_i915_xd_tc_mg_dp_mode(struct i915_xd_io *io, unsigned tc, unsigned pin_assignment, unsigned lanes);
const struct i915_xd_dkl_swing_table *drv_i915_xd_tc_swing_table(const struct i915_xd_platform *platform, int hdmi, uint32_t rate_khz);
void drv_i915_xd_tc_signal_levels(struct i915_xd_dkl *dkl, const struct i915_xd_platform *platform, unsigned tc, const struct i915_xd_dkl_swing_table *table, const uint8_t levels[4], int hdmi);
const struct i915_xd_mg_swing_table *drv_i915_xd_tc_mg_swing_table(const struct i915_xd_platform *platform, int hdmi, uint32_t rate_khz);
void drv_i915_xd_tc_mg_signal_levels(struct i915_xd_io *io, unsigned tc, const struct i915_xd_mg_swing_table *table, const uint8_t levels[4], uint32_t port_khz);
void drv_i915_xd_tc_release_soft_reset(struct i915_xd_dkl *dkl, unsigned tc);
int drv_i915_xd_tc_fia(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned tc, struct i915_xd_tc_fia *fia);
unsigned drv_i915_xd_tc_live(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, const struct i915_xd_tc_fia *fia);
int drv_i915_xd_tc_ready(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned tc, const struct i915_xd_tc_fia *fia);
int drv_i915_xd_tc_own(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, const struct i915_xd_tc_fia *fia, int take);
int drv_i915_xd_tc_owned(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, const struct i915_xd_tc_fia *fia);
unsigned drv_i915_xd_tc_pin(struct i915_xd_io *io, const struct i915_xd_tc_fia *fia);
unsigned drv_i915_xd_tc_max_lanes(struct i915_xd_io *io, const struct i915_xd_tc_fia *fia);
void drv_i915_xd_tc_set_lanes(struct i915_xd_io *io, const struct i915_xd_tc_fia *fia, unsigned lanes, int reversed);
uint64_t drv_i915_xd_tc_cold_functions(const struct i915_xd_platform *platform, unsigned aux, unsigned mode);
int drv_i915_xd_tc_connect(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, unsigned aux, unsigned mode, unsigned lanes, const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);
void drv_i915_xd_tc_disconnect(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_TC_H */
