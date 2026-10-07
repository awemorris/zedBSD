/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C ports of the display (see tc.h).
 *
 * The hardware facts -- register offsets and bits, which power domain
 * blocks TC cold on Alder Lake-P, the order in which the PHY is taken and
 * given back, and how the live status, the ready bit and the ownership bit
 * combine into the port's mode -- are those Linux v6.8.12 relies on
 * (display/intel_tc.c, i915_reg.h, MIT, Intel Corporation).  The code is
 * this driver's own.
 *
 * Taking the PHY (a connect): with the port's DDI lanes powered, set the
 * ownership bit, check that the Type-C subsystem says the PHY is ready,
 * block TC cold, and for DP-alt check that the partner is still there and
 * offers enough lanes.  Every step that fails gives back what the earlier
 * ones took.  Giving it back (a disconnect) unblocks TC cold and clears the
 * ownership.
 *
 * The PHY is given back as soon as nothing uses the port: when the last
 * link goes, and when a port that was locked without a link is unlocked.
 * Linux delays this by a second; the firmware does not update the hotplug
 * status of the other Type-C ports while one is held with its sink gone, so
 * this driver gives it back at once.
 */

#include "tc.h"

#include <stddef.h>

/* The Type-C subsystem's status of each port (TCSS_DDI_STATUS): the first port's, and the distance between ports. */
#define I915_TC_TCSS_DDI_STATUS		0x161500u
#define I915_TC_TCSS_DDI_STATUS_STRIDE	4u

/* The Type-C subsystem lets the display take the PHY (TCSS_DDI_STATUS bit 2). */
#define I915_TC_TCSS_READY		(1u << 2)

/* What a register of a powered-down Type-C subsystem reads. */
#define I915_TC_REG_DEAD		0xffffffffu

/* The FIA instances' register blocks (FIA1, FIA2, FIA3). */
#define I915_TC_FIA1_BASE		0x163000u
#define I915_TC_FIA2_BASE		0x16e000u
#define I915_TC_FIA3_BASE		0x16f000u

/* The FIA's DisplayPort lane assignment (PORT_TX_DFLEXDPSP): a 4-bit lane mask per slot, 8 bits apart. */
#define I915_TC_FIA_DFLEXDPSP		0x8a0u
#define I915_TC_FIA_LANE_SHIFT		8u
#define I915_TC_FIA_LANE_MASK		0xfu

/* The FIA's pin assignment (PORT_TX_DFLEXPA1): a 4-bit field per slot, 4 bits apart. */
#define I915_TC_FIA_DFLEXPA1		0x880u
#define I915_TC_FIA_PIN_SHIFT		4u
#define I915_TC_FIA_PIN_MASK		0xfu

/*
 * The FIA's DisplayPort main link lanes (PORT_TX_DFLEXDPMLE1): a 4-bit
 * lane mask per slot, 4 bits apart.  One lane is lane 0 (lane 3 when the
 * lanes are reversed), two lanes are 0 and 1 (3 and 2), four are all.
 */
#define I915_TC_FIA_DFLEXDPMLE1		0x8c0u
#define I915_TC_FIA_MLE_SHIFT		4u
#define I915_TC_FIA_MLE_MASK		0xfu
#define I915_TC_FIA_MLE_ONE		0x1u
#define I915_TC_FIA_MLE_ONE_REVERSED	0x8u
#define I915_TC_FIA_MLE_TWO		0x3u
#define I915_TC_FIA_MLE_TWO_REVERSED	0xcu
#define I915_TC_FIA_MLE_FOUR		0xfu

/* How many Type-C ports one modular FIA serves (display version 13). */
#define I915_TC_PORTS_PER_FIA		2u

/* The DDI buffer control (DDI_BUF_CTL) of DDI port A, and the distance between ports. */
#define I915_TC_DDI_BUF_CTL_A		0x64000u
#define I915_TC_DDI_BUF_CTL_STRIDE	0x100u

/* The DDI port of the first Type-C port (PORT_TC1 is DDI D). */
#define I915_TC_FIRST_DDI_PORT		3u

/* The DDI buffer is enabled (bit 31), and the display owns the Type-C PHY (bit 6). */
#define I915_TC_DDI_BUF_ENABLE		(1u << 31)
#define I915_TC_DDI_BUF_PHY_OWNERSHIP	(1u << 6)

/* The display engine's hotplug status (GEN11_DE_HPD_ISR): the DP-alt bit of a port and its Thunderbolt bit. */
#define I915_TC_DE_HPD_ISR		0x44470u
#define I915_TC_DE_HPD_DP_ALT_SHIFT	16u
#define I915_TC_DE_HPD_TBT_SHIFT	0u

/* The south display's hotplug status (SDEISR): the legacy bit of a port. */
#define I915_TC_SDEISR			0xc4000u
#define I915_TC_SDE_LEGACY_SHIFT	24u

/*
 * How long a legacy port's PHY is waited for at a readout, and how long the
 * wait sleeps between two reads (the Linux tc_phy_wait_for_ready()'s 500 ms;
 * the sleep lasts at least to the scheduler's next tick).
 */
#define I915_TC_READY_TIMEOUT_US	500000u
#define I915_TC_READY_POLL_US		1000u

/* The lane count a port offers when nothing restricts it. */
#define I915_TC_FULL_LANES		4

static struct i915_tc_port *tc_port_of(struct i915_tc *tc, unsigned port);
static uint32_t tc_read(struct i915_tc *tc, uint32_t reg);
static void tc_write(struct i915_tc *tc, uint32_t reg, uint32_t value);
static int tc_power_get(struct i915_tc *tc, enum i915_tc_power power, unsigned port);
static void tc_power_put(struct i915_tc *tc, enum i915_tc_power power, unsigned port);
static uint32_t tc_fia_base(const struct i915_tc_port *p);
static uint32_t tc_ddi_buf_ctl(const struct i915_tc_port *p);
static uint32_t tc_live_status_raw(struct i915_tc *tc, unsigned port);
static int tc_is_ready(struct i915_tc *tc, const struct i915_tc_port *p);
static int tc_is_owned(struct i915_tc *tc, const struct i915_tc_port *p);
static void tc_set_ownership(struct i915_tc *tc, const struct i915_tc_port *p, int take);
static unsigned tc_lane_mask(struct i915_tc *tc, const struct i915_tc_port *p);
static int tc_lanes_of_mask(unsigned lane_mask);
static enum i915_tc_mode tc_mode_of_live(uint32_t live);
static enum i915_tc_mode tc_default_mode(const struct i915_tc_port *p);
static enum i915_tc_mode tc_target_mode(const struct i915_tc_port *p, uint32_t live);
static void tc_fix_legacy(struct i915_tc *tc, struct i915_tc_port *p, uint32_t live);
static void tc_wait_ready(struct i915_tc *tc, const struct i915_tc_port *p);
static enum i915_tc_mode tc_current_mode(struct i915_tc *tc, struct i915_tc_port *p);
static int tc_block_cold(struct i915_tc *tc, struct i915_tc_port *p);
static void tc_unblock_cold(struct i915_tc *tc, struct i915_tc_port *p);
static int tc_verify_mode(struct i915_tc *tc, struct i915_tc_port *p, int required_lanes);
static int tc_take_phy(struct i915_tc *tc, struct i915_tc_port *p, int required_lanes);
static void tc_connect(struct i915_tc *tc, struct i915_tc_port *p, int required_lanes);
static void tc_disconnect(struct i915_tc *tc, struct i915_tc_port *p);
static void tc_update_mode(struct i915_tc *tc, struct i915_tc_port *p, int required_lanes);
static void tc_readout_port(struct i915_tc *tc, struct i915_tc_port *p);
static int tc_connected_locked(struct i915_tc *tc, const struct i915_tc_port *p);

/*
 * Binds the environment and prepares the Type-C ports of a display.
 *
 * Every port starts undeclared and held in no mode.  On display version 13
 * each modular FIA serves two ports.
 */
void
drv_i915_tc_init(
	struct i915_tc *tc,
	const struct i915_tc_env *env,
	unsigned display_ver)
{
	struct i915_tc_port *p;
	unsigned index;

	/* Binds the environment. */
	tc->env = *env;
	tc->display_ver = display_ver;

	/* Prepares every port: undeclared, held in no mode, its FIA slot computed. */
	for (index = 0u; index < I915_TC_PORTS; index++) {
		p = &tc->port[index];
		p->index = index;
		p->present = 0;
		p->legacy = 0;
		p->fia = index / I915_TC_PORTS_PER_FIA;
		p->fia_slot = index % I915_TC_PORTS_PER_FIA;
		p->mode = I915_TC_MODE_NONE;
		p->cold_held = 0;
		p->links = 0u;
		p->connects = 0u;
		p->connect_failures = 0u;
		p->disconnects = 0u;
	}

	/* The ports may be declared from here on. */
	tc->live = 1;
}

/*
 * Declares a Type-C port the VBT names, and whether a connector is wired to
 * it (legacy) rather than a USB-C receptacle.
 */
void
drv_i915_tc_declare(
	struct i915_tc *tc,
	unsigned port,
	int legacy)
{
	struct i915_tc_port *p;

	/* Ignores a port this display does not have. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;

	/* The port is used from here on. */
	p->present = 1;
	p->legacy = legacy;
}

/*
 * Reads how the firmware left every declared Type-C port and settles it.
 *
 * A port whose DDI buffer the firmware left enabled is driving its output:
 * the display keeps the PHY and counts one link for that output, which the
 * output's disable gives back.  Any other port the firmware left held is
 * given back.  Each port's state is logged.
 */
void
drv_i915_tc_readout(
	struct i915_tc *tc)
{
	struct i915_tc_port *p;
	unsigned index;

	/* A display without Type-C ports has nothing to read. */
	if (!tc->live)
		return;

	/* Reads and settles each declared port. */
	for (index = 0u; index < I915_TC_PORTS; index++) {
		/* Skips a port the VBT does not declare. */
		p = &tc->port[index];
		if (!p->present)
			continue;

		/* Reads and settles the port under its lock. */
		tc->env.lock(tc->env.ctx, index);

		tc_readout_port(tc, p);

		tc->env.unlock(tc->env.ctx, index);

		/* Logs what the readout found and left. */
		drv_i915_tc_log_state(tc, index, "readout");
	}
}

/*
 * Gives every Type-C port back to the Type-C subsystem when the driver
 * stops: each port's links are dropped (the outputs were stopped before),
 * its TC cold block and its PHY ownership are given back, and it is
 * retired, so a step already on its way takes nothing.  The display is
 * not live afterwards.  Each port's state is logged.
 */
void
drv_i915_tc_stop(
	struct i915_tc *tc)
{
	struct i915_tc_port *p;
	unsigned index;

	/* A display without Type-C ports, or one already stopped, has nothing to give back. */
	if (!tc->live)
		return;

	/* Gives each declared port back. */
	for (index = 0u; index < I915_TC_PORTS; index++) {
		/* Skips a port the VBT does not declare. */
		p = &tc->port[index];
		if (!p->present)
			continue;

		/* Gives the PHY back and retires the port under its lock. */
		tc->env.lock(tc->env.ctx, index);

		/* An output that did not put its link back no longer drives the port. */
		if (p->links != 0u)
			tc->env.log(tc->env.ctx, "i915: TC%u: stop: %u link(s) still held: given back\n", index + 1u, p->links);
		p->links = 0u;

		/* The PHY and the TC cold block go back to the Type-C subsystem. */
		tc_disconnect(tc, p);

		/*
		 * Retired: present is what every step checks under this lock, so one
		 * that already found the port takes nothing more.
		 */
		p->present = 0;

		tc->env.unlock(tc->env.ctx, index);

		/* Logs what the port was left in. */
		tc->env.log(tc->env.ctx, "i915: TC%u: stop: PHY given back (mode %s, cold %d, connects %u, disconnects %u)\n", index + 1u, drv_i915_tc_mode_name(p->mode), p->cold_held, p->connects, p->disconnects);
	}

	/* No port is answered for from here on. */
	tc->live = 0;
}

/*
 * Reads what the hardware says is plugged into a Type-C port: a mask of
 * I915_TC_LIVE_DP_ALT, I915_TC_LIVE_TBT and I915_TC_LIVE_LEGACY.
 *
 * The display core is powered for the read.  An undeclared port, or one
 * whose power did not come, reads nothing plugged in.
 */
uint32_t
drv_i915_tc_live_status(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	uint32_t live;
	int error;

	/* An undeclared port has nothing plugged in. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return 0u;
	if (!p->present)
		return 0u;

	/* Reads the hotplug status with the display core powered. */
	error = tc_power_get(tc, I915_TC_POWER_CORE, port);
	if (error != 0)
		return 0u;

	live = tc_live_status_raw(tc, port);

	tc_power_put(tc, I915_TC_POWER_CORE, port);

	/* Succeeded: what is plugged in. */
	return live;
}

/*
 * Tells whether something the display can use is plugged into a Type-C
 * port: 1 or 0.
 *
 * A port the display holds counts only what it is held for, so a DP-alt
 * port does not count a Thunderbolt partner; a port held in no mode counts
 * anything plugged in.
 */
int
drv_i915_tc_connected(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	int connected;

	/* An undeclared port has nothing connected. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Reads the live status and the mode together under the port's lock. */
	tc->env.lock(tc->env.ctx, port);

	connected = tc_connected_locked(tc, p);

	tc->env.unlock(tc->env.ctx, port);

	/* Succeeded: reports whether the port is connected. */
	return connected;
}

/*
 * Tells whether something the display can use is plugged into a Type-C
 * port whose lock the caller holds (the Linux
 * intel_tc_port_connected_locked()): 1 or 0, as drv_i915_tc_connected().
 */
int
drv_i915_tc_connected_locked(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	int connected;

	/* An undeclared port has nothing connected. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Compares the live status with the mode the lock holds the port in. */
	connected = tc_connected_locked(tc, p);

	/* Succeeded: reports whether the port is connected. */
	return connected;
}

/*
 * Tells whether the link of a Type-C port must be reset (the Linux
 * intel_tc_port_link_needs_reset()): 1 when an output holds the port in
 * DP-alt mode and what is plugged in now asks for another mode (the
 * partner went, or came back as something else); 0 otherwise.
 */
int
drv_i915_tc_link_needs_reset(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	uint32_t live;
	enum i915_tc_mode target;
	int needs;

	/* An undeclared port has no link. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Compares the mode the links hold with what is plugged in, under the port's lock. */
	tc->env.lock(tc->env.ctx, port);

	/* Only a link held in DP-alt mode can lose its partner; what is plugged in now asks for its own mode. */
	needs = 0;
	if (p->links != 0u && p->mode == I915_TC_MODE_DP_ALT) {
		live = drv_i915_tc_live_status(tc, port);
		target = tc_target_mode(p, live);
		if (target != p->mode)
			needs = 1;
	}

	tc->env.unlock(tc->env.ctx, port);

	/* Succeeded: reports whether the link must be reset. */
	return needs;
}

/*
 * Locks a Type-C port for a step that uses its PHY, and brings its mode up
 * to date first when no link fixes it.
 *
 * required_lanes is what the step needs of a DP-alt partner.  Returns the
 * mode the port is held in; I915_TC_MODE_TBT means the display could not
 * take the PHY and must not drive the port.  Every lock is matched by
 * drv_i915_tc_unlock().
 */
enum i915_tc_mode
drv_i915_tc_lock(
	struct i915_tc *tc,
	unsigned port,
	int required_lanes)
{
	struct i915_tc_port *p;

	/* An undeclared port is never held. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return I915_TC_MODE_NONE;

	/* Takes the port's lock for the caller's step. */
	tc->env.lock(tc->env.ctx, port);

	/* A declared port without links follows what is plugged in now. */
	if (p->present && p->links == 0u)
		tc_update_mode(tc, p, required_lanes);

	/* Succeeded: the mode the port is held in. */
	return p->mode;
}

/*
 * Unlocks a Type-C port, giving its PHY back when no link uses it.
 */
void
drv_i915_tc_unlock(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;

	/* An undeclared port was never locked. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;

	/* A port no link uses gives its PHY back before the lock goes. */
	if (p->links == 0u)
		tc_disconnect(tc, p);

	tc->env.unlock(tc->env.ctx, port);
}

/*
 * Adds an output's link to a Type-C port: the port takes its PHY for the
 * required lanes if it does not hold it, and keeps its mode until the link
 * is put back.
 */
void
drv_i915_tc_get_link(
	struct i915_tc *tc,
	unsigned port,
	int required_lanes)
{
	struct i915_tc_port *p;

	/* An undeclared port has no links. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;

	/*
	 * The link fixes the mode the lock brought up to date.  links is what
	 * keeps the PHY held: the unlock gives it back only while it is zero.
	 */
	(void)drv_i915_tc_lock(tc, port, required_lanes);
	p->links++;
	drv_i915_tc_unlock(tc, port);
}

/*
 * Puts an output's link back; the last one gives the port's PHY back at
 * once.
 */
void
drv_i915_tc_put_link(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;

	/* An undeclared port has no links. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;

	/* Drops the link under the lock; the unlock gives the PHY back when it was the last. */
	tc->env.lock(tc->env.ctx, port);

	/* A link never taken is reported, not counted below zero. */
	if (p->links == 0u) {
		tc->env.log(tc->env.ctx, "i915: TC%u: a link was put back that was never taken\n", port + 1u);
	} else {
		p->links--;
	}

	drv_i915_tc_unlock(tc, port);
}

/*
 * Reports the mode a Type-C port is held in, as the port's lock last left
 * it.
 */
enum i915_tc_mode
drv_i915_tc_mode(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	enum i915_tc_mode mode;

	/* An undeclared port is held in no mode. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return I915_TC_MODE_NONE;

	/* Samples the mode under the port's lock. */
	tc->env.lock(tc->env.ctx, port);

	mode = p->mode;

	tc->env.unlock(tc->env.ctx, port);

	/* Succeeded: the mode. */
	return mode;
}

/*
 * Reports how many lanes a Type-C port offers its output.
 *
 * Only a DP-alt partner restricts the lanes, by the lanes the FIA assigned
 * to DisplayPort; any other mode offers all four.  The caller holds the
 * port's lock with the port in its mode (TC cold blocked).
 */
int
drv_i915_tc_max_lanes(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	unsigned lane_mask;
	int lanes;

	/* An undeclared port is not restricted. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return I915_TC_FULL_LANES;

	/* Only DP-alt restricts the lanes. */
	if (p->mode != I915_TC_MODE_DP_ALT)
		return I915_TC_FULL_LANES;

	/* Counts the lanes the FIA assigned. */
	lane_mask = tc_lane_mask(tc, p);
	lanes = tc_lanes_of_mask(lane_mask);

	/* Succeeded: the lanes the partner offers. */
	return lanes;
}

/*
 * Reads the DisplayPort pin assignment the FIA records for a Type-C port
 * (3 is pin C, 4 is D, 5 is E; 0 when none is recorded).
 */
unsigned
drv_i915_tc_pin_assignment(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_port *p;
	uint32_t pins;

	/* An undeclared port has no pin assignment. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return 0u;

	/* Takes the port's slot of the FIA's pin assignment. */
	pins = tc_read(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXPA1);
	pins >>= p->fia_slot * I915_TC_FIA_PIN_SHIFT;
	pins &= I915_TC_FIA_PIN_MASK;

	/* Succeeded: the pin assignment. */
	return pins;
}

/*
 * Tells the FIA which of a Type-C port's lanes carry the DisplayPort main
 * link: one, two or four, counted from the far end when the lanes are
 * reversed (which only a legacy port's are).
 *
 * The caller holds a link to the port, so the PHY is held and TC cold is
 * blocked.  Another lane count is logged and leaves the port with no
 * lanes, as it was asked for none the FIA has.
 */
void
drv_i915_tc_set_fia_lane_count(
	struct i915_tc *tc,
	unsigned port,
	int required_lanes,
	int lane_reversal)
{
	struct i915_tc_port *p;
	uint32_t lanes;
	uint32_t value;
	unsigned shift;

	/* An undeclared port has no FIA slot. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;

	/* The lanes the link needs, from the near end or from the far end. */
	lanes = 0u;
	if (required_lanes == 1) {
		if (lane_reversal) {
			lanes = I915_TC_FIA_MLE_ONE_REVERSED;
		} else {
			lanes = I915_TC_FIA_MLE_ONE;
		}
	} else if (required_lanes == 2) {
		if (lane_reversal) {
			lanes = I915_TC_FIA_MLE_TWO_REVERSED;
		} else {
			lanes = I915_TC_FIA_MLE_TWO;
		}
	} else if (required_lanes == 4) {
		lanes = I915_TC_FIA_MLE_FOUR;
	} else {
		tc->env.log(tc->env.ctx, "i915: TC%u: no FIA lanes for a %d-lane link\n", port + 1u, required_lanes);
	}

	/* Replaces the port's slot of the FIA's main link lanes. */
	shift = p->fia_slot * I915_TC_FIA_MLE_SHIFT;
	value = tc_read(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXDPMLE1);
	value &= ~(I915_TC_FIA_MLE_MASK << shift);
	value |= lanes << shift;
	tc_write(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXDPMLE1, value);
}

/*
 * Reads what a Type-C port has of DisplayPort for the Type-C connector
 * layer, under the port's lock: the DP-alt live status, and the FIA's pin
 * assignment and lanes when the display holds the port in DP-alt (TC cold
 * is blocked then, so the FIA answers).  An undeclared port has nothing.
 */
void
drv_i915_tc_dp_sample(
	struct i915_tc *tc,
	unsigned port,
	struct i915_tc_dp_sample *sample)
{
	struct i915_tc_port *p;
	uint32_t live;
	unsigned lane_mask;
	unsigned pins;
	int held;

	/* Nothing, until a declared port says otherwise. */
	sample->hpd = 0;
	sample->pin = 0u;
	sample->lanes = 0;

	/* An undeclared port has nothing plugged in. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;
	if (!p->present)
		return;

	/* Reads the live status, and the FIA while the port is held in DP-alt. */
	tc->env.lock(tc->env.ctx, port);

	live = drv_i915_tc_live_status(tc, port);
	held = 0;
	pins = 0u;
	lane_mask = 0u;
	if (p->mode == I915_TC_MODE_DP_ALT) {
		held = 1;
		pins = drv_i915_tc_pin_assignment(tc, port);
		lane_mask = tc_lane_mask(tc, p);
	}

	tc->env.unlock(tc->env.ctx, port);

	/* A live DP-alt partner raises the hot plug detect. */
	if ((live & I915_TC_LIVE_DP_ALT) != 0u)
		sample->hpd = 1;

	/* The FIA's pin assignment and lanes count only while the port was held in DP-alt. */
	if (held) {
		sample->pin = pins;
		sample->lanes = tc_lanes_of_mask(lane_mask);
	}
}

/*
 * Logs a Type-C port's state: its mode, links, live status, and the raw
 * registers a reader needs to tell what the Type-C subsystem and the FIA
 * reported.
 *
 * The FIA's lane mask is logged as read: with a pin D partner it is the
 * only trace of the plug's orientation, which is recorded and not used.
 */
void
drv_i915_tc_log_state(
	struct i915_tc *tc,
	unsigned port,
	const char *why)
{
	struct i915_tc_port *p;
	uint32_t live;
	uint32_t tcss;
	uint32_t lanes;
	uint32_t pins;
	uint32_t buffer;
	int error;

	/* An undeclared port has no state. */
	p = tc_port_of(tc, port);
	if (p == NULL)
		return;
	if (!p->present)
		return;

	/* Reads the registers with the display core and the port's lanes powered. */
	error = tc_power_get(tc, I915_TC_POWER_CORE, port);
	if (error != 0)
		return;

	/* The port's lanes, for its DDI buffer control. */
	error = tc_power_get(tc, I915_TC_POWER_PORT, port);
	if (error != 0) {
		tc_power_put(tc, I915_TC_POWER_CORE, port);
		return;
	}

	/* The live status, the Type-C subsystem's status, the FIA's lanes and pins, and the buffer control. */
	live = tc_live_status_raw(tc, port);
	tcss = tc_read(tc, I915_TC_TCSS_DDI_STATUS + I915_TC_TCSS_DDI_STATUS_STRIDE * p->index);
	lanes = tc_read(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXDPSP);
	pins = tc_read(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXPA1);
	buffer = tc_read(tc, tc_ddi_buf_ctl(p));

	/* The power was needed for the reads only. */
	tc_power_put(tc, I915_TC_POWER_PORT, port);
	tc_power_put(tc, I915_TC_POWER_CORE, port);

	/* Logs one line a reader can compare with a known good machine. */
	tc->env.log(tc->env.ctx,
		"i915: TC%u %s: mode=%s links=%u legacy=%d cold=%d live=0x%x | TCSS_DDI_STATUS=0x%08x FIA%u.%u DFLEXDPSP=0x%08x DFLEXPA1=0x%08x DDI_BUF_CTL=0x%08x\n",
		port + 1u,
		why,
		drv_i915_tc_mode_name(p->mode),
		p->links,
		p->legacy,
		p->cold_held,
		(unsigned)live,
		(unsigned)tcss,
		p->fia + 1u,
		p->fia_slot,
		(unsigned)lanes,
		(unsigned)pins,
		(unsigned)buffer);
}

/*
 * Names a Type-C mode for the log.
 */
const char *
drv_i915_tc_mode_name(
	enum i915_tc_mode mode)
{
	/* Picks the name of the mode. */
	switch (mode) {
	case I915_TC_MODE_NONE:
		return "none";
	case I915_TC_MODE_TBT:
		return "tbt";
	case I915_TC_MODE_DP_ALT:
		return "dp-alt";
	case I915_TC_MODE_LEGACY:
		return "legacy";
	default:
		break;
	}

	/* Succeeded: a value outside the enumeration. */
	return "?";
}

/* Finds a port of a live display, or NULL. */
static struct i915_tc_port *
tc_port_of(
	struct i915_tc *tc,
	unsigned port)
{
	/* A display without Type-C ports has none. */
	if (!tc->live)
		return NULL;

	/* A number past the last port names none. */
	if (port >= I915_TC_PORTS)
		return NULL;

	/* Succeeded: the port. */
	return &tc->port[port];
}

/* Reads a display register. */
static uint32_t
tc_read(
	struct i915_tc *tc,
	uint32_t reg)
{
	uint32_t value;

	/* Reads through the environment. */
	value = tc->env.read32(tc->env.ctx, reg);

	/* Succeeded: the value. */
	return value;
}

/* Writes a display register. */
static void
tc_write(
	struct i915_tc *tc,
	uint32_t reg,
	uint32_t value)
{
	/* Writes through the environment. */
	tc->env.write32(tc->env.ctx, reg, value);
}

/* Takes a port's power; 0, or the environment's error. */
static int
tc_power_get(
	struct i915_tc *tc,
	enum i915_tc_power power,
	unsigned port)
{
	int error;

	/* Asks the environment for the power. */
	error = tc->env.power_get(tc->env.ctx, power, port);
	if (error != 0)
		return error;

	/* Succeeded: the power is on. */
	return 0;
}

/* Gives a port's power back. */
static void
tc_power_put(
	struct i915_tc *tc,
	enum i915_tc_power power,
	unsigned port)
{
	/* Hands the power back to the environment. */
	tc->env.power_put(tc->env.ctx, power, port);
}

/* Names the register block of the FIA that serves a port. */
static uint32_t
tc_fia_base(
	const struct i915_tc_port *p)
{
	/* FIA1 serves the first two ports; FIA2 and FIA3 are apart from it. */
	if (p->fia == 0u)
		return I915_TC_FIA1_BASE;
	if (p->fia == 1u)
		return I915_TC_FIA2_BASE;

	/* Succeeded: the third FIA. */
	return I915_TC_FIA3_BASE;
}

/* Names a port's DDI buffer control. */
static uint32_t
tc_ddi_buf_ctl(
	const struct i915_tc_port *p)
{
	/* Type-C port n is DDI port D + n. */
	return I915_TC_DDI_BUF_CTL_A + I915_TC_DDI_BUF_CTL_STRIDE * (I915_TC_FIRST_DDI_PORT + p->index);
}

/* Reads a port's live status; the caller powers the display core. */
static uint32_t
tc_live_status_raw(
	struct i915_tc *tc,
	unsigned port)
{
	uint32_t de_isr;
	uint32_t sde_isr;
	uint32_t live;

	/* Reads the display engine's and the south display's hotplug status. */
	de_isr = tc_read(tc, I915_TC_DE_HPD_ISR);
	sde_isr = tc_read(tc, I915_TC_SDEISR);

	/* The display engine's Type-C bit reports a DP-alt partner. */
	live = 0u;
	if ((de_isr & (1u << (I915_TC_DE_HPD_DP_ALT_SHIFT + port))) != 0u)
		live |= I915_TC_LIVE_DP_ALT;

	/* The display engine's Thunderbolt bit reports a Thunderbolt partner. */
	if ((de_isr & (1u << (I915_TC_DE_HPD_TBT_SHIFT + port))) != 0u)
		live |= I915_TC_LIVE_TBT;

	/* The south display's Type-C bit reports a connector wired to the port. */
	if ((sde_isr & (1u << (I915_TC_SDE_LEGACY_SHIFT + port))) != 0u)
		live |= I915_TC_LIVE_LEGACY;

	/* Succeeded: what is plugged in. */
	return live;
}

/* Tells whether the Type-C subsystem lets the display take a port's PHY: 1 or 0. */
static int
tc_is_ready(
	struct i915_tc *tc,
	const struct i915_tc_port *p)
{
	uint32_t status;

	/* Reads the Type-C subsystem's status of the port. */
	status = tc_read(tc, I915_TC_TCSS_DDI_STATUS + I915_TC_TCSS_DDI_STATUS_STRIDE * p->index);

	/* A subsystem in TC cold reads all-ones and is not ready. */
	if (status == I915_TC_REG_DEAD)
		return 0;

	/* The ready bit decides. */
	if ((status & I915_TC_TCSS_READY) == 0u)
		return 0;

	/* Succeeded: the PHY may be taken. */
	return 1;
}

/* Tells whether the display owns a port's PHY: 1 or 0.  The caller powers the port. */
static int
tc_is_owned(
	struct i915_tc *tc,
	const struct i915_tc_port *p)
{
	uint32_t buffer;

	/* Reads the ownership bit of the port's DDI buffer control. */
	buffer = tc_read(tc, tc_ddi_buf_ctl(p));
	if ((buffer & I915_TC_DDI_BUF_PHY_OWNERSHIP) == 0u)
		return 0;

	/* Succeeded: the display owns the PHY. */
	return 1;
}

/* Takes or gives back the ownership of a port's PHY.  The caller powers the port. */
static void
tc_set_ownership(
	struct i915_tc *tc,
	const struct i915_tc_port *p,
	int take)
{
	uint32_t buffer;

	/* Reads the port's DDI buffer control; only the ownership bit changes. */
	buffer = tc_read(tc, tc_ddi_buf_ctl(p));

	/* Sets the bit to take the PHY, clears it to give it back. */
	if (take) {
		buffer |= I915_TC_DDI_BUF_PHY_OWNERSHIP;
	} else {
		buffer &= ~I915_TC_DDI_BUF_PHY_OWNERSHIP;
	}

	/* Writes the control back. */
	tc_write(tc, tc_ddi_buf_ctl(p), buffer);
}

/* Reads the 4-bit mask of the lanes the FIA assigned to DisplayPort on a port. */
static unsigned
tc_lane_mask(
	struct i915_tc *tc,
	const struct i915_tc_port *p)
{
	uint32_t assignment;

	/* Takes the port's slot of the FIA's lane assignment. */
	assignment = tc_read(tc, tc_fia_base(p) + I915_TC_FIA_DFLEXDPSP);
	assignment >>= p->fia_slot * I915_TC_FIA_LANE_SHIFT;
	assignment &= I915_TC_FIA_LANE_MASK;

	/* Succeeded: the lane mask. */
	return (unsigned)assignment;
}

/*
 * Counts the lanes of an FIA lane mask: one lane, a pair (lanes 0-1 or
 * 2-3), or all four.  Any other mask counts as one lane, the least that
 * still lets the link train.
 */
static int
tc_lanes_of_mask(
	unsigned lane_mask)
{
	/* Picks the count of the mask. */
	switch (lane_mask) {
	case 0x3u:
	case 0xcu:
		return 2;
	case 0xfu:
		return 4;
	default:
		break;
	}

	/* Succeeded: a single lane, or a mask that offers no more. */
	return 1;
}

/*
 * The mode a live status asks for: the legacy connector over a DP-alt
 * partner over a Thunderbolt partner, or none when nothing is plugged in.
 */
static enum i915_tc_mode
tc_mode_of_live(
	uint32_t live)
{
	/* A legacy connector wins, then DP-alt, then Thunderbolt. */
	if ((live & I915_TC_LIVE_LEGACY) != 0u)
		return I915_TC_MODE_LEGACY;
	if ((live & I915_TC_LIVE_DP_ALT) != 0u)
		return I915_TC_MODE_DP_ALT;
	if ((live & I915_TC_LIVE_TBT) != 0u)
		return I915_TC_MODE_TBT;

	/* Succeeded: nothing plugged in asks for no mode. */
	return I915_TC_MODE_NONE;
}

/* The mode of a port with nothing plugged in: a wired connector stays legacy, a receptacle is left to Thunderbolt. */
static enum i915_tc_mode
tc_default_mode(
	const struct i915_tc_port *p)
{
	/* A legacy port is always the display's. */
	if (p->legacy)
		return I915_TC_MODE_LEGACY;

	/* Succeeded: a receptacle the display does not hold. */
	return I915_TC_MODE_TBT;
}

/* The mode a port should be held in for a live status. */
static enum i915_tc_mode
tc_target_mode(
	const struct i915_tc_port *p,
	uint32_t live)
{
	enum i915_tc_mode mode;

	/* What is plugged in decides; nothing plugged in leaves the default. */
	mode = tc_mode_of_live(live);
	if (mode == I915_TC_MODE_NONE)
		mode = tc_default_mode(p);

	/* Succeeded: the target mode. */
	return mode;
}

/*
 * Corrects the VBT's legacy flag of a port when the live status reports a
 * single kind of partner that contradicts it.
 */
static void
tc_fix_legacy(
	struct i915_tc *tc,
	struct i915_tc_port *p,
	uint32_t live)
{
	int live_legacy;

	/* Only a single live bit says what is wired to the port. */
	if (live != I915_TC_LIVE_DP_ALT &&
	    live != I915_TC_LIVE_TBT &&
	    live != I915_TC_LIVE_LEGACY)
		return;

	/* A live legacy bit means a wired connector; any other means a receptacle. */
	live_legacy = 0;
	if (live == I915_TC_LIVE_LEGACY)
		live_legacy = 1;

	/* The flag agrees with the live status. */
	if (live_legacy == p->legacy)
		return;

	/* Trusts the live status over the VBT. */
	tc->env.log(tc->env.ctx, "i915: TC%u: live status 0x%x contradicts the VBT's legacy flag %d: the flag is corrected\n", p->index + 1u, (unsigned)live, p->legacy);
	p->legacy = live_legacy;
}

/*
 * Waits for a legacy port's PHY to become ready: the Type-C subsystem's
 * firmware brings it up at boot and resume whether or not a connector is
 * plugged in.  The wait sleeps between the reads, so the CPU serves other
 * threads for the up to half a second it may last; the time is measured on
 * the clock, as a sleep may last longer than asked.  A PHY that does not
 * come is logged.
 */
static void
tc_wait_ready(
	struct i915_tc *tc,
	const struct i915_tc_port *p)
{
	uint64_t start_us;
	uint64_t now_us;
	int ready;
	int clock_error;
	int sleep_error;

	/* The time the wait starts at; without a clock the ready bit is read once. */
	clock_error = tc->env.now_us(tc->env.ctx, &start_us);
	if (clock_error != 0) {
		ready = tc_is_ready(tc, p);
		if (!ready)
			tc->env.log(tc->env.ctx, "i915: TC%u: the legacy port's PHY is not ready and the clock failed (error %d)\n", p->index + 1u, clock_error);
		return;
	}

	/* Polls the ready bit, sleeping between the reads, until it comes or the time is up. */
	for (;;) {
		/* Reads the ready bit; the wait is over once it is set. */
		ready = tc_is_ready(tc, p);
		if (ready)
			return;

		/* Reads the clock; a failed time base ends the wait. */
		clock_error = tc->env.now_us(tc->env.ctx, &now_us);
		if (clock_error != 0)
			break;

		/* The time is up: the PHY did not come. */
		if (now_us - start_us >= I915_TC_READY_TIMEOUT_US)
			break;

		/* Sleeps before the next read; a failed time base ends the wait. */
		sleep_error = tc->env.sleep_us(tc->env.ctx, I915_TC_READY_POLL_US);
		if (sleep_error != 0)
			break;
	}

	/* Reports a legacy PHY that never became ready. */
	tc->env.log(tc->env.ctx, "i915: TC%u: timeout waiting for the legacy port's PHY to become ready\n", p->index + 1u);
}

/*
 * Reads the mode a port's hardware is in now.  The caller powers the port.
 *
 * The display holds the port only when the PHY is both ready and owned;
 * it then holds it for what is plugged in, or for the port's kind when
 * nothing is.  A PHY not held is left to Thunderbolt, unless a legacy
 * connector, which only the display drives, is what is plugged in.
 */
static enum i915_tc_mode
tc_current_mode(
	struct i915_tc *tc,
	struct i915_tc_port *p)
{
	enum i915_tc_mode live_mode;
	enum i915_tc_mode mode;
	uint32_t live;
	int ready;
	int owned;

	/* Reads what is plugged in. */
	live = drv_i915_tc_live_status(tc, p->index);
	live_mode = tc_mode_of_live(live);

	/* A legacy port's PHY is brought up by the firmware: waits for it. */
	if (p->legacy)
		tc_wait_ready(tc, p);

	/* Reads whether the PHY is ready and whether the display owns it. */
	ready = tc_is_ready(tc, p);
	owned = tc_is_owned(tc, p);

	/* An owned PHY that is not ready is a hardware contradiction: it counts as not held. */
	if (owned && !ready)
		tc->env.log(tc->env.ctx, "i915: TC%u: the PHY is owned but not ready\n", p->index + 1u);

	/* The PHY the display holds: it is held for what is plugged in, or for the port's kind. */
	if (ready && owned) {
		/* A legacy connector or a DP-alt partner names the mode; nothing plugged in leaves the port's kind. */
		if (live_mode == I915_TC_MODE_LEGACY || live_mode == I915_TC_MODE_DP_ALT) {
			mode = live_mode;
		} else if (p->legacy) {
			mode = I915_TC_MODE_LEGACY;
		} else {
			mode = I915_TC_MODE_DP_ALT;
		}
	} else if (live_mode == I915_TC_MODE_LEGACY) {
		/* A legacy connector the display does not hold: nothing holds it. */
		mode = I915_TC_MODE_NONE;
	} else if (live_mode == I915_TC_MODE_NONE && p->legacy) {
		/* A legacy port with nothing plugged in: nothing holds it. */
		mode = I915_TC_MODE_NONE;
	} else {
		/* A receptacle the display does not hold is left to Thunderbolt. */
		mode = I915_TC_MODE_TBT;
	}

	/* Logs what the mode was read from. */
	tc->env.log(tc->env.ctx, "i915: TC%u: PHY mode %s (ready %d, owned %d, live %s)\n", p->index + 1u, drv_i915_tc_mode_name(mode), ready, owned, drv_i915_tc_mode_name(live_mode));

	/* Succeeded: the mode. */
	return mode;
}

/* Blocks TC cold for a port the display holds; 0, or the power's error. */
static int
tc_block_cold(
	struct i915_tc *tc,
	struct i915_tc_port *p)
{
	int error;

	/* A port that already blocks it holds nothing more. */
	if (p->cold_held)
		return 0;

	/* Takes the port's AUX_USBC power, which keeps the Type-C subsystem out of TC cold. */
	error = tc_power_get(tc, I915_TC_POWER_COLD, p->index);
	if (error != 0)
		return error;

	/* cold_held is what the disconnect gives back. */
	p->cold_held = 1;

	/* Succeeded: TC cold is blocked. */
	return 0;
}

/* Lets the Type-C subsystem enter TC cold again. */
static void
tc_unblock_cold(
	struct i915_tc *tc,
	struct i915_tc_port *p)
{
	/* A port that does not block it has nothing to give back. */
	if (!p->cold_held)
		return;

	/* Gives the AUX_USBC power back. */
	tc_power_put(tc, I915_TC_POWER_COLD, p->index);
	p->cold_held = 0;
}

/*
 * Checks that a port just taken can serve the mode it was taken for: 1 or
 * 0.  A legacy port always can; a DP-alt partner must still be plugged in
 * and offer the required lanes.
 */
static int
tc_verify_mode(
	struct i915_tc *tc,
	struct i915_tc_port *p,
	int required_lanes)
{
	uint32_t live;
	int lanes;

	/* A wired connector has all four lanes. */
	if (p->mode == I915_TC_MODE_LEGACY)
		return 1;

	/* The partner may have gone between the live read and the connect. */
	live = drv_i915_tc_live_status(tc, p->index);
	if ((live & I915_TC_LIVE_DP_ALT) == 0u) {
		tc->env.log(tc->env.ctx, "i915: TC%u: the DP-alt partner went away during the connect\n", p->index + 1u);
		return 0;
	}

	/* The partner must offer the lanes the output needs. */
	lanes = drv_i915_tc_max_lanes(tc, p->index);
	if (lanes < required_lanes) {
		tc->env.log(tc->env.ctx, "i915: TC%u: the partner offers %d lanes, %d are required\n", p->index + 1u, lanes, required_lanes);
		return 0;
	}

	/* Succeeded: the port serves the mode. */
	return 1;
}

/*
 * Takes a port's PHY for its mode: 1 when the port now serves the mode, 0
 * when it was refused and everything taken was given back.
 *
 * A Thunderbolt mode takes nothing: the display does not own the PHY.
 */
static int
tc_take_phy(
	struct i915_tc *tc,
	struct i915_tc_port *p,
	int required_lanes)
{
	int error;
	int ready;
	int verified;

	/* The display does not take a PHY left to Thunderbolt. */
	if (p->mode == I915_TC_MODE_TBT)
		return 1;

	/* Powers the port's DDI lanes for the ownership bit. */
	error = tc_power_get(tc, I915_TC_POWER_PORT, p->index);
	if (error != 0)
		return 0;

	/* Takes the ownership. */
	tc_set_ownership(tc, p, 1);

	/* The Type-C subsystem must let the display have the PHY. */
	ready = tc_is_ready(tc, p);
	if (!ready) {
		tc->env.log(tc->env.ctx, "i915: TC%u: the PHY is not ready\n", p->index + 1u);
		tc_set_ownership(tc, p, 0);
		tc_power_put(tc, I915_TC_POWER_PORT, p->index);
		return 0;
	}

	/* Keeps the Type-C subsystem out of TC cold. */
	error = tc_block_cold(tc, p);
	if (error != 0) {
		tc_set_ownership(tc, p, 0);
		tc_power_put(tc, I915_TC_POWER_PORT, p->index);
		return 0;
	}

	/* The port must serve the mode it was taken for. */
	verified = tc_verify_mode(tc, p, required_lanes);
	if (!verified) {
		tc_unblock_cold(tc, p);
		tc_set_ownership(tc, p, 0);
		tc_power_put(tc, I915_TC_POWER_PORT, p->index);
		return 0;
	}

	/* The DDI lanes' power was needed for the ownership step only. */
	tc_power_put(tc, I915_TC_POWER_PORT, p->index);

	/* Succeeded: the display holds the PHY. */
	return 1;
}

/*
 * Brings a port that holds nothing into the mode its live status asks for;
 * a mode that cannot be served falls back to the port's default.
 */
static void
tc_connect(
	struct i915_tc *tc,
	struct i915_tc_port *p,
	int required_lanes)
{
	uint32_t live;
	enum i915_tc_mode fallback;
	int taken;

	/* Reads what is plugged in and corrects the legacy flag by it. */
	live = drv_i915_tc_live_status(tc, p->index);
	tc_fix_legacy(tc, p, live);

	/* Tries the mode the live status asks for. */
	p->mode = tc_target_mode(p, live);
	taken = tc_take_phy(tc, p, required_lanes);
	if (taken) {
		p->connects++;
		return;
	}

	/* Counts the refusal. */
	p->connect_failures++;

	/* A refused default mode leaves the port holding nothing. */
	fallback = tc_default_mode(p);
	if (p->mode == fallback) {
		p->mode = I915_TC_MODE_NONE;
		return;
	}

	/* Tries the default mode; a refusal of it too leaves the port holding nothing. */
	p->mode = fallback;
	taken = tc_take_phy(tc, p, required_lanes);
	if (!taken) {
		p->connect_failures++;
		p->mode = I915_TC_MODE_NONE;
		return;
	}

	/* Succeeded: the port holds its default mode. */
	p->connects++;
}

/*
 * Gives a port's PHY back: TC cold unblocked and the ownership cleared.  A
 * port that holds nothing stays so.
 */
static void
tc_disconnect(
	struct i915_tc *tc,
	struct i915_tc_port *p)
{
	int error;
	int owned_mode;

	/* A port that holds nothing has nothing to give back. */
	if (p->mode == I915_TC_MODE_NONE)
		return;

	/* Only a DP-alt or legacy port owns the PHY. */
	owned_mode = 0;
	if (p->mode == I915_TC_MODE_DP_ALT || p->mode == I915_TC_MODE_LEGACY)
		owned_mode = 1;

	/* Unblocks TC cold. */
	tc_unblock_cold(tc, p);

	/* Gives the ownership back with the port's DDI lanes powered. */
	if (owned_mode) {
		error = tc_power_get(tc, I915_TC_POWER_PORT, p->index);
		if (error == 0) {
			tc_set_ownership(tc, p, 0);
			tc_power_put(tc, I915_TC_POWER_PORT, p->index);
		} else {
			tc->env.log(tc->env.ctx, "i915: TC%u: the ownership was not given back: the port's power did not come (error %d)\n", p->index + 1u, error);
		}
	}

	/* The port holds nothing now. */
	p->mode = I915_TC_MODE_NONE;
	p->disconnects++;
}

/*
 * Brings a port without links to the mode its live status asks for:
 * nothing is done when it is already there.
 */
static void
tc_update_mode(
	struct i915_tc *tc,
	struct i915_tc_port *p,
	int required_lanes)
{
	uint32_t live;
	enum i915_tc_mode target;
	enum i915_tc_mode old_mode;

	/* The port already holds what is plugged in. */
	live = drv_i915_tc_live_status(tc, p->index);
	target = tc_target_mode(p, live);
	if (target == p->mode)
		return;

	/* Gives back what the port holds and takes the PHY for the new mode. */
	old_mode = p->mode;
	tc_disconnect(tc, p);
	tc_connect(tc, p, required_lanes);

	/* Logs the change. */
	tc->env.log(tc->env.ctx, "i915: TC%u: mode %s -> %s\n", p->index + 1u, drv_i915_tc_mode_name(old_mode), drv_i915_tc_mode_name(p->mode));
}

/*
 * Reads how the firmware left a port and settles it; the caller holds the
 * port's lock.
 *
 * A held PHY keeps TC cold blocked; one whose DDI buffer is enabled is the
 * firmware's output and counts one link.  Any other held PHY is given back.
 */
static void
tc_readout_port(
	struct i915_tc *tc,
	struct i915_tc_port *p)
{
	uint32_t buffer;
	int error;

	/* The port's DDI lanes are powered for the ownership and the buffer. */
	error = tc_power_get(tc, I915_TC_POWER_PORT, p->index);
	if (error != 0) {
		tc->env.log(tc->env.ctx, "i915: TC%u: readout skipped: the port's power did not come (error %d)\n", p->index + 1u, error);
		return;
	}

	/* Reads the mode the hardware is in. */
	p->mode = tc_current_mode(tc, p);

	/* A PHY the display holds keeps TC cold blocked; one that cannot is given back to the Type-C subsystem. */
	if (p->mode == I915_TC_MODE_DP_ALT || p->mode == I915_TC_MODE_LEGACY) {
		error = tc_block_cold(tc, p);
		if (error != 0) {
			tc_set_ownership(tc, p, 0);
			p->mode = I915_TC_MODE_NONE;
		}
	}

	/* An enabled DDI buffer on a held PHY is the firmware's output on the port: one link. */
	buffer = tc_read(tc, tc_ddi_buf_ctl(p));
	if ((buffer & I915_TC_DDI_BUF_ENABLE) != 0u && p->cold_held)
		p->links = 1u;

	/* The lanes' power was needed for the reads only. */
	tc_power_put(tc, I915_TC_POWER_PORT, p->index);

	/* A port nothing drives is given back. */
	if (p->links == 0u)
		tc_disconnect(tc, p);
}

/* Compares a port's live status with what its mode accepts; the caller holds the port's lock. */
static int
tc_connected_locked(
	struct i915_tc *tc,
	const struct i915_tc_port *p)
{
	uint32_t live;
	uint32_t accepted;

	/* Reads what is plugged in now. */
	live = drv_i915_tc_live_status(tc, p->index);

	/* The live bits the port's mode accepts: any of them while it is held in no mode. */
	accepted = I915_TC_LIVE_DP_ALT | I915_TC_LIVE_TBT | I915_TC_LIVE_LEGACY;
	if (p->mode == I915_TC_MODE_DP_ALT) {
		accepted = I915_TC_LIVE_DP_ALT;
	} else if (p->mode == I915_TC_MODE_TBT) {
		accepted = I915_TC_LIVE_TBT;
	} else if (p->mode == I915_TC_MODE_LEGACY) {
		accepted = I915_TC_LIVE_LEGACY;
	}

	/* Something the mode accepts is not live. */
	if ((live & accepted) == 0u)
		return 0;

	/* Succeeded: something the mode accepts is plugged in. */
	return 1;
}
