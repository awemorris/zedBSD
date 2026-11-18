/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What binds the display's Type-C ports to the device (see tc-port.h).
 *
 * The ports (xd-tc-port.c) ask for a lock per port, the power wells by
 * function, a clock, a sleep and the log; this file answers with one
 * mutex per port, the display's power wells, the kernel's monotonic
 * counter, its sleep to the next tick and the kernel log.  The start
 * declares the Type-C ports the output setup found, with the VBT's
 * legacy flag and AUX channel, reads how the firmware left them and
 * tells the Type-C connector layer what each has of DisplayPort; the
 * stop gives every port back.
 */

#include "internal.h"
#include "display-power.h"
#include "tc-port.h"
#include "xd-backend.h"
#include "xd-vbt.h"

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include <stdarg.h>

/* The longest line the ports log. */
#define I915_TC_PORT_LOG_LINE		256U

/* Microseconds in a second, for the counter's conversion. */
#define I915_TC_PORT_US_PER_SECOND	1000000U

/*
 * The display's Type-C ports and what binds them to the device.
 *
 * The display owns it from the start of its outputs to its destruction
 * (display->tc_ports).  Each port's mutex is the lock the ports take
 * through their environment, before any power well's lock.
 */
struct i915_tc_port_world {
	struct i915_display *display;
	struct i915_xd_tc_ports ports;
	struct mutex locks[I915_XD_TC_PORTS];
	unsigned power_failures;
};

/*
 * The USB Type-C connector layer, which the ports' DisplayPort state is
 * reported to, and which forgets it when the display stops.
 *
 * It is linked only with CONFIG_DRIVER_ACPI and CONFIG_DRIVER_TYPEC (the
 * UCSI driver); without it the symbols are weak and NULL, and nothing is
 * told.
 */
extern int drv_typec_display_report(unsigned port, const struct drv_typec_dp_state *state) __attribute__((weak));
extern int drv_typec_display_forget(unsigned port) __attribute__((weak));

static int tc_port_power_get(void *context, uint64_t functions);
static void tc_port_power_put(void *context, uint64_t functions);
static void tc_port_lock(void *ctx, unsigned tc);
static void tc_port_unlock(void *ctx, unsigned tc);
static int tc_port_now_us(void *ctx, uint64_t *now);
static int tc_port_sleep_us(void *ctx, unsigned us);
static void tc_port_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void tc_port_declare(struct i915_display *display, struct i915_tc_port_world *world);
static struct i915_xd_tc_ports *tc_port_lcd_ports(struct i915_lcd_kernel *kernel, int tc_port, const char *what);
static struct i915_xd_dkl *tc_port_lcd_dkl(struct i915_lcd_kernel *kernel, int tc_port);

/*
 * Binds the display's Type-C ports and reads how the firmware left them.
 *
 * Runs once, after the output setup made the table of the ports and
 * before the hotplug path starts.  A display whose platform has no Type-C
 * flow, or that has no xd description, has no Type-C ports here.
 */
void
drv_i915_tc_port_start(
	struct i915_display *display)
{
	struct i915_tc_port_world *world;
	struct i915_xd_tc_port_env env;
	struct i915_xd_tc_power power;
	const struct i915_xd_platform *platform;
	unsigned tc;
	int error;

	/* A display that started its ports already has nothing more to bind. */
	if (display->tc_ports != NULL)
		return;

	/* Only a described platform with a Type-C flow has Type-C ports. */
	platform = NULL;
	if (display->xd.chosen)
		platform = display->xd.choice.platform;
	if (platform == NULL || platform->tc_flow == I915_XD_TC_FLOW_NONE) {
		kern_logf("i915: TC: the platform has no Type-C ports the display drives\n");
		return;
	}

	/* The binding, zeroed. */
	world = kern_calloc(1U, sizeof(*world));
	if (world == NULL) {
		kern_logf("i915: TC: no memory for the Type-C ports: they are not driven\n");
		return;
	}

	/* A lock per port. */
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++) {
		error = mutex_init(&world->locks[tc], LOCK_RANK_DEVICE, "i915 tc port");
		if (error != 0) {
			kern_free(world);
			kern_logf("i915: TC: the port locks could not be made: the Type-C ports are not driven\n");
			return;
		}
	}

	/* What the ports ask the device for. */
	world->display = display;
	power.get = tc_port_power_get;
	power.put = tc_port_power_put;
	power.context = world;
	kern_memset(&env, 0, sizeof(env));
	env.ctx = world;
	env.lock = tc_port_lock;
	env.unlock = tc_port_unlock;
	env.now_us = tc_port_now_us;
	env.sleep_us = tc_port_sleep_us;
	env.log = tc_port_log;
	drv_i915_xd_tc_ports_init(&world->ports, &display->xd.io, platform, &power, &env);

	/* The ports are bound from here on. */
	display->tc_ports = world;

	/* Declares the ports the VBT names and reads how the firmware left them. */
	tc_port_declare(display, world);
	drv_i915_xd_tc_ports_readout(&world->ports);

	/* Tells the Type-C layer what each declared port has of DisplayPort after the readout. */
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++) {
		if (world->ports.port[tc].present)
			drv_i915_tc_port_report(&world->ports, tc);
	}
}

/*
 * Gives the display's Type-C ports back to the Type-C subsystem when the
 * driver stops: every PHY the display owns and every TC cold block it
 * holds.
 *
 * Runs after the outputs were stopped and the hotplug path closed, while
 * the power wells still work; any later step on a port finds none.
 */
void
drv_i915_tc_port_stop(
	struct i915_display *display)
{
	struct i915_tc_port_world *world;
	unsigned tc;

	/* A display whose ports were never bound has nothing to give back. */
	world = display->tc_ports;
	if (world == NULL)
		return;

	/* Gives every port back; the ports log each one. */
	drv_i915_xd_tc_ports_stop(&world->ports);

	/* The Type-C layer forgets what the display reported of each port; a kernel without it has none. */
	if (drv_typec_display_forget == NULL)
		return;
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++)
		(void)drv_typec_display_forget(tc);
}

/*
 * Gives the display's Type-C ports back before the display goes down for
 * a suspend (the hotplug was quiesced and the outputs darkened before).
 */
void
drv_i915_tc_port_suspend(
	struct i915_display *display)
{
	/* A display whose ports were never bound has nothing to give back. */
	if (display->tc_ports == NULL)
		return;

	/* Gives every port's connection back; the ports stay declared. */
	drv_i915_xd_tc_ports_suspend(&display->tc_ports->ports);
}

/*
 * Reads the display's Type-C ports again after a resume, once the display
 * core runs and the interrupts are back, and tells the Type-C connector
 * layer what each has of DisplayPort.
 */
void
drv_i915_tc_port_resume(
	struct i915_display *display)
{
	struct i915_tc_port_world *world;
	unsigned tc;

	/* A display whose ports were never bound has nothing to read. */
	world = display->tc_ports;
	if (world == NULL)
		return;

	/* Reads and settles every port as the start did. */
	drv_i915_xd_tc_ports_resume(&world->ports);

	/* Tells the Type-C layer what each declared port has of DisplayPort now. */
	for (tc = 0U; tc < I915_XD_TC_PORTS; tc++) {
		if (world->ports.port[tc].present)
			drv_i915_tc_port_report(&world->ports, tc);
	}
}

/*
 * Frees the display's Type-C ports, which must have been stopped.
 */
void
drv_i915_tc_port_destroy(
	struct i915_display *display)
{
	/* A display without them has nothing to free. */
	if (display->tc_ports == NULL)
		return;

	/* Frees them and forgets them. */
	kern_free(display->tc_ports);
	display->tc_ports = NULL;
}

/*
 * Returns the display's Type-C ports, or NULL when they are not bound.
 */
struct i915_xd_tc_ports *
drv_i915_tc_port_ports(
	struct i915_display *display)
{
	/* A display without bound ports has none to give. */
	if (display->tc_ports == NULL)
		return NULL;

	/* Succeeded: the ports. */
	return &display->tc_ports->ports;
}

/*
 * Tells which Type-C port a DDI is: 0 for TC1, or -1 for a DDI that is
 * not a Type-C port of the display's bound ports.
 */
int
drv_i915_tc_port_of(
	struct i915_display *display,
	int ddi)
{
	int tc;

	/* Without bound ports, and for no DDI, there is none. */
	if (display->tc_ports == NULL || ddi < 0)
		return -1;

	/* The platform's Type-C port of the DDI. */
	tc = drv_i915_xd_tc_ports_index(&display->tc_ports->ports, (unsigned)ddi);

	/* Succeeded: the port, or -1. */
	return tc;
}

/*
 * Returns the AUX channel of a Type-C port (the VBT's, or the platform's
 * own when the VBT names none); 0 for a port that is not bound.
 */
unsigned
drv_i915_tc_port_aux(
	struct i915_display *display,
	unsigned tc)
{
	/* A port of no bound ports has no channel. */
	if (display->tc_ports == NULL || tc >= I915_XD_TC_PORTS)
		return 0U;

	/* Succeeded: the port's channel. */
	return display->tc_ports->ports.port[tc].aux;
}

/*
 * Tells the USB Type-C connector layer what a Type-C port has of
 * DisplayPort: the hot plug detect of a DisplayPort alternate mode
 * partner, the pin assignment and the lanes (the plug's orientation is
 * not known to the display).
 *
 * Called from the display's start and its hotplug work, never from the
 * interrupt, and with the port's lock not held (the sample takes it).  A
 * kernel without the Type-C layer reports nothing.
 */
void
drv_i915_tc_port_report(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	struct i915_xd_tc_dp_sample sample;
	struct drv_typec_dp_state state;
	int error;

	/* A kernel without the Type-C layer has no one to tell. */
	if (drv_typec_display_report == NULL)
		return;

	/* Reads the port. */
	drv_i915_xd_tc_ports_dp_sample(ports, tc, &sample);

	/* The layer's form: the FIA numbers the pin assignments as the layer does (3 is C). */
	kern_memset(&state, 0, sizeof(state));
	state.known = true;
	if (sample.hpd)
		state.hpd = true;
	state.pin = DRV_TYPEC_DP_PIN_NONE;
	if (sample.pin <= (unsigned)DRV_TYPEC_DP_PIN_F)
		state.pin = (enum drv_typec_dp_pin)sample.pin;
	state.lanes = sample.lanes;
	state.orientation = DRV_TYPEC_ORIENTATION_UNKNOWN;

	/* Tells the layer; a refusal is only logged. */
	error = drv_typec_display_report(tc, &state);
	if (error != 0)
		kern_logf("i915: TC%u: the Type-C layer refused the DisplayPort report (error %d)\n", tc + 1U, error);
}

/*
 * Gives an output's link to a Type-C port back (the screen's tc_put_link
 * hook).
 *
 * The run's context is its struct i915_lcd_kernel; a run whose display has
 * no bound ports gives nothing back.
 */
void
drv_i915_lcd_tc_put_link(
	void *ctx,
	int tc_port)
{
	struct i915_xd_tc_ports *ports;

	/* Finds the run's Type-C ports; without them nothing is given back. */
	ports = tc_port_lcd_ports(ctx, tc_port, "put_link");
	if (ports == NULL)
		return;

	/* Gives the link back; the last one gives the PHY back at once. */
	drv_i915_xd_tc_ports_put_link(ports, (unsigned)tc_port);
}

/*
 * Takes an output's link to a Type-C port (the screen's tc_get_link
 * hook): the port takes its PHY for the lanes if it does not hold it.
 */
void
drv_i915_lcd_tc_get_link(
	void *ctx,
	int tc_port,
	int required_lanes)
{
	struct i915_xd_tc_ports *ports;
	unsigned lanes;

	/* Finds the run's Type-C ports; without them nothing is taken. */
	ports = tc_port_lcd_ports(ctx, tc_port, "get_link");
	if (ports == NULL)
		return;

	/* A link asks for at least one lane. */
	lanes = 1U;
	if (required_lanes > 1)
		lanes = (unsigned)required_lanes;

	/* Takes the link. */
	drv_i915_xd_tc_ports_get_link(ports, (unsigned)tc_port, lanes);
}

/*
 * Reports the mode a Type-C port is held in (the screen's tc_mode hook),
 * in the backend's numbering (I915_XD_BACKEND_TC_*): none without bound
 * ports.
 */
int
drv_i915_lcd_tc_mode(
	void *ctx,
	int tc_port)
{
	struct i915_xd_tc_ports *ports;
	unsigned mode;

	/* Finds the run's Type-C ports; without them the port is held in no mode. */
	ports = tc_port_lcd_ports(ctx, tc_port, "mode");
	if (ports == NULL)
		return I915_XD_BACKEND_TC_NONE;

	/* Samples the mode under the port's lock. */
	mode = drv_i915_xd_tc_ports_mode(ports, (unsigned)tc_port);

	/* The backend's numbering of the mode. */
	if (mode == I915_XD_TC_MODE_TBT)
		return I915_XD_BACKEND_TC_TBT;
	if (mode == I915_XD_TC_MODE_DP_ALT)
		return I915_XD_BACKEND_TC_DP_ALT;
	if (mode == I915_XD_TC_MODE_LEGACY)
		return I915_XD_BACKEND_TC_LEGACY;

	/* Succeeded: the port is held in no mode. */
	return I915_XD_BACKEND_TC_NONE;
}

/*
 * Programs the FIA's DisplayPort lanes of a Type-C port (the screen's
 * tc_set_fia_lane_count hook).
 */
void
drv_i915_lcd_tc_set_fia_lane_count(
	void *ctx,
	int tc_port,
	int required_lanes,
	int lane_reversal)
{
	struct i915_xd_tc_ports *ports;
	unsigned lanes;

	/* Finds the run's Type-C ports; without them nothing is programmed. */
	ports = tc_port_lcd_ports(ctx, tc_port, "set_fia_lane_count");
	if (ports == NULL)
		return;

	/* A count below one is no count the FIA has. */
	lanes = 0U;
	if (required_lanes > 0)
		lanes = (unsigned)required_lanes;

	/* Programs the FIA. */
	drv_i915_xd_tc_ports_set_lanes(ports, (unsigned)tc_port, lanes, lane_reversal);
}

/*
 * Reads the FIA's DisplayPort pin assignment of a Type-C port (the
 * screen's tc_pin_assignment hook): 0 without bound ports.
 */
unsigned
drv_i915_lcd_tc_pin_assignment(
	void *ctx,
	int tc_port)
{
	struct i915_xd_tc_ports *ports;
	unsigned pin_assignment;

	/* Finds the run's Type-C ports; without them there is no assignment. */
	ports = tc_port_lcd_ports(ctx, tc_port, "pin_assignment");
	if (ports == NULL)
		return 0U;

	/* Reads the FIA. */
	pin_assignment = drv_i915_xd_tc_ports_pin(ports, (unsigned)tc_port);

	/* Succeeded: the pin assignment. */
	return pin_assignment;
}

/*
 * Reads a register of a Type-C port's Dekel PHY (the screen's dkl_read
 * hook): all-ones, a PHY that does not answer, when the run has no
 * Dekel access.
 */
uint32_t
drv_i915_lcd_dkl_read(
	void *ctx,
	int tc_port,
	uint32_t phy_address)
{
	struct i915_xd_dkl *dkl;
	uint32_t value;

	/* Finds the display's Dekel access; without it the PHY does not answer. */
	dkl = tc_port_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return 0xffffffffU;

	/* Reads under the bank index lock. */
	value = drv_i915_xd_dkl_read(dkl, (unsigned)tc_port, phy_address);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a register of a Type-C port's Dekel PHY (the screen's dkl_write
 * hook).
 */
void
drv_i915_lcd_dkl_write(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t value)
{
	struct i915_xd_dkl *dkl;

	/* Finds the display's Dekel access; without it nothing is written. */
	dkl = tc_port_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return;

	/* Writes under the bank index lock. */
	drv_i915_xd_dkl_write(dkl, (unsigned)tc_port, phy_address, value);
}

/*
 * Clears and sets bits of a register of a Type-C port's Dekel PHY (the
 * screen's dkl_rmw hook).
 */
void
drv_i915_lcd_dkl_rmw(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t clear,
	uint32_t set)
{
	struct i915_xd_dkl *dkl;

	/* Finds the display's Dekel access; without it nothing is changed. */
	dkl = tc_port_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return;

	/* Changes the register under one hold of the bank index lock. */
	drv_i915_xd_dkl_rmw(dkl, (unsigned)tc_port, phy_address, clear, set);
}

/* Takes the wells of a set of functions for the ports: 0, or the wells' error. */
static int
tc_port_power_get(
	void *context,
	uint64_t functions)
{
	struct i915_tc_port_world *world;
	struct i915_display *display;
	int error;

	/* The ports' state and their display. */
	world = context;
	display = world->display;

	/* Takes a reference on the wells. */
	error = drv_i915_display_power_get(&display->power, functions);
	if (error != 0) {
		world->power_failures++;
		kern_logf("i915: TC: power functions 0x%llx did not come (error %d)\n", (unsigned long long)functions, error);
		return error;
	}

	/* Succeeded: the wells are on. */
	return 0;
}

/* Gives the wells of a set of functions back for the ports. */
static void
tc_port_power_put(
	void *context,
	uint64_t functions)
{
	struct i915_tc_port_world *world;
	struct i915_display *display;

	/* The ports' state and their display. */
	world = context;
	display = world->display;

	/* Gives the reference back. */
	drv_i915_display_power_put(&display->power, functions);
}

/* Takes a port's lock for the ports. */
static void
tc_port_lock(
	void *ctx,
	unsigned tc)
{
	struct i915_tc_port_world *world;

	/* Takes the port's mutex. */
	world = ctx;
	mutex_lock(&world->locks[tc]);
}

/* Releases a port's lock for the ports. */
static void
tc_port_unlock(
	void *ctx,
	unsigned tc)
{
	struct i915_tc_port_world *world;

	/* Releases the port's mutex. */
	world = ctx;
	mutex_unlock(&world->locks[tc]);
}

/* Reads the monotonic counter in microseconds for the ports: 0, or EIO when there is no counter. */
static int
tc_port_now_us(
	void *ctx,
	uint64_t *now)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t seconds;
	uint64_t remainder;
	bool read;

	UNUSED_PARAMETER(ctx);

	/* The counter and its rate; without them there is no time. */
	read = kern_rtc_read_counter(&counter, &frequency);
	if (!read)
		return EIO;
	if (frequency == 0U)
		return EIO;

	/*
	 * The whole seconds and the rest apart, so the conversion cannot
	 * overflow however long the counter has run.
	 */
	seconds = counter / frequency;
	remainder = counter % frequency;
	*now = seconds * I915_TC_PORT_US_PER_SECOND;
	*now += remainder * I915_TC_PORT_US_PER_SECOND / frequency;

	/* Succeeded: the time now. */
	return 0;
}

/*
 * Sleeps for the ports, at least the time asked for, giving the CPU to
 * other threads (the kernel's sleep lasts to a scheduler tick): 0.  A
 * failed time base ends the sleep early; the clock then reports it.
 */
static int
tc_port_sleep_us(
	void *ctx,
	unsigned us)
{
	UNUSED_PARAMETER(ctx);

	/* Sleeps, with no upper bound to keep. */
	kern_usleep_range(us, us + us);

	/* Succeeded: the time has passed, or the clock will say why not. */
	return 0;
}

/* Writes a line of the ports' log to the kernel log. */
static void
tc_port_log(
	void *ctx,
	const char *format,
	...)
{
	char line[I915_TC_PORT_LOG_LINE];
	va_list arguments;

	UNUSED_PARAMETER(ctx);

	/* Formats the line and writes it. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	kern_logf("%s", line);
}

/*
 * Declares the Type-C ports of the output setup's table, with the VBT's
 * legacy flag (a port that supports neither USB-C nor Thunderbolt has a
 * connector wired to it) and its AUX channel.
 */
static void
tc_port_declare(
	struct i915_display *display,
	struct i915_tc_port_world *world)
{
	const struct i915_xd_vbt_child *child;
	const struct i915_xd_port *port;
	unsigned index;
	int legacy;
	int error;
	int tc;

	/* Declares each Type-C port of the display. */
	for (index = 0U; index < display->xd.ports.n; index++) {
		/* Skips a port that is not a Type-C port. */
		port = &display->xd.ports.port[index];
		if (port->tc == 0U)
			continue;

		/* The VBT's child of the port, when the VBT was read. */
		child = NULL;
		if (display->xd.vbt_parsed)
			child = drv_i915_xd_vbt_child(&display->xd.vbt, port->ddi);

		/* A port with neither a USB-C nor a Thunderbolt receptacle has a connector wired to it. */
		legacy = 0;
		if (child != NULL &&
		    !child->type_c &&
		    !child->tbt)
			legacy = 1;

		/* Declares the port; one the platform does not have as Type-C is skipped. */
		error = drv_i915_xd_tc_ports_declare(&world->ports, port->ddi, legacy, port->aux);
		if (error != 0)
			continue;

		/* Logs the port as declared. */
		tc = drv_i915_xd_tc_ports_index(&world->ports, port->ddi);
		kern_logf("i915: TC%d: declared (DDI %c, legacy %d, AUX channel %u)\n", tc + 1, (char)('A' + port->ddi), legacy, (unsigned)port->aux);
	}
}

/*
 * Finds the Type-C ports of a screen's run for a hook that names a port,
 * or NULL, logged, when the run's display has no bound ports or the
 * number names none of them.
 */
static struct i915_xd_tc_ports *
tc_port_lcd_ports(
	struct i915_lcd_kernel *kernel,
	int tc_port,
	const char *what)
{
	/* A display without bound ports has none to use. */
	if (kernel->d == NULL || kernel->d->tc == NULL) {
		kern_logf("i915: TC: %s of TC%d without bound Type-C ports: nothing done\n", what, tc_port + 1);
		return NULL;
	}

	/* A number outside the ports names none. */
	if (tc_port < 0 || tc_port >= (int)I915_XD_TC_PORTS) {
		kern_logf("i915: TC: %s of a port that is not Type-C (%d): nothing done\n", what, tc_port);
		return NULL;
	}

	/* Succeeded: the run's ports. */
	return kernel->d->tc;
}

/*
 * Finds the Dekel PHY access of a screen's run for a hook that names a
 * port, or NULL, logged, when the run has none or the number names none
 * of the Type-C ports.
 */
static struct i915_xd_dkl *
tc_port_lcd_dkl(
	struct i915_lcd_kernel *kernel,
	int tc_port)
{
	/* A run without the display's power has no Dekel access. */
	if (kernel->d == NULL ||
	    kernel->d->power == NULL ||
	    kernel->d->power->dkl == NULL) {
		kern_logf("i915: TC: Dekel access of TC%d without the display's Dekel PHYs: nothing done\n", tc_port + 1);
		return NULL;
	}

	/* A number outside the ports names none. */
	if (tc_port < 0 || tc_port >= (int)I915_XD_TC_PORTS) {
		kern_logf("i915: TC: Dekel access of a port that is not Type-C (%d): nothing done\n", tc_port);
		return NULL;
	}

	/* Succeeded: the display's Dekel access. */
	return kernel->d->power->dkl;
}
