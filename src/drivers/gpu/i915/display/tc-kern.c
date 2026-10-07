/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What binds the display's Type-C ports to the device (see tc-kern.h).
 *
 * The Type-C core (tc.c) asks for registers, power, a lock per port, a
 * clock, a sleep and the log through struct i915_tc_env; this file answers
 * with the display's MMIO, its power domains, one mutex per port, the
 * kernel's monotonic counter, its sleep to the next tick and the kernel
 * log.  The start declares the Type-C ports the display probe made encoders
 * for, with the VBT's legacy flag and AUX channel, and reads how the
 * firmware left them; the stop gives every port back.
 */

#include "tc-kern.h"
#include "power.h"
#include "vbt-parse.h"

#include "../mmio.h"
#include "../sync.h"

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include <stdarg.h>

/* The display version whose Type-C ports the core knows (Alder Lake-P). */
#define I915_TC_KERN_DISPLAY_VER	13u

/* The longest line the core logs. */
#define I915_TC_KERN_LOG_LINE		256u

/* Microseconds in a second, for the counter's conversion. */
#define I915_TC_KERN_US_PER_SECOND	1000000u

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

static uint32_t tc_kern_read32(void *ctx, uint32_t reg);
static void tc_kern_write32(void *ctx, uint32_t reg, uint32_t value);
static enum i915_power_domain tc_kern_domain(struct i915_tc_kern *k, enum i915_tc_power power, unsigned port);
static int tc_kern_power_get(void *ctx, enum i915_tc_power power, unsigned port);
static void tc_kern_power_put(void *ctx, enum i915_tc_power power, unsigned port);
static void tc_kern_lock(void *ctx, unsigned port);
static void tc_kern_unlock(void *ctx, unsigned port);
static int tc_kern_now_us(void *ctx, uint64_t *now);
static int tc_kern_sleep_us(void *ctx, unsigned us);
static void tc_kern_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void tc_kern_declare_ports(struct i915_display *display, struct i915_tc_kern *k);
static struct i915_tc *tc_kern_lcd_ports(struct i915_lcd_kernel *kernel, int tc_port, const char *what);
static struct i915_dkl_phy *tc_kern_lcd_dkl(struct i915_lcd_kernel *kernel, int tc_port);

/*
 * Binds the display's Type-C ports and reads how the firmware left them.
 *
 * Runs once, after the display probe made its encoders and before the
 * hotplug path starts.  A display version other than 13 has no Type-C
 * ports here.
 */
void
drv_i915_tc_kern_start(
	struct i915_display *display,
	struct i915_mmio *mmio,
	unsigned display_ver)
{
	struct i915_tc_kern *k;
	struct i915_tc_env env;
	unsigned port;

	/* The display's binding of its Type-C ports. */
	k = &display->tck;

	/* Only display version 13's Type-C ports are known. */
	if (display_ver != I915_TC_KERN_DISPLAY_VER) {
		kern_logf("i915: TC: display version %u: the Type-C ports are not driven\n", display_ver);
		return;
	}

	/* What the ports use: the display's registers and power, and a lock each. */
	k->mmio = mmio;
	k->pd = &display->power_domains;
	k->pwc = &display->pwc;
	k->display_ver = display_ver;
	k->power_failures = 0u;
	for (port = 0u; port < I915_TC_PORTS; port++) {
		(void)mutex_init(&k->locks[port], LOCK_RANK_DEVICE, "i915 tc port");
		k->aux_ch[port] = I915_AUX_CH_USBC1 + (int)port;
	}

	/* The environment the core asks through. */
	kern_memset(&env, 0, sizeof(env));
	env.ctx = k;
	env.read32 = tc_kern_read32;
	env.write32 = tc_kern_write32;
	env.power_get = tc_kern_power_get;
	env.power_put = tc_kern_power_put;
	env.lock = tc_kern_lock;
	env.unlock = tc_kern_unlock;
	env.now_us = tc_kern_now_us;
	env.sleep_us = tc_kern_sleep_us;
	env.log = tc_kern_log;
	drv_i915_tc_init(&k->tc, &env, display_ver);

	/* The ports are bound from here on. */
	k->live = 1;

	/* Declares the ports the VBT names and reads how the firmware left them. */
	tc_kern_declare_ports(display, k);
	drv_i915_tc_readout(&k->tc);

	/* Tells the Type-C layer what each declared port has of DisplayPort after the readout. */
	for (port = 0u; port < I915_TC_PORTS; port++) {
		if (k->tc.port[port].present)
			drv_i915_tc_kern_report(&k->tc, port);
	}
}

/*
 * Gives the display's Type-C ports back to the Type-C subsystem when the
 * driver stops (intel_tc_port_cleanup()): every PHY the display owns and
 * every TC cold block it holds.
 *
 * Runs after the outputs were stopped and the hotplug path closed, while
 * the power domains still work; any later step on a port finds none.
 */
void
drv_i915_tc_kern_stop(
	struct i915_display *display)
{
	struct i915_tc_kern *k;
	unsigned port;

	/* A display whose ports were never bound has nothing to give back. */
	k = &display->tck;
	if (!k->live)
		return;

	/* Gives every port back; the core logs each one. */
	drv_i915_tc_stop(&k->tc);

	/* The Type-C layer forgets what the display reported of each port (ws177-p003); a kernel without it has none. */
	if (drv_typec_display_forget == NULL)
		return;
	for (port = 0u; port < I915_TC_PORTS; port++)
		(void)drv_typec_display_forget(port);
}

/*
 * Returns the display's Type-C ports, or NULL when they are not bound.
 */
struct i915_tc *
drv_i915_tc_kern_ports(
	struct i915_display *display)
{
	/* A display without bound ports has none to give. */
	if (!display->tck.live)
		return NULL;

	/* Succeeded: the ports. */
	return &display->tck.tc;
}

/*
 * Tells which Type-C port a DDI port is: 0 for TC1, or -1 for a port that
 * is not a Type-C port of display version 13.
 */
int
drv_i915_tc_kern_port_of(
	int port)
{
	/* TC1 to TC4 are DDI ports D to G. */
	if (port < I915_PORT_TC1)
		return -1;
	if (port >= I915_PORT_TC1 + (int)I915_TC_PORTS)
		return -1;

	/* Succeeded: the Type-C port's number. */
	return port - I915_PORT_TC1;
}

/*
 * Gives an output's link to a Type-C port back (the panel run's
 * tc_put_link hook, intel_tc_port_put_link()).
 *
 * The run's context is its struct i915_lcd_kernel; a run whose display has
 * no bound ports gives nothing back.
 */
void
drv_i915_lcd_tc_put_link(
	void *ctx,
	int tc_port)
{
	struct i915_lcd_kernel *kernel;

	/* Resolves the run the hook belongs to. */
	kernel = ctx;

	/* A display without bound ports, or a port it does not have, has no link. */
	if (kernel->d == NULL || kernel->d->tc == NULL) {
		kern_logf("i915: TC: put_link TC%d without bound Type-C ports: nothing given back\n", tc_port + 1);
		return;
	}

	/* A number outside the four ports names none. */
	if (tc_port < 0 || tc_port >= (int)I915_TC_PORTS)
		return;

	/* Gives the link back; the last one gives the PHY back at once. */
	drv_i915_tc_put_link(kernel->d->tc, (unsigned)tc_port);
}

/*
 * Takes an output's link to a Type-C port (the panel run's tc_get_link
 * hook, intel_tc_port_get_link()): the port takes its PHY for the lanes if
 * it does not hold it.
 */
void
drv_i915_lcd_tc_get_link(
	void *ctx,
	int tc_port,
	int required_lanes)
{
	struct i915_tc *tc;

	/* Finds the run's Type-C ports; without them nothing is taken. */
	tc = tc_kern_lcd_ports(ctx, tc_port, "get_link");
	if (tc == NULL)
		return;

	/* Takes the link. */
	drv_i915_tc_get_link(tc, (unsigned)tc_port, required_lanes);
}

/*
 * Reports the mode a Type-C port is held in (the panel run's tc_mode
 * hook): an enum i915_tc_mode, I915_TC_MODE_NONE without bound ports.
 */
int
drv_i915_lcd_tc_mode(
	void *ctx,
	int tc_port)
{
	struct i915_tc *tc;
	enum i915_tc_mode mode;

	/* Finds the run's Type-C ports; without them the port is held in no mode. */
	tc = tc_kern_lcd_ports(ctx, tc_port, "mode");
	if (tc == NULL)
		return (int)I915_TC_MODE_NONE;

	/* Samples the mode under the port's lock. */
	mode = drv_i915_tc_mode(tc, (unsigned)tc_port);

	/* Succeeded: the mode. */
	return (int)mode;
}

/*
 * Programs the FIA's DisplayPort lanes of a Type-C port (the panel run's
 * tc_set_fia_lane_count hook, intel_tc_port_set_fia_lane_count()).
 */
void
drv_i915_lcd_tc_set_fia_lane_count(
	void *ctx,
	int tc_port,
	int required_lanes,
	int lane_reversal)
{
	struct i915_tc *tc;

	/* Finds the run's Type-C ports; without them nothing is programmed. */
	tc = tc_kern_lcd_ports(ctx, tc_port, "set_fia_lane_count");
	if (tc == NULL)
		return;

	/* Programs the FIA. */
	drv_i915_tc_set_fia_lane_count(tc, (unsigned)tc_port, required_lanes, lane_reversal);
}

/*
 * Reads the FIA's DisplayPort pin assignment of a Type-C port (the panel
 * run's tc_pin_assignment hook, intel_tc_port_get_pin_assignment_mask()):
 * 0 without bound ports.
 */
unsigned
drv_i915_lcd_tc_pin_assignment(
	void *ctx,
	int tc_port)
{
	struct i915_tc *tc;
	unsigned pin_assignment;

	/* Finds the run's Type-C ports; without them there is no assignment. */
	tc = tc_kern_lcd_ports(ctx, tc_port, "pin_assignment");
	if (tc == NULL)
		return 0u;

	/* Reads the FIA. */
	pin_assignment = drv_i915_tc_pin_assignment(tc, (unsigned)tc_port);

	/* Succeeded: the pin assignment. */
	return pin_assignment;
}

/*
 * Reads a register of a Type-C port's DKL PHY (the panel run's dkl_read
 * hook, intel_dkl_phy_read()): all-ones, a PHY that does not answer, when
 * the run has no DKL access.
 */
uint32_t
drv_i915_lcd_dkl_read(
	void *ctx,
	int tc_port,
	uint32_t phy_address)
{
	struct i915_dkl_phy *dkl;
	uint32_t value;

	/* Finds the display's DKL access; without it the PHY does not answer. */
	dkl = tc_kern_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return 0xffffffffu;

	/* Reads under the bank index lock. */
	value = drv_i915_dkl_phy_read(dkl, (unsigned)tc_port, phy_address);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a register of a Type-C port's DKL PHY (the panel run's dkl_write
 * hook, intel_dkl_phy_write()).
 */
void
drv_i915_lcd_dkl_write(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t value)
{
	struct i915_dkl_phy *dkl;

	/* Finds the display's DKL access; without it nothing is written. */
	dkl = tc_kern_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return;

	/* Writes under the bank index lock. */
	drv_i915_dkl_phy_write(dkl, (unsigned)tc_port, phy_address, value);
}

/*
 * Clears and sets bits of a register of a Type-C port's DKL PHY (the panel
 * run's dkl_rmw hook, intel_dkl_phy_rmw()).
 */
void
drv_i915_lcd_dkl_rmw(
	void *ctx,
	int tc_port,
	uint32_t phy_address,
	uint32_t clear,
	uint32_t set)
{
	struct i915_dkl_phy *dkl;

	/* Finds the display's DKL access; without it nothing is changed. */
	dkl = tc_kern_lcd_dkl(ctx, tc_port);
	if (dkl == NULL)
		return;

	/* Changes the register under one hold of the bank index lock. */
	drv_i915_dkl_phy_rmw(dkl, (unsigned)tc_port, phy_address, clear, set);
}

/*
 * Tells the USB Type-C connector layer what a Type-C port has of
 * DisplayPort: the DP-alt hot plug detect, the pin assignment and the
 * lanes (the plug's orientation is not known to the display).
 *
 * Called from the display's start and its hotplug work, never from the
 * interrupt, and with the port's lock not held (the sample takes it).  A
 * kernel without the Type-C layer reports nothing.
 */
void
drv_i915_tc_kern_report(
	struct i915_tc *tc,
	unsigned port)
{
	struct i915_tc_dp_sample sample;
	struct drv_typec_dp_state state;
	int error;

	/* A kernel without the Type-C layer has no one to tell. */
	if (drv_typec_display_report == NULL)
		return;

	/* Reads the port. */
	drv_i915_tc_dp_sample(tc, port, &sample);

	/* The layer's form: the FIA numbers the pin assignments as the layer does (3 is C). */
	kern_memset(&state, 0, sizeof(state));
	state.known = true;
	if (sample.hpd)
		state.hpd = true;
	state.pin = DRV_TYPEC_DP_PIN_NONE;
	if (sample.pin <= (unsigned)DRV_TYPEC_DP_PIN_F)
		state.pin = (enum drv_typec_dp_pin)sample.pin;
	state.lanes = (unsigned)sample.lanes;
	state.orientation = DRV_TYPEC_ORIENTATION_UNKNOWN;

	/* Tells the layer; a refusal is only logged. */
	error = drv_typec_display_report(port, &state);
	if (error != 0)
		kern_logf("i915: TC%u: the Type-C layer refused the DisplayPort report (error %d)\n", port + 1u, error);
}

/* Reads a display register for the core. */
static uint32_t
tc_kern_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_tc_kern *k;
	uint32_t value;

	/* Reads the display's register directly, as the display does. */
	k = ctx;
	value = drv_i915_raw_read32(k->mmio, reg);

	/* Succeeded: the value. */
	return value;
}

/* Writes a display register for the core. */
static void
tc_kern_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_tc_kern *k;

	/* Writes the display's register directly, as the display does. */
	k = ctx;
	drv_i915_raw_write32(k->mmio, reg, value);
}

/* Names the power domain of a port's power. */
static enum i915_power_domain
tc_kern_domain(
	struct i915_tc_kern *k,
	enum i915_tc_power power,
	unsigned port)
{
	enum i915_power_domain domain;

	/* Picks the domain of the power. */
	switch (power) {
	case I915_TC_POWER_PORT:
		/* The port's DDI lanes. */
		return (enum i915_power_domain)(I915_PW_DOMAIN_PORT_DDI_LANES_TC1 + port);
	case I915_TC_POWER_COLD:
		/* The AUX_USBC domain of the port's AUX channel, which blocks TC cold. */
		domain = drv_i915_aux_legacy_power_domain(k->display_ver, k->aux_ch[port]);
		return domain;
	default:
		break;
	}

	/* Succeeded: the display core. */
	return I915_PW_DOMAIN_DISPLAY_CORE;
}

/* Takes a port's power for the core: 0, or the power domains' error. */
static int
tc_kern_power_get(
	void *ctx,
	enum i915_tc_power power,
	unsigned port)
{
	struct i915_tc_kern *k;
	enum i915_power_domain domain;
	int error;

	/* Names the domain of the power. */
	k = ctx;
	domain = tc_kern_domain(k, power, port);

	/* Takes a reference on the domain. */
	error = drv_i915_display_power_get(k->pd, domain, k->pwc);
	if (error != 0) {
		k->power_failures++;
		kern_logf("i915: TC%u: power domain %d did not come (error %d)\n", port + 1u, (int)domain, error);
		return error;
	}

	/* Succeeded: the power is on. */
	return 0;
}

/* Gives a port's power back for the core. */
static void
tc_kern_power_put(
	void *ctx,
	enum i915_tc_power power,
	unsigned port)
{
	struct i915_tc_kern *k;
	enum i915_power_domain domain;

	/* Names the domain of the power. */
	k = ctx;
	domain = tc_kern_domain(k, power, port);

	/* Gives the reference back. */
	drv_i915_display_power_put(k->pd, domain, k->pwc);
}

/* Takes a port's lock for the core. */
static void
tc_kern_lock(
	void *ctx,
	unsigned port)
{
	struct i915_tc_kern *k;

	/* Takes the port's mutex. */
	k = ctx;
	mutex_lock(&k->locks[port]);
}

/* Releases a port's lock for the core. */
static void
tc_kern_unlock(
	void *ctx,
	unsigned port)
{
	struct i915_tc_kern *k;

	/* Releases the port's mutex. */
	k = ctx;
	mutex_unlock(&k->locks[port]);
}

/* Reads the monotonic counter in microseconds for the core: 0, or EIO when there is no counter. */
static int
tc_kern_now_us(
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
	if (frequency == 0u)
		return EIO;

	/*
	 * The whole seconds and the rest apart, so the conversion cannot
	 * overflow however long the counter has run.
	 */
	seconds = counter / frequency;
	remainder = counter % frequency;
	*now = seconds * I915_TC_KERN_US_PER_SECOND;
	*now += remainder * I915_TC_KERN_US_PER_SECOND / frequency;

	/* Succeeded: the time now. */
	return 0;
}

/*
 * Sleeps for the core, at least the time asked for, giving the CPU to other
 * threads (the kernel's sleep lasts to a scheduler tick): 0.  A failed time
 * base ends the sleep early; the core's clock then reports it.
 */
static int
tc_kern_sleep_us(
	void *ctx,
	unsigned us)
{
	UNUSED_PARAMETER(ctx);

	/* Sleeps, with no upper bound to keep. */
	kern_usleep_range(us, us + us);

	/* Succeeded: the time has passed, or the clock will say why not. */
	return 0;
}

/* Writes a line of the core's log to the kernel log. */
static void
tc_kern_log(
	void *ctx,
	const char *format,
	...)
{
	char line[I915_TC_KERN_LOG_LINE];
	va_list arguments;

	UNUSED_PARAMETER(ctx);

	/* Formats the line and writes it. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	kern_logf("%s", line);
}

/*
 * Declares the Type-C ports the display probe made encoders for, with the
 * VBT's legacy flag (a port that supports neither USB-C nor Thunderbolt has
 * a connector wired to it) and its AUX channel.
 */
static void
tc_kern_declare_ports(
	struct i915_display *display,
	struct i915_tc_kern *k)
{
	const struct i915_encoder *encoder;
	const struct i915_vbt_encoder *child;
	unsigned index;
	int tc_port;
	int legacy;

	/* Declares the port of each Type-C encoder. */
	for (index = 0u; index < display->nogem.num_encoders; index++) {
		/* Skips an encoder that is not a Type-C port's. */
		encoder = &display->nogem.encoders[index];
		if (!encoder->is_tc)
			continue;

		/* Skips a Type-C encoder outside TC1 to TC4. */
		tc_port = drv_i915_tc_kern_port_of(encoder->port);
		if (tc_port < 0)
			continue;

		/* Without the VBT's word the port is a USB-C receptacle on its own AUX channel. */
		legacy = 0;
		child = NULL;
		if (display->vbt_state.parsed_live)
			child = drv_i915_vbt_encoder_for_port(&display->vbt_state.parsed, encoder->port);

		/* The VBT's child says whether the port has a receptacle, and its AUX channel. */
		if (child != NULL) {
			/* A port with neither a USB-C nor a Thunderbolt receptacle has a connector wired to it. */
			if (!child->supports_typec_usb && !child->supports_tbt)
				legacy = 1;

			/* The VBT's AUX channel, when it names one. */
			if (child->aux_ch >= 0)
				k->aux_ch[tc_port] = child->aux_ch;
		}

		/* Declares the port. */
		drv_i915_tc_declare(&k->tc, (unsigned)tc_port, legacy);
		kern_logf("i915: TC%d: declared (DDI %c, legacy %d, AUX channel %d)\n", tc_port + 1, (char)('A' + encoder->port), legacy, k->aux_ch[tc_port]);
	}
}

/*
 * Finds the Type-C ports of a panel run for a hook that names a port, or
 * NULL, logged, when the run's display has no bound ports or the number
 * names none of the four.
 */
static struct i915_tc *
tc_kern_lcd_ports(
	struct i915_lcd_kernel *kernel,
	int tc_port,
	const char *what)
{
	/* A display without bound ports has none to use. */
	if (kernel->d == NULL || kernel->d->tc == NULL) {
		kern_logf("i915: TC: %s of TC%d without bound Type-C ports: nothing done\n", what, tc_port + 1);
		return NULL;
	}

	/* A number outside the four ports names none. */
	if (tc_port < 0 || tc_port >= (int)I915_TC_PORTS) {
		kern_logf("i915: TC: %s of a port that is not Type-C (%d): nothing done\n", what, tc_port);
		return NULL;
	}

	/* Succeeded: the run's ports. */
	return kernel->d->tc;
}

/*
 * Finds the DKL PHY access of a panel run's display for a hook that names
 * a port, or NULL, logged, when the run has none or the number names none
 * of the four Type-C ports.
 */
static struct i915_dkl_phy *
tc_kern_lcd_dkl(
	struct i915_lcd_kernel *kernel,
	int tc_port)
{
	/* A run without the display's power context has no DKL access. */
	if (kernel->d == NULL || kernel->d->pwc == NULL || kernel->d->pwc->dkl == NULL) {
		kern_logf("i915: TC: DKL access of TC%d without the display's DKL PHYs: nothing done\n", tc_port + 1);
		return NULL;
	}

	/* A number outside the four ports names none. */
	if (tc_port < 0 || tc_port >= (int)I915_TC_PORTS) {
		kern_logf("i915: TC: DKL access of a port that is not Type-C (%d): nothing done\n", tc_port);
		return NULL;
	}

	/* Succeeded: the display's DKL access. */
	return kernel->d->pwc->dkl;
}
