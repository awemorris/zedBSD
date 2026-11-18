/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Type-C ports of the xd display (display/xd-tc-port.c)
 * over a register record of its own: the live state, the readout of how
 * the firmware left a port, the connect's order and its unwinding, the
 * FIA's lanes and slots, the legacy flag's correction, the connected
 * answer, the link reset, what the Type-C layer is told, the FIA's main
 * link lanes, the legacy PHY's sleeping wait measured on the clock, and
 * the stop that gives every port back -- on Alder Lake-P (the TCSS flow)
 * -- and the same steps on Tiger Lake (the FIA flow, TC cold kept off by
 * the firmware's well).  The power wells are counted by function, so a
 * check says which well a step held.
 */

#include "host-test.h"

#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-tc.h"
#include "../../display/xd-tc-port.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The GPUs of the Latitude 5330 (Alder Lake-P) and of a Tiger Lake laptop. */
#define TC_PORT_TEST_ADLP		0x46a6U
#define TC_PORT_TEST_ADLP_REVID		0x0cU
#define TC_PORT_TEST_TGL		0x9a49U
#define TC_PORT_TEST_TGL_REVID		0x01U

/* How many registers and writes the record keeps, and the longest log line looked into. */
#define TC_PORT_TEST_REGS		64U
#define TC_PORT_TEST_WRITES		256U
#define TC_PORT_TEST_LINE		256U

/*
 * The device the ports run against: its registers, the writes in order,
 * each power function's references, a set of functions refused, each
 * port's lock, the clock and the sleeps, the sleep after which a port's
 * PHY becomes ready (0: never), and the log lines that told of a wait
 * that timed out.
 */
static struct {
	uint32_t reg[TC_PORT_TEST_REGS];
	uint32_t value[TC_PORT_TEST_REGS];
	unsigned regs;
	uint32_t write_reg[TC_PORT_TEST_WRITES];
	uint32_t write_value[TC_PORT_TEST_WRITES];
	unsigned writes;
	int refs[64];
	unsigned gets;
	uint64_t refuse;
	int locked[I915_XD_TC_PORTS];
	int lock_errors;
	uint64_t now_us;
	unsigned tick_us;
	unsigned sleeps;
	int clock_fails;
	unsigned ready_after;
	unsigned ready_port;
	unsigned timeouts;
} tc_port_test;

/* The register window and the platform every check uses. */
static struct i915_xd_io tc_port_test_io;
static const struct i915_xd_platform *tc_port_test_platform;

static void tc_port_test_reset(uint16_t device, uint8_t revid);
static uint32_t tc_port_test_get(uint32_t reg);
static void tc_port_test_set(uint32_t reg, uint32_t value);
static uint32_t tc_port_test_read(void *context, uint32_t offset);
static void tc_port_test_write(void *context, uint32_t offset, uint32_t value);
static void tc_port_test_wait(void *context, unsigned microseconds);
static void tc_port_test_io_lock(void *context, unsigned lock, int taken);
static int tc_port_test_power_get(void *context, uint64_t functions);
static void tc_port_test_power_put(void *context, uint64_t functions);
static void tc_port_test_lock(void *ctx, unsigned tc);
static void tc_port_test_unlock(void *ctx, unsigned tc);
static int tc_port_test_now(void *ctx, uint64_t *now);
static int tc_port_test_sleep(void *ctx, unsigned us);
static void tc_port_test_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void tc_port_test_bind(struct i915_xd_tc_ports *ports);
static unsigned tc_port_test_ddi(unsigned tc);
static void tc_port_test_declare(struct i915_xd_tc_ports *ports, unsigned tc, int legacy);
static int tc_port_test_held(uint64_t functions);
static int tc_port_test_balanced(uint64_t allowed);
static int tc_port_test_wrote(uint32_t reg, uint32_t mask, uint32_t value);
static uint32_t tc_port_test_buf(unsigned tc);
static uint64_t tc_port_test_cold(unsigned tc);
static void tc_port_test_hold_dp_alt(struct i915_xd_tc_ports *ports, unsigned tc);
static void tc_port_test_live(void);
static void tc_port_test_readout(void);
static void tc_port_test_connect(void);
static void tc_port_test_refusals(void);
static void tc_port_test_lanes_and_slots(void);
static void tc_port_test_legacy_and_connected(void);
static void tc_port_test_dp_sample(void);
static void tc_port_test_fia_lanes(void);
static void tc_port_test_wait_ready(void);
static void tc_port_test_stop(void);
static void tc_port_test_suspend(void);
static void tc_port_test_tiger_lake(void);

/*
 * Runs every check of the Type-C ports and reports.
 */
int
main(void)
{
	int failures;

	/* Each part of the ports. */
	tc_port_test_live();
	tc_port_test_readout();
	tc_port_test_connect();
	tc_port_test_refusals();
	tc_port_test_lanes_and_slots();
	tc_port_test_legacy_and_connected();
	tc_port_test_dp_sample();
	tc_port_test_fia_lanes();
	tc_port_test_wait_ready();
	tc_port_test_stop();
	tc_port_test_suspend();
	tc_port_test_tiger_lake();

	/* Reports the result. */
	failures = i915_host_report("host_xd_tc_port_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Empties the device and binds the window to it, for the platform of a GPU. */
static void
tc_port_test_reset(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* No register, write, power or lock. */
	memset(&tc_port_test, 0, sizeof(tc_port_test));

	/* The window reaches the record. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.read = tc_port_test_read;
	hooks.write = tc_port_test_write;
	hooks.wait = tc_port_test_wait;
	hooks.lock = tc_port_test_io_lock;
	drv_i915_xd_io_init(&tc_port_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&tc_port_test_io, &hooks);

	/* The description. */
	tc_port_test_platform = NULL;
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error == 0)
		tc_port_test_platform = choice.platform;
}

/* Reads a register of the record (0 when never set). */
static uint32_t
tc_port_test_get(
	uint32_t reg)
{
	unsigned index;

	/* The register's slot. */
	for (index = 0U; index < tc_port_test.regs; index++) {
		if (tc_port_test.reg[index] == reg)
			return tc_port_test.value[index];
	}

	/* Succeeded: an unset register reads zero. */
	return 0U;
}

/* Sets a register of the record. */
static void
tc_port_test_set(
	uint32_t reg,
	uint32_t value)
{
	unsigned index;

	/* An existing slot. */
	for (index = 0U; index < tc_port_test.regs; index++) {
		if (tc_port_test.reg[index] == reg) {
			tc_port_test.value[index] = value;
			return;
		}
	}

	/* A new slot. */
	if (tc_port_test.regs < TC_PORT_TEST_REGS) {
		tc_port_test.reg[tc_port_test.regs] = reg;
		tc_port_test.value[tc_port_test.regs] = value;
		tc_port_test.regs++;
	}
}

/* The window's read. */
static uint32_t
tc_port_test_read(
	void *context,
	uint32_t offset)
{
	uint32_t value;

	UNUSED_PARAMETER(context);

	/* Reads the record. */
	value = tc_port_test_get(offset);

	/* Succeeded: the value. */
	return value;
}

/* The window's write: recorded in order, then stored. */
static void
tc_port_test_write(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	(void)context;

	/* Records the write. */
	if (tc_port_test.writes < TC_PORT_TEST_WRITES) {
		tc_port_test.write_reg[tc_port_test.writes] = offset;
		tc_port_test.write_value[tc_port_test.writes] = value;
		tc_port_test.writes++;
	}

	/* Stores it. */
	tc_port_test_set(offset, value);
}

/* The window's wait: the clock moves on. */
static void
tc_port_test_wait(
	void *context,
	unsigned microseconds)
{
	(void)context;

	/* The time passes. */
	tc_port_test.now_us += microseconds;
}

/* The window's lock record: nothing to keep. */
static void
tc_port_test_io_lock(
	void *context,
	unsigned lock,
	int taken)
{
	(void)context;
	(void)lock;
	(void)taken;
}

/* The owner's wells: each function counted, a refused set refused whole. */
static int
tc_port_test_power_get(
	void *context,
	uint64_t functions)
{
	unsigned bit;

	UNUSED_PARAMETER(context);

	/* A refused set takes nothing. */
	if ((functions & tc_port_test.refuse) != 0U)
		return 5;

	/* One more reference on each function. */
	for (bit = 0U; bit < 64U; bit++) {
		if ((functions & (1ULL << bit)) != 0U)
			tc_port_test.refs[bit]++;
	}

	/* Counts the get. */
	tc_port_test.gets++;

	/* Succeeded: the wells are on. */
	return 0;
}

/* The owner's wells given back. */
static void
tc_port_test_power_put(
	void *context,
	uint64_t functions)
{
	unsigned bit;

	UNUSED_PARAMETER(context);

	/* One reference fewer on each function. */
	for (bit = 0U; bit < 64U; bit++) {
		if ((functions & (1ULL << bit)) != 0U)
			tc_port_test.refs[bit]--;
	}
}

/* A port's lock: a lock taken twice is an error. */
static void
tc_port_test_lock(
	void *ctx,
	unsigned tc)
{
	(void)ctx;

	/* Counts a recursive lock. */
	if (tc_port_test.locked[tc])
		tc_port_test.lock_errors++;
	tc_port_test.locked[tc] = 1;
}

/* A port's unlock: an unlock of a lock not taken is an error. */
static void
tc_port_test_unlock(
	void *ctx,
	unsigned tc)
{
	(void)ctx;

	/* Counts an unbalanced unlock. */
	if (!tc_port_test.locked[tc])
		tc_port_test.lock_errors++;
	tc_port_test.locked[tc] = 0;
}

/* The clock: the record's time, or a failure when the check asks. */
static int
tc_port_test_now(
	void *ctx,
	uint64_t *now)
{
	(void)ctx;

	/* A failed time base. */
	if (tc_port_test.clock_fails)
		return 5;

	/* Succeeded: the time now. */
	*now = tc_port_test.now_us;
	return 0;
}

/*
 * The sleep: the clock moves on by the time asked, or by a whole tick
 * when that is longer, and the port the check names becomes ready after
 * its sleep.
 */
static int
tc_port_test_sleep(
	void *ctx,
	unsigned us)
{
	(void)ctx;

	/* The time passes, at least a tick. */
	tc_port_test.now_us += us;
	if (tc_port_test.tick_us > us)
		tc_port_test.now_us += tc_port_test.tick_us - us;
	tc_port_test.sleeps++;

	/* The PHY the check waits for comes. */
	if (tc_port_test.ready_after != 0U && tc_port_test.sleeps == tc_port_test.ready_after)
		tc_port_test_set(I915_XD_TCSS_DDI_STATUS(tc_port_test.ready_port), I915_XD_TCSS_READY);

	/* Succeeded: slept. */
	return 0;
}

/* The log: lines that tell of a timed-out wait are counted; printed in a verbose run. */
static void
tc_port_test_log(
	void *ctx,
	const char *format,
	...)
{
	char line[TC_PORT_TEST_LINE];
	va_list arguments;
	const char *timeout;

	UNUSED_PARAMETER(ctx);

	/* The line, counted when it tells of a wait that timed out. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	timeout = strstr(line, "timeout");
	if (timeout != NULL)
		tc_port_test.timeouts++;

	/* The log, only in a verbose run. */
	if (i915_host_verbose)
		fputs(line, stdout);
}

/* Binds the ports to the record. */
static void
tc_port_test_bind(
	struct i915_xd_tc_ports *ports)
{
	struct i915_xd_tc_port_env env;
	struct i915_xd_tc_power power;

	/* The record's callbacks. */
	memset(&env, 0, sizeof(env));
	env.lock = tc_port_test_lock;
	env.unlock = tc_port_test_unlock;
	env.now_us = tc_port_test_now;
	env.sleep_us = tc_port_test_sleep;
	env.log = tc_port_test_log;
	power.get = tc_port_test_power_get;
	power.put = tc_port_test_power_put;
	power.context = NULL;
	drv_i915_xd_tc_ports_init(ports, &tc_port_test_io, tc_port_test_platform, &power, &env);
}

/* Finds the DDI of a Type-C port (0 for TC1) on the platform. */
static unsigned
tc_port_test_ddi(
	unsigned tc)
{
	const struct i915_xd_port_desc *desc;
	unsigned ddi;

	/* Every DDI of the platform. */
	for (ddi = 0U; ddi < 16U; ddi++) {
		desc = drv_i915_xd_platform_port(tc_port_test_platform, ddi);
		if (desc != NULL && desc->tc == tc + 1U)
			return ddi;
	}

	/* Succeeded: a DDI no port has. */
	return 0xffU;
}

/* Declares a Type-C port on its own AUX channel. */
static void
tc_port_test_declare(
	struct i915_xd_tc_ports *ports,
	unsigned tc,
	int legacy)
{
	const struct i915_xd_port_desc *desc;
	unsigned ddi;

	/* The port's DDI and channel. */
	ddi = tc_port_test_ddi(tc);
	desc = drv_i915_xd_platform_port(tc_port_test_platform, ddi);
	if (desc == NULL)
		return;

	/* Declares it. */
	(void)drv_i915_xd_tc_ports_declare(ports, ddi, legacy, desc->aux);
}

/* Tells how many references every function of a set has (the least of them). */
static int
tc_port_test_held(
	uint64_t functions)
{
	unsigned bit;
	int least;

	/* The least count of the set. */
	least = 1000;
	for (bit = 0U; bit < 64U; bit++) {
		if ((functions & (1ULL << bit)) != 0U && tc_port_test.refs[bit] < least)
			least = tc_port_test.refs[bit];
	}

	/* Succeeded: the count. */
	return least;
}

/* Tells whether every function but those allowed has no reference left. */
static int
tc_port_test_balanced(
	uint64_t allowed)
{
	unsigned bit;

	/* Every function. */
	for (bit = 0U; bit < 64U; bit++) {
		if ((allowed & (1ULL << bit)) != 0U)
			continue;
		if (tc_port_test.refs[bit] != 0)
			return 0;
	}

	/* Succeeded: balanced. */
	return 1;
}

/* Tells whether a write of a register had the bits of a mask at a value. */
static int
tc_port_test_wrote(
	uint32_t reg,
	uint32_t mask,
	uint32_t value)
{
	unsigned index;

	/* Any write of the register with those bits. */
	for (index = 0U; index < tc_port_test.writes; index++) {
		if (tc_port_test.write_reg[index] == reg && (tc_port_test.write_value[index] & mask) == value)
			return 1;
	}

	/* Succeeded: none. */
	return 0;
}

/* The DDI buffer word of a Type-C port. */
static uint32_t
tc_port_test_buf(
	unsigned tc)
{
	/* The port's DDI. */
	return I915_XD_DDI_BUF_CTL(tc_port_test_ddi(tc));
}

/* The functions that block TC cold for a held receptacle port. */
static uint64_t
tc_port_test_cold(
	unsigned tc)
{
	const struct i915_xd_port_desc *desc;

	/* The port's AUX channel's well, or the firmware's. */
	desc = drv_i915_xd_platform_port(tc_port_test_platform, tc_port_test_ddi(tc));

	/* Succeeded: the functions. */
	return drv_i915_xd_tc_cold_functions(tc_port_test_platform, desc->aux, I915_XD_TC_MODE_DP_ALT);
}

/* Marks a port as held in DisplayPort alternate mode, with its FIA, without any step. */
static void
tc_port_test_hold_dp_alt(
	struct i915_xd_tc_ports *ports,
	unsigned tc)
{
	/* The mode and the FIA. */
	ports->port[tc].connection.mode = I915_XD_TC_MODE_DP_ALT;
	(void)drv_i915_xd_tc_fia(&tc_port_test_io, tc_port_test_platform, tc, &ports->port[tc].connection.fia);
}

/* The live state: the hotplug word's Type-C and Thunderbolt bits and the south display's legacy bit of each port. */
static void
tc_port_test_live(void)
{
	struct i915_xd_tc_ports ports;
	unsigned live;

	/* TC2 with a DP-alt partner, TC3 with Thunderbolt, TC4 legacy. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	i915_host_check(tc_port_test_platform != NULL, "live: Alder Lake-P is described");
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 1U, 0);
	tc_port_test_declare(&ports, 2U, 0);
	tc_port_test_declare(&ports, 3U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(1U) | I915_XD_HPD_INT_TBT(2U));
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));

	/* Reads TC2's live state. */
	live = drv_i915_xd_tc_ports_live(&ports, 1U);
	i915_host_check(live == I915_XD_TC_LIVE_DP_ALT, "live: TC2 DP-alt");
	live = drv_i915_xd_tc_ports_live(&ports, 2U);
	i915_host_check(live == I915_XD_TC_LIVE_TBT, "live: TC3 Thunderbolt");
	live = drv_i915_xd_tc_ports_live(&ports, 3U);
	i915_host_check(live == I915_XD_TC_LIVE_LEGACY, "live: TC4 legacy");

	/* An undeclared port reads nothing, and no power was needed. */
	live = drv_i915_xd_tc_ports_live(&ports, 0U);
	i915_host_check(live == 0U, "live: undeclared TC1 reads nothing");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.gets == 0U, "live: no power on the TCSS flow");

	/* The DDI's Type-C port: TC1 to TC4 are DDIs D to G; a combo port is none. */
	i915_host_check(drv_i915_xd_tc_ports_index(&ports, tc_port_test_ddi(0U)) == 0 && drv_i915_xd_tc_ports_index(&ports, tc_port_test_ddi(3U)) == 3,
		"index: TC1 and TC4 by their DDIs");
	i915_host_check(drv_i915_xd_tc_ports_index(&ports, 0U) == -1, "index: DDI A is not a Type-C port");
}

/* The readout: a firmware output is kept with one link, anything else held is given back. */
static void
tc_port_test_readout(void)
{
	struct i915_xd_tc_ports ports;

	/* TC1 driven by the firmware in DP-alt: owned, ready, buffer enabled. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT, "readout: firmware output in DP-alt");
	i915_host_check(ports.port[0].links == 1U, "readout: the firmware output holds one link");
	i915_host_check(ports.port[0].connection.blocked == tc_port_test_cold(0U) && tc_port_test_held(tc_port_test_cold(0U)) == 1, "readout: TC cold blocked by TC1's AUX well");
	i915_host_check(!tc_port_test_wrote(tc_port_test_buf(0U), I915_XD_DDI_BUF_TC_OWNED, 0U), "readout: ownership kept");
	i915_host_check(tc_port_test_balanced(tc_port_test_cold(0U)), "readout: the lanes' well given back");

	/* The output's disable gives the link back: the PHY is given back at once. */
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE, "put_link: the last link gives the port back");
	i915_host_check(tc_port_test_held(tc_port_test_cold(0U)) == 0, "put_link: TC cold unblocked");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "put_link: ownership given back");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_ENABLE) != 0U, "put_link: the rest of the buffer control kept");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "put_link: power and locks balanced");

	/* TC2 left owned by the firmware without an output: given back at the readout. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 1U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(1U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(1U), I915_XD_TCSS_READY);
	tc_port_test_set(tc_port_test_buf(1U), I915_XD_DDI_BUF_TC_OWNED);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[1].connection.mode == I915_XD_TC_MODE_NONE && ports.port[1].links == 0U, "readout: an idle held port is given back");
	i915_host_check((tc_port_test_get(tc_port_test_buf(1U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "readout: its ownership cleared");
	i915_host_check(tc_port_test_balanced(0U), "readout: its TC cold unblocked");

	/* TC2 held by the firmware whose TC cold cannot be blocked: the PHY is given back, no link. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 1U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(1U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(1U), I915_XD_TCSS_READY);
	tc_port_test_set(tc_port_test_buf(1U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	tc_port_test.refuse = tc_port_test_cold(1U);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[1].connection.mode == I915_XD_TC_MODE_NONE && ports.port[1].links == 0U, "readout: no cold block, no link");
	i915_host_check((tc_port_test_get(tc_port_test_buf(1U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "readout: no cold block, ownership given back");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "readout: no cold block, power and locks balanced");

	/* TC1 with nothing plugged in and nothing owned: nothing written, nothing held. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && tc_port_test.writes == 0U, "readout: an empty port is left alone");
	i915_host_check(tc_port_test_balanced(0U), "readout: empty port power balanced");

	/* A port whose lanes' well does not come is not read. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	tc_port_test.refuse = I915_XD_POWERS_LANES(tc_port_test_ddi(0U));
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[0].links == 0U && tc_port_test.writes == 0U && tc_port_test_balanced(0U), "readout: no lanes' well, nothing read or written");
}

/* A connect for a DP-alt partner, in its order, and the unlock that gives the PHY back. */
static void
tc_port_test_connect(void)
{
	struct i915_xd_tc_ports ports;
	unsigned mode;
	unsigned own_write;
	unsigned index;
	uint32_t buffer_reg;

	/* TC1: a DP-alt partner offering two lanes (FIA1 slot 0, mask 0x3), the PHY ready. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0x3U);

	/* Locks TC1 for two lanes. */
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 2U);
	i915_host_check(mode == I915_XD_TC_MODE_DP_ALT, "connect: DP-alt for two lanes");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) != 0U, "connect: ownership taken");
	i915_host_check(tc_port_test_held(tc_port_test_cold(0U)) == 1, "connect: TC cold blocked");
	i915_host_check(tc_port_test.locked[0] == 1, "connect: the lock is held for the caller");
	i915_host_check(tc_port_test_balanced(tc_port_test_cold(0U)), "connect: the lanes' well given back after the ownership");

	/* The ownership is the first write. */
	own_write = TC_PORT_TEST_WRITES;
	buffer_reg = tc_port_test_buf(0U);
	for (index = 0U; index < tc_port_test.writes; index++) {
		if (tc_port_test.write_reg[index] == buffer_reg) {
			own_write = index;
			break;
		}
	}

	/* Checks the ownership is the first write. */
	i915_host_check(own_write == 0U, "connect: the ownership is the first write");

	/* The unlock without a link gives the PHY back at once. */
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE, "unlock: no link, the port is given back");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "unlock: ownership cleared");
	i915_host_check(tc_port_test_held(tc_port_test_cold(0U)) == 0, "unlock: TC cold unblocked");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "unlock: power and locks balanced");

	/* A link keeps the port held across the unlock; its put gives it back. */
	drv_i915_xd_tc_ports_get_link(&ports, 0U, 2U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT && ports.port[0].links == 1U, "get_link: held with one link");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) != 0U, "get_link: ownership kept after the unlock");
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && tc_port_test_held(tc_port_test_cold(0U)) == 0, "put_link: given back");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "get/put_link: power and locks balanced");
	i915_host_check(ports.port[0].connects == 2U && ports.port[0].disconnects == 2U, "connect: two connects and two disconnects counted");

	/* A link put back that was never taken is not counted below zero. */
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(ports.port[0].links == 0U && tc_port_test.lock_errors == 0, "put_link: a link never taken is not counted");
}

/* Refusals unwind what the connect took and fall back to the receptacle's default (Thunderbolt, nothing held). */
static void
tc_port_test_refusals(void)
{
	struct i915_xd_tc_ports ports;
	unsigned mode;

	/* The PHY not ready: ownership taken and given back, no cold block, Thunderbolt. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	i915_host_check(mode == I915_XD_TC_MODE_TBT, "refusal: not ready falls back to Thunderbolt");
	i915_host_check(tc_port_test_wrote(tc_port_test_buf(0U), I915_XD_DDI_BUF_TC_OWNED, I915_XD_DDI_BUF_TC_OWNED), "refusal: ownership was asked for");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "refusal: ownership given back");
	i915_host_check(tc_port_test_balanced(0U) && ports.port[0].connect_failures == 1U, "refusal: no cold block, one failure");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && tc_port_test_balanced(0U), "refusal: unlock leaves nothing held");

	/* Too few lanes: the cold block and the ownership unwound. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0x3U);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 4U);
	i915_host_check(mode == I915_XD_TC_MODE_TBT, "refusal: two lanes for four falls back");
	i915_host_check(tc_port_test_balanced(0U), "refusal: cold block unwound");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "refusal: ownership unwound");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);

	/* The cold power refused: the ownership unwound. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0xfU);
	tc_port_test.refuse = tc_port_test_cold(0U);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	i915_host_check(mode == I915_XD_TC_MODE_TBT, "refusal: cold power refused falls back");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "refusal: ownership unwound after the cold power");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "refusals: power and locks balanced");

	/* A TCSS in TC cold (all-ones) is not ready. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), 0xffffffffU);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	i915_host_check(mode == I915_XD_TC_MODE_TBT, "refusal: TCSS all-ones is not ready");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);

	/* A legacy port's refused connect leaves the port holding nothing (its default is its own mode). */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 1);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	tc_port_test.refuse = I915_XD_POWERS_LANES(tc_port_test_ddi(3U));
	mode = drv_i915_xd_tc_ports_lock(&ports, 3U, 4U);
	i915_host_check(mode == I915_XD_TC_MODE_NONE && ports.port[3].connect_failures == 1U, "refusal: a refused legacy port holds nothing");
	drv_i915_xd_tc_ports_unlock(&ports, 3U);
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "refusal: legacy power and locks balanced");
}

/* The FIA's lanes of each mask, and the slots of the four ports. */
static void
tc_port_test_lanes_and_slots(void)
{
	static const uint32_t masks[] = { 0x1U, 0x2U, 0x4U, 0x8U, 0x3U, 0xcU, 0xfU, 0x5U };
	static const unsigned lanes[] = { 1U, 1U, 1U, 1U, 2U, 2U, 4U, 1U };
	static const uint32_t bases[] = { I915_XD_FIA1_BASE, I915_XD_FIA1_BASE, I915_XD_FIA2_BASE, I915_XD_FIA2_BASE };
	struct i915_xd_tc_ports ports;
	struct i915_xd_tc_fia fia;
	unsigned index;
	unsigned counted;
	char what[64];
	int error;

	/* Each mask on TC2 (FIA1 slot 1, bits 8 to 11), the port held in DP-alt. */
	for (index = 0U; index < sizeof(masks) / sizeof(masks[0]); index++) {
		tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
		tc_port_test_bind(&ports);
		tc_port_test_declare(&ports, 1U, 0);
		tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, masks[index] << 8);
		tc_port_test_hold_dp_alt(&ports, 1U);
		counted = drv_i915_xd_tc_ports_max_lanes(&ports, 1U);
		(void)snprintf(what, sizeof(what), "lanes: mask 0x%x is %u", masks[index], lanes[index]);
		i915_host_check(counted == lanes[index], what);
	}

	/* Only DP-alt restricts the lanes. */
	ports.port[1].connection.mode = I915_XD_TC_MODE_LEGACY;
	counted = drv_i915_xd_tc_ports_max_lanes(&ports, 1U);
	i915_host_check(counted == 4U, "lanes: legacy has four");

	/* The modular FIAs: TC1 and TC2 on FIA1, TC3 and TC4 on FIA2, slots 0 and 1. */
	for (index = 0U; index < 4U; index++) {
		error = drv_i915_xd_tc_fia(&tc_port_test_io, tc_port_test_platform, index, &fia);
		(void)snprintf(what, sizeof(what), "slots: TC%u is FIA%u.%u", index + 1U, index / 2U + 1U, index % 2U);
		i915_host_check(error == 0 && fia.base == bases[index] && fia.index == index % 2U, what);
	}

	/* TC3's lanes are read from FIA2. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 2U, 0);
	tc_port_test_set(I915_XD_FIA2_BASE + I915_XD_FIA_DFLEXDPSP, 0xfU);
	tc_port_test_hold_dp_alt(&ports, 2U);
	counted = drv_i915_xd_tc_ports_max_lanes(&ports, 2U);
	i915_host_check(counted == 4U, "lanes: TC3 on FIA2");

	/* The pin assignment: TC2's field (bits 4 to 7) of FIA1's DFLEXPA1. */
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXPA1, 0x4U << 4);
	tc_port_test_declare(&ports, 1U, 0);
	i915_host_check(drv_i915_xd_tc_ports_pin(&ports, 1U) == 4U, "pins: TC2 pin D");
}

/* The legacy flag corrected by the live state, and what counts as connected. */
static void
tc_port_test_legacy_and_connected(void)
{
	struct i915_xd_tc_ports ports;
	unsigned mode;
	int connected;

	/* A port the VBT calls legacy with a DP-alt partner: the flag is corrected at the connect. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 1);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0xfU);
	(void)drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	i915_host_check(ports.port[0].legacy == 0 && ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT, "legacy: corrected to a receptacle");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);

	/* A receptacle with only a legacy bit live: the flag is corrected the other way. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 0);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(3U), I915_XD_TCSS_READY);
	mode = drv_i915_xd_tc_ports_lock(&ports, 3U, 4U);
	i915_host_check(ports.port[3].legacy == 1 && mode == I915_XD_TC_MODE_LEGACY, "legacy: corrected to a wired connector");
	drv_i915_xd_tc_ports_unlock(&ports, 3U);

	/* Held in no mode, a Thunderbolt partner counts as connected. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TBT(0U));
	connected = drv_i915_xd_tc_ports_connected(&ports, 0U);
	i915_host_check(connected == 1, "connected: anything plugged into a port held in no mode");

	/* Held in DP-alt, only a DP-alt partner counts. */
	ports.port[0].connection.mode = I915_XD_TC_MODE_DP_ALT;
	connected = drv_i915_xd_tc_ports_connected(&ports, 0U);
	i915_host_check(connected == 0, "connected: a DP-alt port does not count Thunderbolt");
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	connected = drv_i915_xd_tc_ports_connected(&ports, 0U);
	i915_host_check(connected == 1, "connected: a DP-alt port counts its partner");
	i915_host_check(tc_port_test.lock_errors == 0 && tc_port_test_balanced(0U), "connected: power and locks balanced");
	ports.port[0].connection.mode = I915_XD_TC_MODE_NONE;

	/*
	 * A probe holds a link for its AUX messages; each message locks the
	 * port and asks the locked answer, which takes no lock; the probe's
	 * last link gives the PHY back at once.
	 */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0xfU);
	drv_i915_xd_tc_ports_get_link(&ports, 0U, 1U);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	connected = drv_i915_xd_tc_ports_connected_locked(&ports, 0U);
	i915_host_check(mode == I915_XD_TC_MODE_DP_ALT && connected == 1, "connected_locked: the probe's port is DP-alt with its partner");
	i915_host_check(tc_port_test.lock_errors == 0, "connected_locked: no lock taken under the caller's");
	tc_port_test_set(I915_XD_DE_HPD_ISR, 0U);
	connected = drv_i915_xd_tc_ports_connected_locked(&ports, 0U);
	i915_host_check(connected == 0, "connected_locked: an unplugged partner");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT && (tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) != 0U,
		"probe link: the PHY stays between the messages");
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode != I915_XD_TC_MODE_DP_ALT && (tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U,
		"probe link: the last link gives the PHY back at once");
	i915_host_check(tc_port_test.lock_errors == 0 && tc_port_test_balanced(0U), "probe link: power and locks balanced");

	/* A link held in DP-alt needs a reset only while its partner is gone. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0xfU);
	i915_host_check(drv_i915_xd_tc_ports_link_needs_reset(&ports, 0U) == 0, "link reset: no link, no reset");
	drv_i915_xd_tc_ports_get_link(&ports, 0U, 4U);
	i915_host_check(drv_i915_xd_tc_ports_link_needs_reset(&ports, 0U) == 0, "link reset: the partner is there");
	tc_port_test_set(I915_XD_DE_HPD_ISR, 0U);
	i915_host_check(drv_i915_xd_tc_ports_link_needs_reset(&ports, 0U) == 1, "link reset: the partner went");
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	i915_host_check(drv_i915_xd_tc_ports_link_needs_reset(&ports, 0U) == 0, "link reset: the partner came back");
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(tc_port_test.lock_errors == 0 && tc_port_test_balanced(0U), "link reset: power and locks balanced");
}

/* What a port has of DisplayPort for the Type-C layer. */
static void
tc_port_test_dp_sample(void)
{
	struct i915_xd_tc_ports ports;
	struct i915_xd_tc_dp_sample sample;

	/* TC2 held in DP-alt with its partner live: pin D on two lanes (lanes 2-3). */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 1U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(1U));
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, 0xcU << 8);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXPA1, 0x4U << 4);
	tc_port_test_hold_dp_alt(&ports, 1U);
	drv_i915_xd_tc_ports_dp_sample(&ports, 1U, &sample);
	i915_host_check(sample.hpd == 1 && sample.pin == 4U && sample.lanes == 2U, "dp-sample: DP-alt pin D, two lanes");

	/* Not held in DP-alt: the FIA is not read, only the live state counts. */
	ports.port[1].connection.mode = I915_XD_TC_MODE_NONE;
	drv_i915_xd_tc_ports_dp_sample(&ports, 1U, &sample);
	i915_host_check(sample.hpd == 1 && sample.pin == 0U && sample.lanes == 0U, "dp-sample: no FIA read outside DP-alt");

	/* An undeclared port has nothing; the lock and the power stay balanced. */
	drv_i915_xd_tc_ports_dp_sample(&ports, 0U, &sample);
	i915_host_check(sample.hpd == 0 && sample.pin == 0U && sample.lanes == 0U, "dp-sample: undeclared port");
	i915_host_check(tc_port_test.lock_errors == 0 && tc_port_test_balanced(0U), "dp-sample: power and locks balanced");
}

/*
 * The FIA's DisplayPort main link lanes: the 5330's TC2 with four lanes
 * reads 0xf0 in FIA1's DFLEXDPMLE1 (as Linux left it), one and two lanes
 * from the near end and, reversed, from the far end, and the other slot
 * is kept.
 */
static void
tc_port_test_fia_lanes(void)
{
	struct i915_xd_tc_ports ports;
	uint32_t mle;

	/* TC2 with four lanes, TC1's slot holding two lanes. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 1U, 0);
	mle = I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPMLE1;
	tc_port_test_set(mle, 0x3U);
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 4U, 0);
	i915_host_check(tc_port_test_get(mle) == 0xf3U, "fia lanes: TC2 four lanes is 0xf in slot 1 (5330: 0xf0), TC1's slot kept");

	/* One and two lanes from the near end. */
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 1U, 0);
	i915_host_check(tc_port_test_get(mle) == 0x13U, "fia lanes: TC2 one lane is lane 0");
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 2U, 0);
	i915_host_check(tc_port_test_get(mle) == 0x33U, "fia lanes: TC2 two lanes are lanes 0 and 1");

	/* Reversed, from the far end. */
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 1U, 1);
	i915_host_check(tc_port_test_get(mle) == 0x83U, "fia lanes: TC2 one reversed lane is lane 3");
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 2U, 1);
	i915_host_check(tc_port_test_get(mle) == 0xc3U, "fia lanes: TC2 two reversed lanes are lanes 3 and 2");

	/* A lane count the FIA has not is logged and leaves no lanes. */
	drv_i915_xd_tc_ports_set_lanes(&ports, 1U, 3U, 0);
	i915_host_check(tc_port_test_get(mle) == 0x03U, "fia lanes: three lanes leaves none");

	/* TC3 is slot 0 of FIA2. */
	tc_port_test_declare(&ports, 2U, 0);
	drv_i915_xd_tc_ports_set_lanes(&ports, 2U, 4U, 0);
	i915_host_check(tc_port_test_get(I915_XD_FIA2_BASE + I915_XD_FIA_DFLEXDPMLE1) == 0x0fU, "fia lanes: TC3 four lanes is FIA2 slot 0");

	/* The 5330's TC2 pin assignment: FIA1's DFLEXPA1 0x30 is pin C. */
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXPA1, 0x30U);
	i915_host_check(drv_i915_xd_tc_ports_pin(&ports, 1U) == 3U, "pins: the 5330's TC2 DFLEXPA1 0x30 is pin C");
}

/*
 * The legacy port's wait for its PHY: it sleeps between the reads and
 * measures the half second on the clock, so a sleep that lasts a whole
 * tick ends it after the same time with fewer reads; a PHY that comes
 * ends it at once; a failed clock reads the bit once; a receptacle does
 * not wait.
 */
static void
tc_port_test_wait_ready(void)
{
	struct i915_xd_tc_ports ports;

	/* TC4 legacy, the PHY never ready, a sleep of a 10 ms tick: 50 sleeps, half a second. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test.tick_us = 10000U;
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 1);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(tc_port_test.sleeps == 50U, "wait: a 10 ms tick sleeps 50 times");
	i915_host_check(tc_port_test.now_us >= 500000U && tc_port_test.now_us < 510000U, "wait: half a second on the clock");
	i915_host_check(tc_port_test.timeouts == 1U, "wait: the timeout is logged once");
	i915_host_check(ports.port[3].connection.mode == I915_XD_TC_MODE_NONE, "wait: a PHY that never came is not held");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "wait: power and locks balanced");

	/* The same with sleeps of exactly the time asked (1 ms): 500 sleeps, half a second. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 1);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(tc_port_test.sleeps == 500U, "wait: a 1 ms sleep sleeps 500 times");
	i915_host_check(tc_port_test.now_us == 500000U, "wait: exactly half a second");

	/* The PHY comes after the third sleep: the wait ends there, no timeout. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test.tick_us = 10000U;
	tc_port_test.ready_after = 3U;
	tc_port_test.ready_port = 3U;
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 1);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(tc_port_test.sleeps == 3U, "wait: ends at the read after the PHY came");
	i915_host_check(tc_port_test.timeouts == 0U, "wait: no timeout when the PHY came");

	/* A failed clock: the bit is read once, no sleep, no endless wait. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test.clock_fails = 1;
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 3U, 1);
	tc_port_test_set(I915_XD_SDE_ISR, I915_XD_SDE_TC_HPD(3U));
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(tc_port_test.sleeps == 0U, "wait: a failed clock does not sleep");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "wait: a failed clock leaves power and locks balanced");

	/* A receptacle (not legacy) does not wait. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test.tick_us = 10000U;
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(tc_port_test.sleeps == 0U, "wait: a USB-C port does not wait");
}

/*
 * The driver's stop: a port the firmware's output holds with a link
 * gives its PHY and its TC cold block back, every port is retired, the
 * ports answer for none afterwards, and a second stop does nothing.
 */
static void
tc_port_test_stop(void)
{
	struct i915_xd_tc_ports ports;
	unsigned mode;
	unsigned writes;
	unsigned gets;

	/* TC1 driven by the firmware in DP-alt (one link, TC cold blocked), TC3 declared and empty. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_declare(&ports, 2U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[0].links == 1U && ports.port[0].connection.blocked != 0U, "stop: TC1 held with a link before the stop");

	/* The stop gives TC1 back although its link was not put back. */
	drv_i915_xd_tc_ports_stop(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && ports.port[0].links == 0U, "stop: TC1 holds nothing");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U, "stop: TC1's ownership given back");
	i915_host_check(tc_port_test_held(tc_port_test_cold(0U)) == 0 && ports.port[0].connection.blocked == 0U, "stop: TC1's TC cold unblocked");
	i915_host_check(!ports.port[0].present && !ports.port[2].present, "stop: every port retired");
	i915_host_check(!ports.live, "stop: the ports answer for none");
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "stop: power and locks balanced");

	/* Nothing after the stop takes a port again, writes or takes power. */
	writes = tc_port_test.writes;
	gets = tc_port_test.gets;
	drv_i915_xd_tc_ports_get_link(&ports, 0U, 4U);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 4U);
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	drv_i915_xd_tc_ports_stop(&ports);
	i915_host_check(mode == I915_XD_TC_MODE_NONE, "stop: a lock after the stop holds no mode");
	i915_host_check(tc_port_test.writes == writes && tc_port_test.gets == gets, "stop: nothing written or powered after the stop");
	i915_host_check(tc_port_test.lock_errors == 0, "stop: no lock taken after the stop");
}

/*
 * The suspend and the resume: the suspend gives back a port the
 * firmware's output held (its link dropped, logged), keeps the ports
 * declared and live, and a port that held nothing is left alone; the
 * resume reads the ports again as the start did, so a port the firmware
 * left held after the sleep is read out, and an empty port is not.
 */
static void
tc_port_test_suspend(void)
{
	struct i915_xd_tc_ports ports;
	unsigned writes;

	/* TC1 held by the firmware's output with one link, TC3 declared and empty. */
	tc_port_test_reset(TC_PORT_TEST_ADLP, TC_PORT_TEST_ADLP_REVID);
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	tc_port_test_declare(&ports, 2U, 0);
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(I915_XD_TCSS_DDI_STATUS(0U), I915_XD_TCSS_READY);
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	drv_i915_xd_tc_ports_readout(&ports);

	/* The suspend gives TC1 back and keeps the ports. */
	drv_i915_xd_tc_ports_suspend(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && ports.port[0].links == 0U, "suspend: TC1 holds nothing");
	i915_host_check((tc_port_test_get(tc_port_test_buf(0U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U && tc_port_test_balanced(0U), "suspend: TC1's PHY and TC cold given back");
	i915_host_check(ports.live && ports.port[0].present && ports.port[2].present, "suspend: the ports stay declared and live");
	i915_host_check(tc_port_test.lock_errors == 0, "suspend: locks balanced");

	/* A second suspend finds nothing held and writes nothing. */
	writes = tc_port_test.writes;
	drv_i915_xd_tc_ports_suspend(&ports);
	i915_host_check(tc_port_test.writes == writes, "suspend: a port that holds nothing is left alone");

	/* The resume with nothing plugged in reads the ports and holds nothing. */
	tc_port_test_set(I915_XD_DE_HPD_ISR, 0U);
	drv_i915_xd_tc_ports_resume(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && ports.port[2].connection.mode == I915_XD_TC_MODE_NONE && tc_port_test_balanced(0U),
		"resume: empty ports hold nothing");

	/* A resume that finds the firmware's output again keeps it with one link. */
	tc_port_test_set(I915_XD_DE_HPD_ISR, I915_XD_HPD_INT_TC(0U));
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_TC_OWNED);
	drv_i915_xd_tc_ports_resume(&ports);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT && ports.port[0].links == 1U, "resume: a held output is read out again");
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check(tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0, "resume: power and locks balanced");
}

/*
 * Tiger Lake's FIA flow: the live state is read from the FIA with TC
 * cold kept off by the firmware's well, a DP-alt partner is connected
 * with TC cold blocked first and the FIA's ownership bit, the unlock
 * gives both back, and the readout keeps the firmware's output.
 */
static void
tc_port_test_tiger_lake(void)
{
	struct i915_xd_tc_ports ports;
	unsigned live;
	unsigned mode;
	uint32_t csss;

	/* TC1 on a modular FIA1 (place 0): a DP-alt partner, the PHY ready, four lanes. */
	tc_port_test_reset(TC_PORT_TEST_TGL, TC_PORT_TEST_TGL_REVID);
	i915_host_check(tc_port_test_platform != NULL && tc_port_test_platform->tc_flow == I915_XD_TC_FLOW_FIA, "tgl: Tiger Lake is described with the FIA flow");
	tc_port_test_bind(&ports);
	tc_port_test_declare(&ports, 0U, 0);
	i915_host_check(ports.port[0].present, "tgl: TC1 declared");
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, I915_XD_DFLEXDPSP_MODULAR | I915_XD_DFLEXDPSP_LIVE_TC(0U) | 0xfU);
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPPMS, I915_XD_DFLEXDPPMS_READY(0U));
	csss = I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPCSSS;

	/* The live read keeps TC cold off for itself only. */
	live = drv_i915_xd_tc_ports_live(&ports, 0U);
	i915_host_check(live == I915_XD_TC_LIVE_DP_ALT, "tgl: the FIA reports a DP-alt partner");
	i915_host_check(tc_port_test.gets == 1U && tc_port_test_balanced(0U), "tgl: the live read held TC cold off and gave it back");

	/* The connect: TC cold blocked, the FIA's ownership bit, the lanes. */
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 4U);
	i915_host_check(mode == I915_XD_TC_MODE_DP_ALT, "tgl: DP-alt for four lanes");
	i915_host_check(ports.port[0].connection.blocked == I915_XD_POWERS_TC_COLD_OFF && tc_port_test_held(I915_XD_POWERS_TC_COLD_OFF) == 1,
		"tgl: TC cold blocked by the firmware's well");
	i915_host_check((tc_port_test_get(csss) & I915_XD_DFLEXDPCSSS_OWNED(0U)) != 0U, "tgl: the FIA's ownership bit set");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check((tc_port_test_get(csss) & I915_XD_DFLEXDPCSSS_OWNED(0U)) == 0U && tc_port_test_balanced(0U), "tgl: the unlock gives the PHY and TC cold back");
	i915_host_check(tc_port_test.lock_errors == 0, "tgl: locks balanced");

	/* The readout of the firmware's output: owned, ready, buffer enabled -- one link, TC cold kept blocked. */
	tc_port_test_set(csss, I915_XD_DFLEXDPCSSS_OWNED(0U));
	tc_port_test_set(tc_port_test_buf(0U), I915_XD_DDI_BUF_ENABLE);
	drv_i915_xd_tc_ports_readout(&ports);
	i915_host_check(ports.port[0].links == 1U && ports.port[0].connection.mode == I915_XD_TC_MODE_DP_ALT, "tgl: readout keeps the firmware's output");
	i915_host_check(tc_port_test_held(I915_XD_POWERS_TC_COLD_OFF) == 1 && tc_port_test_balanced(I915_XD_POWERS_TC_COLD_OFF), "tgl: readout holds TC cold off only");
	drv_i915_xd_tc_ports_put_link(&ports, 0U);
	i915_host_check((tc_port_test_get(csss) & I915_XD_DFLEXDPCSSS_OWNED(0U)) == 0U && tc_port_test_balanced(0U), "tgl: the output's put gives everything back");

	/* A receptacle with nothing plugged in: left to Thunderbolt for the step, given back at the unlock. */
	tc_port_test_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, I915_XD_DFLEXDPSP_MODULAR);
	mode = drv_i915_xd_tc_ports_lock(&ports, 0U, 1U);
	i915_host_check(mode == I915_XD_TC_MODE_TBT, "tgl: an empty receptacle is left to Thunderbolt");
	drv_i915_xd_tc_ports_unlock(&ports, 0U);
	i915_host_check(ports.port[0].connection.mode == I915_XD_TC_MODE_NONE && tc_port_test_balanced(0U) && tc_port_test.lock_errors == 0,
		"tgl: nothing held after the unlock");
}
