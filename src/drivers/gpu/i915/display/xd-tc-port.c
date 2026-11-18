/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C ports of the display 11 to 13 code (see xd-tc-port.h).
 */

#include "xd-io.h"
#include "xd-platform.h"
#include "xd-tc.h"
#include "xd-tc-port.h"
#include "../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How long a legacy port's PHY is waited for at a readout, and how long
 * the wait sleeps between two reads.
 *
 * Value referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_wait_for_ready).
 */
#define I915_XD_TC_PORT_READY_TIMEOUT_US	500000U
#define I915_XD_TC_PORT_READY_POLL_US		1000U

/* The lanes a port offers when nothing restricts them. */
#define I915_XD_TC_PORT_FULL_LANES		4U

/* What a register of a Type-C subsystem in TC cold reads. */
#define I915_XD_TC_PORT_DEAD			0xffffffffU

static struct i915_xd_tc_port *i915_xd_tc_port_of(struct i915_xd_tc_ports *ports, unsigned tc);
static uint64_t i915_xd_tc_port_cold_read(const struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p);
static uint64_t i915_xd_tc_port_probe_functions(const struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p);
static unsigned i915_xd_tc_port_live_raw(struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p);
static unsigned i915_xd_tc_port_mode_of_live(unsigned live);
static unsigned i915_xd_tc_port_default_mode(const struct i915_xd_tc_port *p);
static unsigned i915_xd_tc_port_target_mode(const struct i915_xd_tc_port *p, unsigned live);
static void i915_xd_tc_port_fix_legacy(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p, unsigned live);
static void i915_xd_tc_port_wait_ready(struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p, const struct i915_xd_tc_fia *fia);
static unsigned i915_xd_tc_port_current_mode(struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p, const struct i915_xd_tc_fia *fia);
static int i915_xd_tc_port_take(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p, unsigned mode, unsigned lanes);
static void i915_xd_tc_port_connect(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p, unsigned lanes);
static void i915_xd_tc_port_disconnect(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p);
static void i915_xd_tc_port_follow_live(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p, unsigned lanes);
static void i915_xd_tc_port_readout(struct i915_xd_tc_ports *ports, struct i915_xd_tc_port *p);
static int i915_xd_tc_port_live_matches(struct i915_xd_tc_ports *ports, const struct i915_xd_tc_port *p);
static unsigned i915_xd_tc_port_fia_number(const struct i915_xd_tc_fia *fia);
static void i915_xd_tc_port_settle_all(struct i915_xd_tc_ports *ports, const char *why);

/*
 * Binds the device to the Type-C ports of a display.
 *
 * Every port starts undeclared and held in no mode.
 */
void
drv_i915_xd_tc_ports_init(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_tc_power *power,
	const struct i915_xd_tc_port_env *env)
{
	struct i915_xd_tc_port *p;
	unsigned index;

	/* Binds the device. */
	ports->io = io;
	ports->platform = platform;
	ports->power = *power;
	ports->env = *env;

	/* Prepares every port: undeclared, holding nothing. */
	for (index = 0U; index < I915_XD_TC_PORTS; index++) {
		p = &ports->port[index];
		p->index = index;
		p->present = 0;
		p->legacy = 0;
		p->aux = 0U;
		p->desc = NULL;
		p->connection.mode = I915_XD_TC_MODE_NONE;
		p->connection.blocked = 0U;
		p->connection.fia.base = 0U;
		p->connection.fia.index = 0U;
		p->connection.owned = 0;
		p->connection.max_lanes = 0U;
		p->connection.pin = 0U;
		p->links = 0U;
		p->connects = 0U;
		p->connect_failures = 0U;
		p->disconnects = 0U;
	}

	/* The ports may be declared from here on. */
	ports->live = 1;
}

/*
 * Declares the Type-C port of a DDI, whether a connector is wired to it
 * (legacy) rather than a receptacle, and its AUX channel.  Returns 0, or
 * EINVAL for a DDI that is not one of the platform's Type-C ports.
 */
int
drv_i915_xd_tc_ports_declare(
	struct i915_xd_tc_ports *ports,
	unsigned ddi,
	int legacy,
	unsigned aux)
{
	const struct i915_xd_port_desc *desc;
	struct i915_xd_tc_port *p;
	int tc;

	/* The platform's Type-C port of the DDI. */
	tc = drv_i915_xd_tc_ports_index(ports, ddi);
	if (tc < 0)
		return EINVAL;

	/* A port of ports that are not live is not declared. */
	p = i915_xd_tc_port_of(ports, (unsigned)tc);
	if (p == NULL)
		return EINVAL;

	/* The port is used from here on. */
	desc = drv_i915_xd_platform_port(ports->platform, ddi);
	p->desc = desc;
	p->legacy = legacy;
	p->aux = aux;
	p->present = 1;

	/* Succeeded: declared. */
	return 0;
}

/*
 * Tells which Type-C port (0 for TC1) a DDI is on the platform, or -1
 * for a DDI that is not a Type-C port.
 */
int
drv_i915_xd_tc_ports_index(
	const struct i915_xd_tc_ports *ports,
	unsigned ddi)
{
	const struct i915_xd_port_desc *desc;

	/* Ports without a platform have no Type-C port. */
	if (ports->platform == NULL)
		return -1;

	/* The platform's description of the DDI. */
	desc = drv_i915_xd_platform_port(ports->platform, ddi);
	if (desc == NULL)
		return -1;

	/* A combo port, or a Type-C port past the ports kept. */
	if (desc->tc == 0U)
		return -1;
	if (desc->tc > I915_XD_TC_PORTS)
		return -1;

	/* Succeeded: the Type-C port. */
	return (int)desc->tc - 1;
}

/*
 * Reads how the firmware left every declared Type-C port and settles it.
 *
 * A port whose DDI buffer the firmware left enabled on a PHY the display
 * holds is driving the firmware's output: the display keeps the PHY and
 * TC cold blocked and counts one link for that output, which the
 * output's disable gives back.  Any other held port is given back.  Each
 * port's state is logged.
 */
void
drv_i915_xd_tc_ports_readout(
	struct i915_xd_tc_ports *ports)
{
	/* Reads and settles each declared port, logged as the start's readout. */
	i915_xd_tc_port_settle_all(ports, "readout");
}

/*
 * Gives every Type-C port back before the display goes down for a
 * suspend: the outputs were darkened before, so a link still held is
 * logged and dropped; the connection is given back with the platform's
 * flow (TC cold unblocked, the PHY's ownership cleared).  The ports stay
 * declared and live for the resume.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_suspend, intel_tc_port_cleanup).
 */
void
drv_i915_xd_tc_ports_suspend(
	struct i915_xd_tc_ports *ports)
{
	struct i915_xd_tc_port *p;
	unsigned index;
	unsigned mode;
	unsigned links;

	/* Ports that are not live hold nothing. */
	if (!ports->live)
		return;

	/* Gives each declared port back. */
	for (index = 0U; index < I915_XD_TC_PORTS; index++) {
		/* Skips a port nobody declared. */
		p = &ports->port[index];
		if (!p->present)
			continue;

		/* Drops the links and gives the connection back under the port's lock. */
		ports->env.lock(ports->env.ctx, index);

		mode = p->connection.mode;
		links = p->links;
		p->links = 0U;
		i915_xd_tc_port_disconnect(ports, p);

		ports->env.unlock(ports->env.ctx, index);

		/* Logs a port that still held something. */
		if (mode != I915_XD_TC_MODE_NONE || links != 0U)
			ports->env.log(ports->env.ctx, "i915: TC%u: suspend: %s connection given back (%u link(s) dropped)\n", index + 1U, drv_i915_xd_tc_mode_name(mode), links);
	}
}

/*
 * Reads every declared Type-C port again after a resume, as the start's
 * readout does: the firmware brought the Type-C subsystem up again, a
 * legacy port's PHY is waited for, and a PHY left held is given back.
 */
void
drv_i915_xd_tc_ports_resume(
	struct i915_xd_tc_ports *ports)
{
	/* Reads and settles each declared port, logged as the resume's. */
	i915_xd_tc_port_settle_all(ports, "resume");
}

/*
 * Gives every Type-C port back to the Type-C subsystem when the driver
 * stops: each port's links are dropped (the outputs were stopped
 * before), the connection is given back and the port is retired, so a
 * step already on its way takes nothing.  The ports are not live
 * afterwards.  Each port is logged.
 */
void
drv_i915_xd_tc_ports_stop(
	struct i915_xd_tc_ports *ports)
{
	struct i915_xd_tc_port *p;
	unsigned index;
	unsigned blocked;

	/* Ports that are not live, or already stopped, have nothing to give back. */
	if (!ports->live)
		return;

	/* Gives each declared port back. */
	for (index = 0U; index < I915_XD_TC_PORTS; index++) {
		/* Skips a port nobody declared. */
		p = &ports->port[index];
		if (!p->present)
			continue;

		/* Gives the PHY back and retires the port under its lock. */
		ports->env.lock(ports->env.ctx, index);

		/* An output that did not put its link back no longer drives the port. */
		if (p->links != 0U)
			ports->env.log(ports->env.ctx, "i915: TC%u: stop: %u link(s) still held: given back\n", index + 1U, p->links);
		p->links = 0U;

		/* The PHY and the TC cold block go back to the Type-C subsystem. */
		i915_xd_tc_port_disconnect(ports, p);
		blocked = 0U;
		if (p->connection.blocked != 0U)
			blocked = 1U;

		/*
		 * Retired: present is what every step checks under this lock, so one
		 * that already found the port takes nothing more.
		 */
		p->present = 0;

		ports->env.unlock(ports->env.ctx, index);

		/* Logs what the port was left in. */
		ports->env.log(ports->env.ctx, "i915: TC%u: stop: PHY given back (mode %s, cold %u, connects %u, disconnects %u)\n",
			index + 1U,
			drv_i915_xd_tc_mode_name(p->connection.mode),
			blocked,
			p->connects,
			p->disconnects);
	}

	/* No port is answered for from here on. */
	ports->live = 0;
}

/*
 * Reads what is plugged into a Type-C port: a mask of
 * I915_XD_TC_LIVE_DP_ALT, _TBT and _LEGACY.
 *
 * Where the live state is in the FIA (Tiger Lake, Ice Lake), TC cold is
 * kept off for the read.  An undeclared port, or one whose power did not
 * come, reads nothing plugged in.
 */
unsigned
drv_i915_xd_tc_ports_live(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	uint64_t hold;
	unsigned live;
	int error;

	/* An undeclared port has nothing plugged in. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return 0U;
	if (!p->present)
		return 0U;

	/* Keeps TC cold off where the read needs it. */
	hold = i915_xd_tc_port_cold_read(ports, p);
	if (hold != 0U) {
		error = ports->power.get(ports->power.context, hold);
		if (error != 0)
			return 0U;
	}

	/* Reads the live state. */
	live = i915_xd_tc_port_live_raw(ports, p);

	/* The power was for the read only. */
	if (hold != 0U)
		ports->power.put(ports->power.context, hold);

	/* Succeeded: what is plugged in. */
	return live;
}

/*
 * Tells whether something the display can use is plugged into a Type-C
 * port: 1 or 0.
 *
 * A held port counts only what it is held for, so a DisplayPort
 * alternate mode port does not count a Thunderbolt partner; a port held
 * in no mode counts anything plugged in.
 */
int
drv_i915_xd_tc_ports_connected(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	int connected;

	/* An undeclared port has nothing connected. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Reads the live state and the mode together under the port's lock. */
	ports->env.lock(ports->env.ctx, tc);

	connected = i915_xd_tc_port_live_matches(ports, p);

	ports->env.unlock(ports->env.ctx, tc);

	/* Succeeded: reports whether the port is connected. */
	return connected;
}

/*
 * Tells whether something the display can use is plugged into a Type-C
 * port whose lock the caller holds: 1 or 0, as
 * drv_i915_xd_tc_ports_connected().
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_connected_locked).
 */
int
drv_i915_xd_tc_ports_connected_locked(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	int connected;

	/* An undeclared port has nothing connected. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Compares the live state with the mode the lock holds the port in. */
	connected = i915_xd_tc_port_live_matches(ports, p);

	/* Succeeded: reports whether the port is connected. */
	return connected;
}

/*
 * Tells whether the link of a Type-C port must be reset: 1 when an
 * output holds the port in DisplayPort alternate mode and what is
 * plugged in now asks for another mode (the partner went, or came back
 * as something else); 0 otherwise.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_link_needs_reset).
 */
int
drv_i915_xd_tc_ports_link_needs_reset(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	unsigned live;
	unsigned target;
	int needs;

	/* An undeclared port has no link. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return 0;
	if (!p->present)
		return 0;

	/* Compares the mode the links hold with what is plugged in, under the port's lock. */
	ports->env.lock(ports->env.ctx, tc);

	/* Only a link held in DisplayPort alternate mode can lose its partner. */
	needs = 0;
	if (p->links != 0U && p->connection.mode == I915_XD_TC_MODE_DP_ALT) {
		live = drv_i915_xd_tc_ports_live(ports, tc);
		target = i915_xd_tc_port_target_mode(p, live);
		if (target != p->connection.mode)
			needs = 1;
	}

	ports->env.unlock(ports->env.ctx, tc);

	/* Succeeded: reports whether the link must be reset. */
	return needs;
}

/*
 * Locks a Type-C port for a step that uses its PHY, and brings its mode
 * up to date first when no link fixes it.
 *
 * lanes is what the step needs of a DisplayPort alternate mode partner.
 * Returns the mode the port is held in (I915_XD_TC_MODE_*); NONE or TBT
 * means the display does not hold the PHY for DisplayPort.  Every lock is
 * matched by drv_i915_xd_tc_ports_unlock().
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (__intel_tc_port_lock, tc_port_update_mode).
 */
unsigned
drv_i915_xd_tc_ports_lock(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	unsigned lanes)
{
	struct i915_xd_tc_port *p;

	/* A port of ports that are not live is never held. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return I915_XD_TC_MODE_NONE;

	/* Takes the port's lock for the caller's step. */
	ports->env.lock(ports->env.ctx, tc);

	/* A declared port without links follows what is plugged in now. */
	if (p->present && p->links == 0U)
		i915_xd_tc_port_follow_live(ports, p, lanes);

	/* Succeeded: the mode the port is held in. */
	return p->connection.mode;
}

/*
 * Unlocks a Type-C port, giving its PHY back when no link uses it.
 */
void
drv_i915_xd_tc_ports_unlock(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;

	/* A port of ports that are not live was never locked. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;

	/* A port no link uses gives its PHY back before the lock goes. */
	if (p->links == 0U)
		i915_xd_tc_port_disconnect(ports, p);

	ports->env.unlock(ports->env.ctx, tc);
}

/*
 * Adds an output's link to a Type-C port: the port takes its PHY for the
 * lanes if it does not hold it, and keeps its mode until the link is put
 * back.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_get_link).
 */
void
drv_i915_xd_tc_ports_get_link(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	unsigned lanes)
{
	struct i915_xd_tc_port *p;

	/* A port of ports that are not live has no links. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;

	/*
	 * The link fixes the mode the lock brought up to date.  links is what
	 * keeps the PHY held: the unlock gives it back only while it is zero.
	 */
	(void)drv_i915_xd_tc_ports_lock(ports, tc, lanes);
	p->links++;
	drv_i915_xd_tc_ports_unlock(ports, tc);
}

/*
 * Puts an output's link back; the last one gives the port's PHY back at
 * once.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_put_link).
 */
void
drv_i915_xd_tc_ports_put_link(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;

	/* A port of ports that are not live has no links. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;

	/* Drops the link under the lock; the unlock gives the PHY back when it was the last. */
	ports->env.lock(ports->env.ctx, tc);

	/* A link never taken is reported, not counted below zero. */
	if (p->links == 0U) {
		ports->env.log(ports->env.ctx, "i915: TC%u: a link was put back that was never taken\n", tc + 1U);
	} else {
		p->links--;
	}

	drv_i915_xd_tc_ports_unlock(ports, tc);
}

/*
 * Reports the mode a Type-C port is held in, as the port's lock last
 * left it.
 */
unsigned
drv_i915_xd_tc_ports_mode(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	unsigned mode;

	/* A port of ports that are not live is held in no mode. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return I915_XD_TC_MODE_NONE;

	/* Samples the mode under the port's lock. */
	ports->env.lock(ports->env.ctx, tc);

	mode = p->connection.mode;

	ports->env.unlock(ports->env.ctx, tc);

	/* Succeeded: the mode. */
	return mode;
}

/*
 * Reports how many lanes a Type-C port offers its output.
 *
 * Only a DisplayPort alternate mode partner restricts the lanes, by the
 * lanes the FIA gave DisplayPort; any other mode offers four.  The caller
 * holds the port's lock with the port in its mode (TC cold blocked).
 */
unsigned
drv_i915_xd_tc_ports_max_lanes(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	unsigned lanes;

	/* A port of ports that are not live is not restricted. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return I915_XD_TC_PORT_FULL_LANES;

	/* Only DisplayPort alternate mode restricts the lanes. */
	if (p->connection.mode != I915_XD_TC_MODE_DP_ALT)
		return I915_XD_TC_PORT_FULL_LANES;

	/* Counts the lanes the FIA gave. */
	lanes = drv_i915_xd_tc_max_lanes(ports->io, &p->connection.fia);

	/* Succeeded: the lanes the partner offers. */
	return lanes;
}

/*
 * Reads the DisplayPort pin assignment the FIA records for a Type-C port
 * (3 is pin C, 4 is D, 5 is E; 0 when none is recorded or the FIA cannot
 * be read).  The caller holds a link to the port.
 */
unsigned
drv_i915_xd_tc_ports_pin(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_port *p;
	struct i915_xd_tc_fia fia;
	unsigned pin;
	int error;

	/* A port of ports that are not live has no pin assignment. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return 0U;

	/* The port's FIA. */
	error = drv_i915_xd_tc_fia(ports->io, ports->platform, tc, &fia);
	if (error != 0)
		return 0U;

	/* The port's slot of the FIA's pin assignment. */
	pin = drv_i915_xd_tc_pin(ports->io, &fia);

	/* Succeeded: the pin assignment. */
	return pin;
}

/*
 * Tells the FIA which of a Type-C port's lanes carry the DisplayPort
 * main link: one, two or four, from the far end when the lanes are
 * reversed (which only a legacy port's are).
 *
 * The caller holds a link to the port, so the PHY is held and TC cold
 * is blocked.  Another lane count is logged and leaves the port with no
 * lanes, as it was asked for none the FIA has.
 */
void
drv_i915_xd_tc_ports_set_lanes(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	unsigned lanes,
	int reversed)
{
	struct i915_xd_tc_port *p;
	struct i915_xd_tc_fia fia;
	int error;

	/* A port of ports that are not live has no FIA slot. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;

	/* The port's FIA. */
	error = drv_i915_xd_tc_fia(ports->io, ports->platform, tc, &fia);
	if (error != 0) {
		ports->env.log(ports->env.ctx, "i915: TC%u: the FIA could not be read: its lanes are not set\n", tc + 1U);
		return;
	}

	/* One, two or four lanes are what the FIA has. */
	if (lanes == 1U ||
	    lanes == 2U ||
	    lanes == 4U) {
		drv_i915_xd_tc_set_lanes(ports->io, &fia, lanes, reversed);
		return;
	}

	/* Any other count leaves the port's slot without lanes. */
	ports->env.log(ports->env.ctx, "i915: TC%u: no FIA lanes for a %u-lane link\n", tc + 1U, lanes);
	(void)drv_i915_xd_io_rmw(ports->io, fia.base + I915_XD_FIA_DFLEXDPMLE1, I915_XD_DFLEXDPMLE1_LANES(fia.index), 0U);
}

/*
 * Reads what a Type-C port has of DisplayPort for the Type-C connector
 * layer, under the port's lock: the DisplayPort alternate mode live
 * state, and the FIA's pin assignment and lanes when the display holds
 * the port in that mode (TC cold is blocked then, so the FIA answers).
 * An undeclared port has nothing.
 */
void
drv_i915_xd_tc_ports_dp_sample(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	struct i915_xd_tc_dp_sample *sample)
{
	struct i915_xd_tc_port *p;
	unsigned live;
	unsigned pin;
	unsigned lanes;

	/* Nothing, until a declared port says otherwise. */
	sample->hpd = 0;
	sample->pin = 0U;
	sample->lanes = 0U;

	/* An undeclared port has nothing plugged in. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;
	if (!p->present)
		return;

	/* Reads the live state, and the FIA while the port is held in DisplayPort alternate mode. */
	ports->env.lock(ports->env.ctx, tc);

	live = drv_i915_xd_tc_ports_live(ports, tc);
	pin = 0U;
	lanes = 0U;
	if (p->connection.mode == I915_XD_TC_MODE_DP_ALT) {
		pin = drv_i915_xd_tc_pin(ports->io, &p->connection.fia);
		lanes = drv_i915_xd_tc_max_lanes(ports->io, &p->connection.fia);
	}

	ports->env.unlock(ports->env.ctx, tc);

	/* A live DisplayPort alternate mode partner raises the hot plug detect. */
	if ((live & I915_XD_TC_LIVE_DP_ALT) != 0U)
		sample->hpd = 1;

	/* The FIA's pin assignment and lanes, which count only while the port was held so. */
	sample->pin = pin;
	sample->lanes = lanes;
}

/*
 * Logs a Type-C port's state: its mode, links, live state, and the raw
 * words a reader needs to tell what the Type-C subsystem and the FIA
 * reported (the PHY's status, the FIA's lanes and pins, the DDI buffer).
 *
 * The FIA's lanes are logged as read: with a pin D partner they are the
 * only trace of the plug's orientation, which is recorded and not used.
 */
void
drv_i915_xd_tc_ports_log_state(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	const char *why)
{
	struct i915_xd_tc_port *p;
	struct i915_xd_tc_fia fia;
	const char *status_name;
	uint64_t hold;
	uint32_t status;
	uint32_t lanes;
	uint32_t pins;
	uint32_t buffer;
	unsigned live;
	int error;

	/* An undeclared port has no state. */
	p = i915_xd_tc_port_of(ports, tc);
	if (p == NULL)
		return;
	if (!p->present)
		return;

	/* Reads the words with the port's lanes powered and TC cold kept off where the reads need it. */
	hold = i915_xd_tc_port_probe_functions(ports, p);
	error = ports->power.get(ports->power.context, hold);
	if (error != 0)
		return;

	/* The port's FIA; without it only the buffer is read. */
	fia.base = 0U;
	fia.index = 0U;
	status = 0U;
	lanes = 0U;
	pins = 0U;
	live = 0U;
	error = drv_i915_xd_tc_fia(ports->io, ports->platform, tc, &fia);
	if (error == 0) {
		live = i915_xd_tc_port_live_raw(ports, p);
		lanes = drv_i915_xd_io_read(ports->io, fia.base + I915_XD_FIA_DFLEXDPSP);
		pins = drv_i915_xd_io_read(ports->io, fia.base + I915_XD_FIA_DFLEXPA1);
	}

	/* The PHY's status: the TCSS's on display 13, the FIA's PHY mode status before. */
	status_name = "DFLEXDPPMS";
	if (ports->platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		status_name = "TCSS_DDI_STATUS";
		status = drv_i915_xd_io_read(ports->io, I915_XD_TCSS_DDI_STATUS(tc));
	} else if (error == 0) {
		status = drv_i915_xd_io_read(ports->io, fia.base + I915_XD_FIA_DFLEXDPPMS);
	}

	/* The DDI buffer, where display 13 keeps the PHY's ownership. */
	buffer = drv_i915_xd_io_read(ports->io, I915_XD_DDI_BUF_CTL(p->desc->ddi));

	/* The power was needed for the reads only. */
	ports->power.put(ports->power.context, hold);

	/* Logs one line a reader can compare with a known good machine. */
	ports->env.log(ports->env.ctx,
		"i915: TC%u %s: mode=%s links=%u legacy=%d cold=%d live=0x%x | %s=0x%08x FIA%u.%u DFLEXDPSP=0x%08x DFLEXPA1=0x%08x DDI_BUF_CTL=0x%08x\n",
		tc + 1U,
		why,
		drv_i915_xd_tc_mode_name(p->connection.mode),
		p->links,
		p->legacy,
		p->connection.blocked != 0U,
		live,
		status_name,
		(unsigned)status,
		i915_xd_tc_port_fia_number(&fia),
		fia.index,
		(unsigned)lanes,
		(unsigned)pins,
		(unsigned)buffer);
}

/*
 * Names a Type-C mode (I915_XD_TC_MODE_*) for the log.
 */
const char *
drv_i915_xd_tc_mode_name(
	unsigned mode)
{
	/* Picks the name of the mode. */
	switch (mode) {
	case I915_XD_TC_MODE_NONE:
		return "none";
	case I915_XD_TC_MODE_TBT:
		return "tbt";
	case I915_XD_TC_MODE_DP_ALT:
		return "dp-alt";
	case I915_XD_TC_MODE_LEGACY:
		return "legacy";
	default:
		break;
	}

	/* Succeeded: a value that names no mode. */
	return "?";
}

/* Finds a port of live ports, or NULL. */
static struct i915_xd_tc_port *
i915_xd_tc_port_of(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	/* Ports that are not live have none. */
	if (!ports->live)
		return NULL;

	/* A number past the last port names none. */
	if (tc >= I915_XD_TC_PORTS)
		return NULL;

	/* Succeeded: the port. */
	return &ports->port[tc];
}

/*
 * Names the wells that keep TC cold off while the FIA is read: on the
 * FIA flow (Tiger Lake, Ice Lake) those of the port's kind, none on the
 * TCSS flow, whose live state is in the interrupt words.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_hpd_live_status, tc_phy_cold_off_domain).
 */
static uint64_t
i915_xd_tc_port_cold_read(
	const struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p)
{
	uint64_t functions;
	unsigned mode;

	/* The TCSS flow reads nothing in the FIA for the live state. */
	if (ports->platform->tc_flow != I915_XD_TC_FLOW_FIA)
		return 0U;

	/* A legacy port's wells, or a receptacle's. */
	mode = I915_XD_TC_MODE_DP_ALT;
	if (p->legacy)
		mode = I915_XD_TC_MODE_LEGACY;
	functions = drv_i915_xd_tc_cold_functions(ports->platform, p->aux, mode);

	/* Succeeded: the wells. */
	return functions;
}

/*
 * Names the wells a readout of a port holds: the port's lanes (the DDI
 * buffer, where display 13 keeps the PHY's ownership) and what keeps TC
 * cold off while the FIA is read.
 */
static uint64_t
i915_xd_tc_port_probe_functions(
	const struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p)
{
	uint64_t functions;

	/* The lanes, and TC cold kept off. */
	functions = I915_XD_POWERS_LANES(p->desc->ddi);
	functions |= i915_xd_tc_port_cold_read(ports, p);

	/* Succeeded: the wells. */
	return functions;
}

/* Reads a port's live state with whatever the read needs already held. */
static unsigned
i915_xd_tc_port_live_raw(
	struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p)
{
	struct i915_xd_tc_fia fia;
	unsigned live;
	int error;

	/* The port's FIA; one that cannot be read has nothing live. */
	error = drv_i915_xd_tc_fia(ports->io, ports->platform, p->index, &fia);
	if (error != 0)
		return 0U;

	/* The platform's live state. */
	live = drv_i915_xd_tc_live(ports->io, ports->platform, p->desc, &fia);

	/* Succeeded: what is plugged in. */
	return live;
}

/*
 * The mode a live state asks for: a legacy connector over a DisplayPort
 * alternate mode partner over a Thunderbolt partner, or none when
 * nothing is plugged in.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (hpd_mask_to_tc_mode).
 */
static unsigned
i915_xd_tc_port_mode_of_live(
	unsigned live)
{
	/* A legacy connector wins, then DisplayPort alternate mode, then Thunderbolt. */
	if ((live & I915_XD_TC_LIVE_LEGACY) != 0U)
		return I915_XD_TC_MODE_LEGACY;
	if ((live & I915_XD_TC_LIVE_DP_ALT) != 0U)
		return I915_XD_TC_MODE_DP_ALT;
	if ((live & I915_XD_TC_LIVE_TBT) != 0U)
		return I915_XD_TC_MODE_TBT;

	/* Succeeded: nothing plugged in asks for no mode. */
	return I915_XD_TC_MODE_NONE;
}

/* The mode of a port with nothing plugged in: a wired connector stays legacy, a receptacle is left to Thunderbolt. */
static unsigned
i915_xd_tc_port_default_mode(
	const struct i915_xd_tc_port *p)
{
	/* A legacy port is always the display's. */
	if (p->legacy)
		return I915_XD_TC_MODE_LEGACY;

	/* Succeeded: a receptacle the display does not hold. */
	return I915_XD_TC_MODE_TBT;
}

/*
 * The mode a port should be held in for a live state.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_get_target_mode).
 */
static unsigned
i915_xd_tc_port_target_mode(
	const struct i915_xd_tc_port *p,
	unsigned live)
{
	unsigned mode;

	/* What is plugged in decides; nothing plugged in leaves the default. */
	mode = i915_xd_tc_port_mode_of_live(live);
	if (mode == I915_XD_TC_MODE_NONE)
		mode = i915_xd_tc_port_default_mode(p);

	/* Succeeded: the target mode. */
	return mode;
}

/*
 * Corrects the VBT's legacy flag of a port when the live state reports a
 * single kind of partner that contradicts it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_port_fixup_legacy_flag).
 */
static void
i915_xd_tc_port_fix_legacy(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p,
	unsigned live)
{
	int live_legacy;

	/* Only a single live bit says what is wired to the port. */
	if (live != I915_XD_TC_LIVE_DP_ALT &&
	    live != I915_XD_TC_LIVE_TBT &&
	    live != I915_XD_TC_LIVE_LEGACY)
		return;

	/* A live legacy bit means a wired connector; any other means a receptacle. */
	live_legacy = 0;
	if (live == I915_XD_TC_LIVE_LEGACY)
		live_legacy = 1;

	/* The flag agrees with the live state. */
	if (live_legacy == p->legacy)
		return;

	/* Trusts the live state over the VBT. */
	ports->env.log(ports->env.ctx, "i915: TC%u: live status 0x%x contradicts the VBT's legacy flag %d: the flag is corrected\n", p->index + 1U, live, p->legacy);
	p->legacy = live_legacy;
}

/*
 * Waits for a legacy port's PHY to become ready: the Type-C subsystem's
 * firmware brings it up at boot and resume whether or not a connector
 * is plugged in.  The wait sleeps between the reads, so the CPU serves
 * other threads for the up to half a second it may last; the time is
 * measured on the clock, as a sleep may last longer than asked.  A PHY
 * that does not come is logged.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_wait_for_ready).
 */
static void
i915_xd_tc_port_wait_ready(
	struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p,
	const struct i915_xd_tc_fia *fia)
{
	uint64_t start_us;
	uint64_t now_us;
	int ready;
	int clock_error;
	int sleep_error;

	/* The time the wait starts at; without a clock the ready bit is read once. */
	clock_error = ports->env.now_us(ports->env.ctx, &start_us);
	if (clock_error != 0) {
		ready = drv_i915_xd_tc_ready(ports->io, ports->platform, p->index, fia);
		if (!ready)
			ports->env.log(ports->env.ctx, "i915: TC%u: the legacy port's PHY is not ready and the clock failed (error %d)\n", p->index + 1U, clock_error);
		return;
	}

	/* Polls the ready bit, sleeping between the reads, until it comes or the time is up. */
	for (;;) {
		/* Reads the ready bit; the wait is over once it is set. */
		ready = drv_i915_xd_tc_ready(ports->io, ports->platform, p->index, fia);
		if (ready)
			return;

		/* Reads the clock; a failed time base ends the wait. */
		clock_error = ports->env.now_us(ports->env.ctx, &now_us);
		if (clock_error != 0)
			break;

		/* The time is up: the PHY did not come. */
		if (now_us - start_us >= I915_XD_TC_PORT_READY_TIMEOUT_US)
			break;

		/* Sleeps before the next read; a failed time base ends the wait. */
		sleep_error = ports->env.sleep_us(ports->env.ctx, I915_XD_TC_PORT_READY_POLL_US);
		if (sleep_error != 0)
			break;
	}

	/* Reports a legacy PHY that never became ready. */
	ports->env.log(ports->env.ctx, "i915: TC%u: timeout waiting for the legacy port's PHY to become ready\n", p->index + 1U);
}

/*
 * Reads the mode a port's hardware is in now, with the readout's wells
 * held.
 *
 * The display holds the port only when the PHY is both ready and owned;
 * it then holds it for what is plugged in, or for the port's kind when
 * nothing is.  A PHY not held is left to Thunderbolt, unless a legacy
 * connector, which only the display drives, is what is plugged in.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_get_current_mode, get_tc_mode_in_phy_owned_state,
 * get_tc_mode_in_phy_not_owned_state).
 */
static unsigned
i915_xd_tc_port_current_mode(
	struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p,
	const struct i915_xd_tc_fia *fia)
{
	unsigned live_mode;
	unsigned mode;
	unsigned live;
	int ready;
	int owned;

	/* Reads what is plugged in. */
	live = drv_i915_xd_tc_live(ports->io, ports->platform, p->desc, fia);
	live_mode = i915_xd_tc_port_mode_of_live(live);

	/* A legacy port's PHY is brought up by the firmware: waits for it. */
	if (p->legacy)
		i915_xd_tc_port_wait_ready(ports, p, fia);

	/* Reads whether the PHY is ready and whether the display owns it. */
	ready = drv_i915_xd_tc_ready(ports->io, ports->platform, p->index, fia);
	owned = drv_i915_xd_tc_owned(ports->io, ports->platform, p->desc, fia);

	/* An owned PHY that is not ready is a hardware contradiction: it counts as not held. */
	if (owned && !ready)
		ports->env.log(ports->env.ctx, "i915: TC%u: the PHY is owned but not ready\n", p->index + 1U);

	/* The PHY the display holds: held for what is plugged in, or for the port's kind. */
	if (ready && owned) {
		/* A legacy connector or a DisplayPort partner names the mode; nothing plugged in leaves the port's kind. */
		if (live_mode == I915_XD_TC_MODE_LEGACY || live_mode == I915_XD_TC_MODE_DP_ALT) {
			mode = live_mode;
		} else if (p->legacy) {
			mode = I915_XD_TC_MODE_LEGACY;
		} else {
			mode = I915_XD_TC_MODE_DP_ALT;
		}
	} else if (live_mode == I915_XD_TC_MODE_LEGACY) {
		/* A legacy connector the display does not hold: nothing holds it. */
		mode = I915_XD_TC_MODE_NONE;
	} else if (live_mode == I915_XD_TC_MODE_NONE && p->legacy) {
		/* A legacy port with nothing plugged in: nothing holds it. */
		mode = I915_XD_TC_MODE_NONE;
	} else {
		/* A receptacle the display does not hold is left to Thunderbolt. */
		mode = I915_XD_TC_MODE_TBT;
	}

	/* Logs what the mode was read from. */
	ports->env.log(ports->env.ctx, "i915: TC%u: PHY mode %s (ready %d, owned %d, live %s)\n",
		p->index + 1U,
		drv_i915_xd_tc_mode_name(mode),
		ready,
		owned,
		drv_i915_xd_tc_mode_name(live_mode));

	/* Succeeded: the mode. */
	return mode;
}

/*
 * Connects a port in a mode with the platform's flow: 1 when the port now
 * serves the mode, 0 when it was refused (the flow gave back what it
 * took, and the reason is logged).
 */
static int
i915_xd_tc_port_take(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p,
	unsigned mode,
	unsigned lanes)
{
	int error;

	/* The platform's flow. */
	error = drv_i915_xd_tc_connect(ports->io, ports->platform, p->desc, p->aux, mode, lanes, &ports->power, &p->connection);
	if (error == 0)
		return 1;

	/* Logs why the port was refused. */
	if (error == EBUSY) {
		ports->env.log(ports->env.ctx, "i915: TC%u: the PHY is not ready\n", p->index + 1U);
	} else if (error == ENODEV) {
		ports->env.log(ports->env.ctx, "i915: TC%u: the DP-alt partner went away during the connect\n", p->index + 1U);
	} else if (error == ENOSPC) {
		ports->env.log(ports->env.ctx, "i915: TC%u: the partner offers %u lanes, %u are required\n", p->index + 1U, p->connection.max_lanes, lanes);
	} else {
		ports->env.log(ports->env.ctx, "i915: TC%u: the %s connect failed (error %d)\n", p->index + 1U, drv_i915_xd_tc_mode_name(mode), error);
	}

	/* The port was refused. */
	return 0;
}

/*
 * Brings a port that holds nothing into the mode its live state asks
 * for; a mode that cannot be served falls back to the port's default.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_connect).
 */
static void
i915_xd_tc_port_connect(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p,
	unsigned lanes)
{
	unsigned live;
	unsigned target;
	unsigned fallback;
	int taken;

	/* Reads what is plugged in and corrects the legacy flag by it. */
	live = drv_i915_xd_tc_ports_live(ports, p->index);
	i915_xd_tc_port_fix_legacy(ports, p, live);

	/* Tries the mode the live state asks for. */
	target = i915_xd_tc_port_target_mode(p, live);
	taken = i915_xd_tc_port_take(ports, p, target, lanes);
	if (taken) {
		p->connects++;
		return;
	}

	/* Counts the refusal. */
	p->connect_failures++;

	/* A refused default mode leaves the port holding nothing. */
	fallback = i915_xd_tc_port_default_mode(p);
	if (target == fallback)
		return;

	/* Tries the default mode; a refusal of it too leaves the port holding nothing. */
	taken = i915_xd_tc_port_take(ports, p, fallback, lanes);
	if (!taken) {
		p->connect_failures++;
		return;
	}

	/* Succeeded: the port holds its default mode. */
	p->connects++;
}

/*
 * Gives a port's connection back with the platform's flow.  A port that
 * holds nothing stays so.
 */
static void
i915_xd_tc_port_disconnect(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p)
{
	/* A port that holds nothing has nothing to give back. */
	if (p->connection.mode == I915_XD_TC_MODE_NONE)
		return;

	/* The platform's flow gives TC cold and the PHY back. */
	drv_i915_xd_tc_disconnect(ports->io, ports->platform, p->desc, &ports->power, &p->connection);
	p->disconnects++;
}

/*
 * Brings a port without links to the mode its live state asks for:
 * nothing is done when it is already there.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_port_update_mode, tc_port_needs_reset).
 */
static void
i915_xd_tc_port_follow_live(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p,
	unsigned lanes)
{
	unsigned live;
	unsigned target;
	unsigned old_mode;

	/* The port already holds what is plugged in. */
	live = drv_i915_xd_tc_ports_live(ports, p->index);
	target = i915_xd_tc_port_target_mode(p, live);
	if (target == p->connection.mode)
		return;

	/* Gives back what the port holds and connects it for the new mode. */
	old_mode = p->connection.mode;
	i915_xd_tc_port_disconnect(ports, p);
	i915_xd_tc_port_connect(ports, p, lanes);

	/* Logs the change. */
	ports->env.log(ports->env.ctx, "i915: TC%u: mode %s -> %s\n", p->index + 1U, drv_i915_xd_tc_mode_name(old_mode), drv_i915_xd_tc_mode_name(p->connection.mode));
}

/*
 * Reads how the firmware left a port and settles it; the caller holds
 * the port's lock.
 *
 * A held PHY keeps TC cold blocked; one whose DDI buffer is enabled is
 * the firmware's output and counts one link.  Any other held PHY is
 * given back.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_init_mode, tc_phy_get_current_mode).
 */
static void
i915_xd_tc_port_readout(
	struct i915_xd_tc_ports *ports,
	struct i915_xd_tc_port *p)
{
	struct i915_xd_tc_fia fia;
	uint64_t hold;
	uint64_t blocked;
	uint32_t buffer;
	unsigned mode;
	int held;
	int error;

	/* The port's lanes, and TC cold kept off where the FIA is read. */
	hold = i915_xd_tc_port_probe_functions(ports, p);
	error = ports->power.get(ports->power.context, hold);
	if (error != 0) {
		ports->env.log(ports->env.ctx, "i915: TC%u: readout skipped: the port's power did not come (error %d)\n", p->index + 1U, error);
		return;
	}

	/* The port's FIA. */
	error = drv_i915_xd_tc_fia(ports->io, ports->platform, p->index, &fia);
	if (error != 0) {
		ports->power.put(ports->power.context, hold);
		ports->env.log(ports->env.ctx, "i915: TC%u: readout skipped: the FIA could not be read (error %d)\n", p->index + 1U, error);
		return;
	}

	/* Reads the mode the hardware is in. */
	mode = i915_xd_tc_port_current_mode(ports, p, &fia);

	/* A PHY the display holds keeps TC cold blocked; one that cannot is given back. */
	held = 0;
	blocked = 0U;
	if (mode == I915_XD_TC_MODE_DP_ALT || mode == I915_XD_TC_MODE_LEGACY) {
		blocked = drv_i915_xd_tc_cold_functions(ports->platform, p->aux, mode);
		error = 0;
		if (blocked != 0U)
			error = ports->power.get(ports->power.context, blocked);
		if (error != 0) {
			(void)drv_i915_xd_tc_own(ports->io, ports->platform, p->desc, &fia, 0);
			blocked = 0U;
			mode = I915_XD_TC_MODE_NONE;
		} else {
			held = 1;
		}
	}

	/* Records the connection the firmware left. */
	p->connection.mode = mode;
	p->connection.blocked = blocked;
	p->connection.fia = fia;
	p->connection.owned = held;
	p->connection.max_lanes = I915_XD_TC_PORT_FULL_LANES;
	p->connection.pin = 0U;
	if (mode == I915_XD_TC_MODE_DP_ALT) {
		p->connection.max_lanes = drv_i915_xd_tc_max_lanes(ports->io, &fia);
		p->connection.pin = drv_i915_xd_tc_pin(ports->io, &fia);
	}

	/* An enabled DDI buffer on a held PHY is the firmware's output on the port: one link. */
	buffer = drv_i915_xd_io_read(ports->io, I915_XD_DDI_BUF_CTL(p->desc->ddi));
	if ((buffer & I915_XD_DDI_BUF_ENABLE) != 0U && held)
		p->links = 1U;

	/* The readout's wells were needed for the reads only. */
	ports->power.put(ports->power.context, hold);

	/* A port nothing drives is given back. */
	if (p->links == 0U)
		i915_xd_tc_port_disconnect(ports, p);
}

/* Compares a port's live state with what its mode accepts; the caller holds the port's lock. */
static int
i915_xd_tc_port_live_matches(
	struct i915_xd_tc_ports *ports,
	const struct i915_xd_tc_port *p)
{
	unsigned live;
	unsigned accepted;

	/* Reads what is plugged in now. */
	live = drv_i915_xd_tc_ports_live(ports, p->index);

	/* The live bits the port's mode accepts: any of them while it is held in no mode. */
	accepted = I915_XD_TC_LIVE_DP_ALT | I915_XD_TC_LIVE_TBT | I915_XD_TC_LIVE_LEGACY;
	if (p->connection.mode == I915_XD_TC_MODE_DP_ALT) {
		accepted = I915_XD_TC_LIVE_DP_ALT;
	} else if (p->connection.mode == I915_XD_TC_MODE_TBT) {
		accepted = I915_XD_TC_LIVE_TBT;
	} else if (p->connection.mode == I915_XD_TC_MODE_LEGACY) {
		accepted = I915_XD_TC_LIVE_LEGACY;
	}

	/* Something the mode accepts is not live. */
	if ((live & accepted) == 0U)
		return 0;

	/* Succeeded: something the mode accepts is plugged in. */
	return 1;
}

/* Numbers the FIA of a port's words for the log (1 to 3; 0 when unknown). */
static unsigned
i915_xd_tc_port_fia_number(
	const struct i915_xd_tc_fia *fia)
{
	/* The FIA by its base. */
	if (fia->base == I915_XD_FIA1_BASE)
		return 1U;
	if (fia->base == I915_XD_FIA2_BASE)
		return 2U;
	if (fia->base == I915_XD_FIA3_BASE)
		return 3U;

	/* Succeeded: a FIA the log cannot name. */
	return 0U;
}

/* Reads and settles every declared port under its lock, and logs each one for a reason. */
static void
i915_xd_tc_port_settle_all(
	struct i915_xd_tc_ports *ports,
	const char *why)
{
	struct i915_xd_tc_port *p;
	unsigned index;

	/* Ports that are not live have nothing to read. */
	if (!ports->live)
		return;

	/* Reads and settles each declared port. */
	for (index = 0U; index < I915_XD_TC_PORTS; index++) {
		/* Skips a port nobody declared. */
		p = &ports->port[index];
		if (!p->present)
			continue;

		/* Reads and settles the port under its lock. */
		ports->env.lock(ports->env.ctx, index);

		i915_xd_tc_port_readout(ports, p);

		ports->env.unlock(ports->env.ctx, index);

		/* Logs what the reading found and left. */
		drv_i915_xd_tc_ports_log_state(ports, index, why);
	}
}
