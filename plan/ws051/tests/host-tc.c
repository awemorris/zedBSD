/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws051-p002b: the host test of the Type-C core (src/drivers/gpu/i915/display/tc.c)
 * over fake registers: the live status, the readout of how the firmware left
 * a port, the connect's order and its unwinding, the FIA's lanes and slots,
 * the legacy flag's correction, the connected answer, and the balance of the
 * power the ports take; the legacy PHY's sleeping wait measured on the clock
 * and the driver's stop that gives every port back (ws177-p002).
 *
 *   sh plan/ws051/tests/host-tc.sh
 */

#include "tc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The longest log line the fake looks into. */
#define FAKE_LOG_LINE	256

/* How many registers and writes the fake keeps. */
#define FAKE_REGS	64
#define FAKE_WRITES	256

/* The registers the core reads (tc.c's offsets). */
#define REG_DE_HPD_ISR		0x44470u
#define REG_SDEISR		0xc4000u
#define REG_TCSS(n)		(0x161500u + 4u * (n))
#define REG_DDI_BUF_CTL(n)	(0x64000u + 0x100u * (3u + (n)))
#define REG_FIA1_DPSP		(0x163000u + 0x8a0u)
#define REG_FIA2_DPSP		(0x16e000u + 0x8a0u)
#define REG_FIA1_PA1		(0x163000u + 0x880u)
#define REG_FIA1_MLE1		(0x163000u + 0x8c0u)
#define REG_FIA2_MLE1		(0x16e000u + 0x8c0u)

/* The bits. */
#define TCSS_READY		(1u << 2)
#define BUF_ENABLE		(1u << 31)
#define BUF_OWNED		(1u << 6)

/*
 * The fake device: its registers, the writes in order, each power's
 * references, and the lock's depth.
 */
struct fake {
	uint32_t reg[FAKE_REGS];
	uint32_t value[FAKE_REGS];
	unsigned regs;
	uint32_t write_reg[FAKE_WRITES];
	uint32_t write_value[FAKE_WRITES];
	unsigned writes;
	int power[3][I915_TC_PORTS];
	unsigned power_gets;
	int locked[I915_TC_PORTS];
	int lock_errors;
	int fail_cold;
	/*
	 * The clock: the time now, how long one sleep lasts at least (the
	 * kernel's tick; 0 for exactly the time asked), the sleeps taken, a
	 * clock that fails, and the sleep after which a port's PHY becomes
	 * ready (0: never).
	 */
	uint64_t now_us;
	unsigned tick_us;
	unsigned sleeps;
	int clock_fails;
	unsigned ready_after;
	unsigned ready_port;
	/* The log lines that said a wait timed out. */
	unsigned timeouts;
};

/* The fake every test uses, reset by fake_reset. */
static struct fake fake;

/* How many checks ran and failed. */
static unsigned checks;
static unsigned failures;

static void check(int condition, const char *what);
static void fake_reset(void);
static uint32_t fake_get(uint32_t reg);
static void fake_set(uint32_t reg, uint32_t value);
static uint32_t env_read32(void *ctx, uint32_t reg);
static void env_write32(void *ctx, uint32_t reg, uint32_t value);
static int env_power_get(void *ctx, enum i915_tc_power power, unsigned port);
static void env_power_put(void *ctx, enum i915_tc_power power, unsigned port);
static void env_lock(void *ctx, unsigned port);
static void env_unlock(void *ctx, unsigned port);
static int env_now_us(void *ctx, uint64_t *now);
static int env_sleep_us(void *ctx, unsigned us);
static void env_log(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void bind(struct i915_tc *tc);
static int power_balanced(void);
static int wrote(uint32_t reg, uint32_t mask, uint32_t value);
static void test_live_status(void);
static void test_readout(void);
static void test_connect(void);
static void test_refusals(void);
static void test_lanes_and_slots(void);
static void test_legacy_and_connected(void);
static void test_dp_sample(void);
static void test_fia_lane_count(void);
static void test_wait_ready(void);
static void test_stop(void);

/*
 * Runs every test and reports the count.
 */
int
main(void)
{
	/* Each part of the core. */
	test_live_status();
	test_readout();
	test_connect();
	test_refusals();
	test_lanes_and_slots();
	test_legacy_and_connected();
	test_dp_sample();
	test_fia_lane_count();
	test_wait_ready();
	test_stop();

	/* The summary the script reads. */
	printf("ws051-p002b host-tc checks=%u failures=%u\n", checks, failures);
	if (failures != 0u)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports a failed one. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check; a failed one is named. */
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Empties the fake. */
static void
fake_reset(void)
{
	/* No register, write, power or lock. */
	memset(&fake, 0, sizeof(fake));
}

/* Reads a fake register (0 when never set). */
static uint32_t
fake_get(
	uint32_t reg)
{
	unsigned index;

	/* The register's slot. */
	for (index = 0u; index < fake.regs; index++) {
		if (fake.reg[index] == reg)
			return fake.value[index];
	}

	/* Succeeded: an unset register reads zero. */
	return 0u;
}

/* Sets a fake register. */
static void
fake_set(
	uint32_t reg,
	uint32_t value)
{
	unsigned index;

	/* An existing slot. */
	for (index = 0u; index < fake.regs; index++) {
		if (fake.reg[index] == reg) {
			fake.value[index] = value;
			return;
		}
	}

	/* A new slot. */
	if (fake.regs < FAKE_REGS) {
		fake.reg[fake.regs] = reg;
		fake.value[fake.regs] = value;
		fake.regs++;
	}
}

/* The environment's register read. */
static uint32_t
env_read32(
	void *ctx,
	uint32_t reg)
{
	uint32_t value;

	(void)ctx;

	/* Reads the fake. */
	value = fake_get(reg);

	/* Succeeded: the value. */
	return value;
}

/* The environment's register write: recorded, then stored. */
static void
env_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	(void)ctx;

	/* Records the write in order. */
	if (fake.writes < FAKE_WRITES) {
		fake.write_reg[fake.writes] = reg;
		fake.write_value[fake.writes] = value;
		fake.writes++;
	}

	/* Stores it. */
	fake_set(reg, value);
}

/* The environment's power get: counted, and the cold power refused when the test asks. */
static int
env_power_get(
	void *ctx,
	enum i915_tc_power power,
	unsigned port)
{
	(void)ctx;

	/* A refused cold power. */
	if (power == I915_TC_POWER_COLD && fake.fail_cold)
		return 5;

	/* One more reference. */
	fake.power[power][port]++;
	fake.power_gets++;

	/* Succeeded: the power is on. */
	return 0;
}

/* The environment's power put. */
static void
env_power_put(
	void *ctx,
	enum i915_tc_power power,
	unsigned port)
{
	(void)ctx;

	/* One reference fewer. */
	fake.power[power][port]--;
}

/* The environment's lock: a lock taken twice is an error. */
static void
env_lock(
	void *ctx,
	unsigned port)
{
	(void)ctx;

	/* Counts a recursive lock. */
	if (fake.locked[port])
		fake.lock_errors++;
	fake.locked[port] = 1;
}

/* The environment's unlock: an unlock of a lock not taken is an error. */
static void
env_unlock(
	void *ctx,
	unsigned port)
{
	(void)ctx;

	/* Counts an unbalanced unlock. */
	if (!fake.locked[port])
		fake.lock_errors++;
	fake.locked[port] = 0;
}

/* The environment's clock: the fake's time, or a failure when the test asks. */
static int
env_now_us(
	void *ctx,
	uint64_t *now)
{
	(void)ctx;

	/* A failed time base. */
	if (fake.clock_fails)
		return 5;

	/* Succeeded: the time now. */
	*now = fake.now_us;
	return 0;
}

/*
 * The environment's sleep: the clock moves on by the time asked, or by a
 * whole tick when that is longer, and the port the test names becomes
 * ready after its sleep.
 */
static int
env_sleep_us(
	void *ctx,
	unsigned us)
{
	(void)ctx;

	/* The time passes, at least a tick. */
	fake.now_us += us;
	if (fake.tick_us > us)
		fake.now_us += fake.tick_us - us;
	fake.sleeps++;

	/* The PHY the test waits for comes. */
	if (fake.ready_after != 0u && fake.sleeps == fake.ready_after)
		fake_set(REG_TCSS(fake.ready_port), TCSS_READY);

	/* Succeeded: slept. */
	return 0;
}

/* The environment's log: printed when HOST_TC_VERBOSE is defined. */
static void
env_log(
	void *ctx,
	const char *format,
	...)
{
	va_list arguments;
	char line[FAKE_LOG_LINE];
	const char *timeout;

	(void)ctx;

	/* The line, counted when it tells of a wait that timed out. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	timeout = strstr(line, "timeout");
	if (timeout != NULL)
		fake.timeouts++;

	/* The log, only in the verbose build. */
#ifdef HOST_TC_VERBOSE
	fputs(line, stdout);
#endif
}

/* Binds the core to the fake. */
static void
bind(
	struct i915_tc *tc)
{
	struct i915_tc_env env;

	/* The fake's callbacks. */
	memset(&env, 0, sizeof(env));
	env.read32 = env_read32;
	env.write32 = env_write32;
	env.power_get = env_power_get;
	env.power_put = env_power_put;
	env.lock = env_lock;
	env.unlock = env_unlock;
	env.now_us = env_now_us;
	env.sleep_us = env_sleep_us;
	env.log = env_log;
	drv_i915_tc_init(tc, &env, 13u);
}

/* Tells whether every power taken was given back except a port's held cold power. */
static int
power_balanced(void)
{
	unsigned port;

	/* The core and the port power are only ever held during a step. */
	for (port = 0u; port < I915_TC_PORTS; port++) {
		if (fake.power[I915_TC_POWER_CORE][port] != 0)
			return 0;
		if (fake.power[I915_TC_POWER_PORT][port] != 0)
			return 0;
	}

	/* Succeeded: balanced. */
	return 1;
}

/* Tells whether a write of a register had the bits of a mask at a value. */
static int
wrote(
	uint32_t reg,
	uint32_t mask,
	uint32_t value)
{
	unsigned index;

	/* Any write of the register with those bits. */
	for (index = 0u; index < fake.writes; index++) {
		if (fake.write_reg[index] == reg && (fake.write_value[index] & mask) == value)
			return 1;
	}

	/* Succeeded: none. */
	return 0;
}

/* The live status: the DE ISR's DP-alt and Thunderbolt bits and the SDE ISR's legacy bit of each port. */
static void
test_live_status(void)
{
	struct i915_tc tc;
	uint32_t live;

	/* TC2 with a DP-alt partner (bit 17), TC3 with Thunderbolt (bit 2), TC4 legacy (SDE bit 27). */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 1u, 0);
	drv_i915_tc_declare(&tc, 2u, 0);
	drv_i915_tc_declare(&tc, 3u, 0);
	fake_set(REG_DE_HPD_ISR, (1u << 17) | (1u << 2));
	fake_set(REG_SDEISR, 1u << 27);

	live = drv_i915_tc_live_status(&tc, 1u);
	check(live == I915_TC_LIVE_DP_ALT, "live: TC2 DP-alt");
	live = drv_i915_tc_live_status(&tc, 2u);
	check(live == I915_TC_LIVE_TBT, "live: TC3 Thunderbolt");
	live = drv_i915_tc_live_status(&tc, 3u);
	check(live == I915_TC_LIVE_LEGACY, "live: TC4 legacy");

	/* An undeclared port reads nothing, and every core power came back. */
	live = drv_i915_tc_live_status(&tc, 0u);
	check(live == 0u, "live: undeclared TC1 reads nothing");
	check(power_balanced(), "live: power balanced");
}

/* The readout: a firmware output is kept with one link, anything else held is given back. */
static void
test_readout(void)
{
	struct i915_tc tc;

	/* TC1 driven by the firmware in DP-alt: owned, ready, buffer enabled. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_DDI_BUF_CTL(0u), BUF_ENABLE | BUF_OWNED);
	drv_i915_tc_readout(&tc);
	check(tc.port[0].mode == I915_TC_MODE_DP_ALT, "readout: firmware output in DP-alt");
	check(tc.port[0].links == 1u, "readout: the firmware output holds one link");
	check(tc.port[0].cold_held && fake.power[I915_TC_POWER_COLD][0] == 1, "readout: TC cold blocked");
	check(!wrote(REG_DDI_BUF_CTL(0u), BUF_OWNED, 0u), "readout: ownership kept");

	/* The output's disable gives the link back: the PHY is given back at once. */
	drv_i915_tc_put_link(&tc, 0u);
	check(tc.port[0].mode == I915_TC_MODE_NONE, "put_link: the last link gives the port back");
	check(fake.power[I915_TC_POWER_COLD][0] == 0, "put_link: TC cold unblocked");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "put_link: ownership given back");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_ENABLE) != 0u, "put_link: the rest of the buffer control kept");
	check(power_balanced() && fake.lock_errors == 0, "put_link: power and locks balanced");

	/* TC2 left owned by the firmware without an output: given back at the readout. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 1u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 17);
	fake_set(REG_TCSS(1u), TCSS_READY);
	fake_set(REG_DDI_BUF_CTL(1u), BUF_OWNED);
	drv_i915_tc_readout(&tc);
	check(tc.port[1].mode == I915_TC_MODE_NONE && tc.port[1].links == 0u, "readout: an idle held port is given back");
	check((fake_get(REG_DDI_BUF_CTL(1u)) & BUF_OWNED) == 0u, "readout: its ownership cleared");
	check(fake.power[I915_TC_POWER_COLD][1] == 0, "readout: its TC cold unblocked");

	/* TC2 held by the firmware whose TC cold cannot be blocked: the PHY is given back, no link. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 1u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 17);
	fake_set(REG_TCSS(1u), TCSS_READY);
	fake_set(REG_DDI_BUF_CTL(1u), BUF_ENABLE | BUF_OWNED);
	fake.fail_cold = 1;
	drv_i915_tc_readout(&tc);
	check(tc.port[1].mode == I915_TC_MODE_NONE && tc.port[1].links == 0u, "readout: no cold block, no link");
	check((fake_get(REG_DDI_BUF_CTL(1u)) & BUF_OWNED) == 0u, "readout: no cold block, ownership given back");
	check(power_balanced() && fake.lock_errors == 0, "readout: no cold block, power and locks balanced");

	/* TC1 with nothing plugged in and nothing owned: nothing written, nothing held. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	drv_i915_tc_readout(&tc);
	check(tc.port[0].mode == I915_TC_MODE_NONE && fake.writes == 0u, "readout: an empty port is left alone");
	check(power_balanced() && fake.power[I915_TC_POWER_COLD][0] == 0, "readout: empty port power balanced");
}

/* A connect for a DP-alt partner, in its order, and the unlock that gives the PHY back. */
static void
test_connect(void)
{
	struct i915_tc tc;
	enum i915_tc_mode mode;
	unsigned own_write;
	unsigned index;

	/* TC1: a DP-alt partner offering two lanes (FIA1 slot 0, mask 0x3), the PHY ready. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0x3u);

	mode = drv_i915_tc_lock(&tc, 0u, 2);
	check(mode == I915_TC_MODE_DP_ALT, "connect: DP-alt for two lanes");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) != 0u, "connect: ownership taken");
	check(fake.power[I915_TC_POWER_COLD][0] == 1, "connect: TC cold blocked");
	check(fake.locked[0] == 1, "connect: the lock is held for the caller");

	/* The ownership was written with the port's lanes powered (the only write before the cold block). */
	own_write = FAKE_WRITES;
	for (index = 0u; index < fake.writes; index++) {
		if (fake.write_reg[index] == REG_DDI_BUF_CTL(0u)) {
			own_write = index;
			break;
		}
	}
	check(own_write == 0u, "connect: the ownership is the first write");

	/* The unlock without a link gives the PHY back at once. */
	drv_i915_tc_unlock(&tc, 0u);
	check(tc.port[0].mode == I915_TC_MODE_NONE, "unlock: no link, the port is given back");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "unlock: ownership cleared");
	check(fake.power[I915_TC_POWER_COLD][0] == 0, "unlock: TC cold unblocked");
	check(power_balanced() && fake.lock_errors == 0, "unlock: power and locks balanced");

	/* A link keeps the port held across the unlock; its put gives it back. */
	drv_i915_tc_get_link(&tc, 0u, 2);
	check(tc.port[0].mode == I915_TC_MODE_DP_ALT && tc.port[0].links == 1u, "get_link: held with one link");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) != 0u, "get_link: ownership kept after the unlock");
	drv_i915_tc_put_link(&tc, 0u);
	check(tc.port[0].mode == I915_TC_MODE_NONE && fake.power[I915_TC_POWER_COLD][0] == 0, "put_link: given back");
	check(power_balanced() && fake.lock_errors == 0, "get/put_link: power and locks balanced");
}

/* Refusals unwind what the connect took and fall back to the receptacle's default (Thunderbolt, nothing held). */
static void
test_refusals(void)
{
	struct i915_tc tc;
	enum i915_tc_mode mode;

	/* The PHY not ready: ownership taken and given back, no cold block, Thunderbolt. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	mode = drv_i915_tc_lock(&tc, 0u, 1);
	check(mode == I915_TC_MODE_TBT, "refusal: not ready falls back to Thunderbolt");
	check(wrote(REG_DDI_BUF_CTL(0u), BUF_OWNED, BUF_OWNED), "refusal: ownership was asked for");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "refusal: ownership given back");
	check(fake.power[I915_TC_POWER_COLD][0] == 0 && tc.port[0].connect_failures == 1u, "refusal: no cold block, one failure");
	drv_i915_tc_unlock(&tc, 0u);
	check(tc.port[0].mode == I915_TC_MODE_NONE && power_balanced(), "refusal: unlock leaves nothing held");

	/* Too few lanes: the cold block and the ownership unwound. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0x3u);
	mode = drv_i915_tc_lock(&tc, 0u, 4);
	check(mode == I915_TC_MODE_TBT, "refusal: two lanes for four falls back");
	check(fake.power[I915_TC_POWER_COLD][0] == 0, "refusal: cold block unwound");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "refusal: ownership unwound");
	drv_i915_tc_unlock(&tc, 0u);

	/* The cold power refused: the ownership unwound. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0xfu);
	fake.fail_cold = 1;
	mode = drv_i915_tc_lock(&tc, 0u, 1);
	check(mode == I915_TC_MODE_TBT, "refusal: cold power refused falls back");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "refusal: ownership unwound after the cold power");
	drv_i915_tc_unlock(&tc, 0u);
	check(power_balanced() && fake.lock_errors == 0, "refusals: power and locks balanced");

	/* A TCSS in TC cold (all-ones) is not ready. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), 0xffffffffu);
	mode = drv_i915_tc_lock(&tc, 0u, 1);
	check(mode == I915_TC_MODE_TBT, "refusal: TCSS all-ones is not ready");
	drv_i915_tc_unlock(&tc, 0u);
}

/* The FIA's lanes of each mask, and the slots of the four ports. */
static void
test_lanes_and_slots(void)
{
	struct i915_tc tc;
	static const uint32_t masks[] = { 0x1u, 0x2u, 0x4u, 0x8u, 0x3u, 0xcu, 0xfu, 0x5u };
	static const int lanes[] = { 1, 1, 1, 1, 2, 2, 4, 1 };
	unsigned index;
	int counted;
	char what[64];

	/* Each mask on TC2 (FIA1 slot 1, bits 8 to 11), the port held in DP-alt. */
	for (index = 0u; index < sizeof(masks) / sizeof(masks[0]); index++) {
		fake_reset();
		bind(&tc);
		drv_i915_tc_declare(&tc, 1u, 0);
		fake_set(REG_FIA1_DPSP, masks[index] << 8);
		tc.port[1].mode = I915_TC_MODE_DP_ALT;
		counted = drv_i915_tc_max_lanes(&tc, 1u);
		snprintf(what, sizeof(what), "lanes: mask 0x%x is %d", masks[index], lanes[index]);
		check(counted == lanes[index], what);
	}

	/* Only DP-alt restricts the lanes. */
	tc.port[1].mode = I915_TC_MODE_LEGACY;
	counted = drv_i915_tc_max_lanes(&tc, 1u);
	check(counted == 4, "lanes: legacy has four");

	/* The modular FIAs: TC1 and TC2 on FIA1, TC3 and TC4 on FIA2, slots 0 and 1. */
	check(tc.port[0].fia == 0u && tc.port[0].fia_slot == 0u, "slots: TC1 FIA1.0");
	check(tc.port[1].fia == 0u && tc.port[1].fia_slot == 1u, "slots: TC2 FIA1.1");
	check(tc.port[2].fia == 1u && tc.port[2].fia_slot == 0u, "slots: TC3 FIA2.0");
	check(tc.port[3].fia == 1u && tc.port[3].fia_slot == 1u, "slots: TC4 FIA2.1");

	/* TC3's lanes are read from FIA2. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 2u, 0);
	fake_set(REG_FIA2_DPSP, 0xfu);
	tc.port[2].mode = I915_TC_MODE_DP_ALT;
	counted = drv_i915_tc_max_lanes(&tc, 2u);
	check(counted == 4, "lanes: TC3 on FIA2");

	/* The pin assignment: TC2's field (bits 4 to 7) of FIA1's DFLEXPA1. */
	fake_set(REG_FIA1_PA1, 0x4u << 4);
	drv_i915_tc_declare(&tc, 1u, 0);
	check(drv_i915_tc_pin_assignment(&tc, 1u) == 4u, "pins: TC2 pin D");
}

/* The legacy flag corrected by the live status, and what counts as connected. */
static void
test_legacy_and_connected(void)
{
	struct i915_tc tc;
	enum i915_tc_mode mode;
	int connected;

	/* A port the VBT calls legacy with a DP-alt partner: the flag is corrected at the connect. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 1);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0xfu);
	(void)drv_i915_tc_lock(&tc, 0u, 1);
	check(tc.port[0].legacy == 0 && tc.port[0].mode == I915_TC_MODE_DP_ALT, "legacy: corrected to a receptacle");
	drv_i915_tc_unlock(&tc, 0u);

	/* Held in no mode, a Thunderbolt partner counts as connected. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 0);
	connected = drv_i915_tc_connected(&tc, 0u);
	check(connected == 1, "connected: anything plugged into a port held in no mode");

	/* Held in DP-alt, only a DP-alt partner counts. */
	tc.port[0].mode = I915_TC_MODE_DP_ALT;
	connected = drv_i915_tc_connected(&tc, 0u);
	check(connected == 0, "connected: a DP-alt port does not count Thunderbolt");
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	connected = drv_i915_tc_connected(&tc, 0u);
	check(connected == 1, "connected: a DP-alt port counts its partner");
	check(fake.lock_errors == 0 && power_balanced(), "connected: power and locks balanced");

	/*
	 * ws051-p004a: a probe holds a link for its AUX messages; each message
	 * locks the port and asks the locked answer, which takes no lock; the
	 * probe's last link gives the PHY back at once.
	 */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0xfu);
	drv_i915_tc_get_link(&tc, 0u, 1);
	mode = drv_i915_tc_lock(&tc, 0u, 1);
	connected = drv_i915_tc_connected_locked(&tc, 0u);
	check(mode == I915_TC_MODE_DP_ALT && connected == 1, "connected_locked: the probe's port is DP-alt with its partner");
	check(fake.lock_errors == 0, "connected_locked: no lock taken under the caller's");
	fake_set(REG_DE_HPD_ISR, 0u);
	connected = drv_i915_tc_connected_locked(&tc, 0u);
	check(connected == 0, "connected_locked: an unplugged partner");
	drv_i915_tc_unlock(&tc, 0u);
	check(tc.port[0].mode == I915_TC_MODE_DP_ALT && (fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) != 0u, "probe link: the PHY stays between the messages");
	drv_i915_tc_put_link(&tc, 0u);
	check(tc.port[0].mode != I915_TC_MODE_DP_ALT && (fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "probe link: the last link gives the PHY back at once (M2)");
	check(fake.lock_errors == 0 && power_balanced(), "probe link: power and locks balanced");

	/* ws051-p005a: a link held in DP-alt needs a reset only while its partner is gone. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_FIA1_DPSP, 0xfu);
	check(drv_i915_tc_link_needs_reset(&tc, 0u) == 0, "link reset: no link, no reset");
	drv_i915_tc_get_link(&tc, 0u, 4);
	check(drv_i915_tc_link_needs_reset(&tc, 0u) == 0, "link reset: the partner is there");
	fake_set(REG_DE_HPD_ISR, 0u);
	check(drv_i915_tc_link_needs_reset(&tc, 0u) == 1, "link reset: the partner went");
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	check(drv_i915_tc_link_needs_reset(&tc, 0u) == 0, "link reset: the partner came back");
	drv_i915_tc_put_link(&tc, 0u);
	check(fake.lock_errors == 0 && power_balanced(), "link reset: power and locks balanced");
}

/* What a port has of DisplayPort for the Type-C layer (ws050-p005). */
static void
test_dp_sample(void)
{
	struct i915_tc tc;
	struct i915_tc_dp_sample sample;

	/* TC2 held in DP-alt with its partner live: pin D on two lanes (lanes 2-3). */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 1u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 17);
	fake_set(REG_FIA1_DPSP, 0xcu << 8);
	fake_set(REG_FIA1_PA1, 0x4u << 4);
	tc.port[1].mode = I915_TC_MODE_DP_ALT;
	drv_i915_tc_dp_sample(&tc, 1u, &sample);
	check(sample.hpd == 1 && sample.pin == 4u && sample.lanes == 2, "dp-sample: DP-alt pin D, two lanes");

	/* Not held in DP-alt: the FIA is not read, only the live status counts. */
	tc.port[1].mode = I915_TC_MODE_NONE;
	drv_i915_tc_dp_sample(&tc, 1u, &sample);
	check(sample.hpd == 1 && sample.pin == 0u && sample.lanes == 0, "dp-sample: no FIA read outside DP-alt");

	/* An undeclared port has nothing; the lock and the power stay balanced. */
	drv_i915_tc_dp_sample(&tc, 0u, &sample);
	check(sample.hpd == 0 && sample.pin == 0u && sample.lanes == 0, "dp-sample: undeclared port");
	check(fake.lock_errors == 0 && power_balanced(), "dp-sample: power and locks balanced");
}

/*
 * The FIA's DisplayPort main link lanes (ws051-p003): the 5330's TC2 with
 * four lanes reads 0xf0 in FIA1's DFLEXDPMLE1 (as Linux left it,
 * plan/ws051/tests/m3-5330-20261007), one and two lanes from the near end
 * and, reversed, from the far end, and the other slot is kept.
 */
static void
test_fia_lane_count(void)
{
	struct i915_tc tc;

	/* TC2 with four lanes, TC1's slot holding two lanes. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 1u, 0);
	fake_set(REG_FIA1_MLE1, 0x3u);
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 4, 0);
	check(fake_get(REG_FIA1_MLE1) == 0xf3u, "fia lanes: TC2 four lanes is 0xf in slot 1 (5330: 0xf0), TC1's slot kept");

	/* One and two lanes from the near end. */
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 1, 0);
	check(fake_get(REG_FIA1_MLE1) == 0x13u, "fia lanes: TC2 one lane is lane 0");
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 2, 0);
	check(fake_get(REG_FIA1_MLE1) == 0x33u, "fia lanes: TC2 two lanes are lanes 0 and 1");

	/* Reversed, from the far end. */
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 1, 1);
	check(fake_get(REG_FIA1_MLE1) == 0x83u, "fia lanes: TC2 one reversed lane is lane 3");
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 2, 1);
	check(fake_get(REG_FIA1_MLE1) == 0xc3u, "fia lanes: TC2 two reversed lanes are lanes 3 and 2");

	/* A lane count the FIA has not is logged and leaves no lanes. */
	drv_i915_tc_set_fia_lane_count(&tc, 1u, 3, 0);
	check(fake_get(REG_FIA1_MLE1) == 0x03u, "fia lanes: three lanes leaves none");

	/* TC3 is slot 0 of FIA2. */
	drv_i915_tc_declare(&tc, 2u, 0);
	drv_i915_tc_set_fia_lane_count(&tc, 2u, 4, 0);
	check(fake_get(REG_FIA2_MLE1) == 0x0fu, "fia lanes: TC3 four lanes is FIA2 slot 0");

	/* The 5330's TC2 pin assignment: FIA1's DFLEXPA1 0x30 is pin C. */
	fake_set(REG_FIA1_PA1, 0x30u);
	check(drv_i915_tc_pin_assignment(&tc, 1u) == 3u, "pins: the 5330's TC2 DFLEXPA1 0x30 is pin C");
}

/*
 * The legacy port's wait for its PHY (ws177-p002): it sleeps between the
 * reads and measures the half second on the clock, so a sleep that lasts a
 * whole tick ends it after the same time with fewer reads; a PHY that comes
 * ends it at once; a failed clock reads the bit once; a USB-C port does not
 * wait.
 */
static void
test_wait_ready(void)
{
	struct i915_tc tc;

	/* TC4 legacy (SDE bit 27), the PHY never ready, a sleep of a 10 ms tick: 50 sleeps, half a second. */
	fake_reset();
	fake.tick_us = 10000u;
	bind(&tc);
	drv_i915_tc_declare(&tc, 3u, 1);
	fake_set(REG_SDEISR, 1u << 27);
	drv_i915_tc_readout(&tc);
	check(fake.sleeps == 50u, "wait: a 10 ms tick sleeps 50 times");
	check(fake.now_us >= 500000u && fake.now_us < 510000u, "wait: half a second on the clock");
	check(fake.timeouts == 1u, "wait: the timeout is logged once");
	check(tc.port[3].mode == I915_TC_MODE_NONE, "wait: a PHY that never came is not held");
	check(power_balanced() && fake.lock_errors == 0, "wait: power and locks balanced");

	/* The same with sleeps of exactly the time asked (1 ms): 500 sleeps, half a second. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 3u, 1);
	fake_set(REG_SDEISR, 1u << 27);
	drv_i915_tc_readout(&tc);
	check(fake.sleeps == 500u, "wait: a 1 ms sleep sleeps 500 times");
	check(fake.now_us == 500000u, "wait: exactly half a second");

	/* The PHY comes after the third sleep: the wait ends there, no timeout. */
	fake_reset();
	fake.tick_us = 10000u;
	fake.ready_after = 3u;
	fake.ready_port = 3u;
	bind(&tc);
	drv_i915_tc_declare(&tc, 3u, 1);
	fake_set(REG_SDEISR, 1u << 27);
	drv_i915_tc_readout(&tc);
	check(fake.sleeps == 3u, "wait: ends at the read after the PHY came");
	check(fake.timeouts == 0u, "wait: no timeout when the PHY came");

	/* A failed clock: the bit is read once, no sleep, no endless wait. */
	fake_reset();
	fake.clock_fails = 1;
	bind(&tc);
	drv_i915_tc_declare(&tc, 3u, 1);
	fake_set(REG_SDEISR, 1u << 27);
	drv_i915_tc_readout(&tc);
	check(fake.sleeps == 0u, "wait: a failed clock does not sleep");
	check(power_balanced() && fake.lock_errors == 0, "wait: a failed clock leaves power and locks balanced");

	/* A USB-C port (not legacy) does not wait. */
	fake_reset();
	fake.tick_us = 10000u;
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	drv_i915_tc_readout(&tc);
	check(fake.sleeps == 0u, "wait: a USB-C port does not wait");
}

/*
 * The driver's stop (ws177-p002): a port the firmware's output holds with a
 * link gives its PHY and its TC cold block back, every port is retired, the
 * display answers for none afterwards, and a second stop does nothing.
 */
static void
test_stop(void)
{
	struct i915_tc tc;
	enum i915_tc_mode mode;
	unsigned writes;
	unsigned gets;

	/* TC1 driven by the firmware in DP-alt (one link, TC cold blocked), TC3 declared and empty. */
	fake_reset();
	bind(&tc);
	drv_i915_tc_declare(&tc, 0u, 0);
	drv_i915_tc_declare(&tc, 2u, 0);
	fake_set(REG_DE_HPD_ISR, 1u << 16);
	fake_set(REG_TCSS(0u), TCSS_READY);
	fake_set(REG_DDI_BUF_CTL(0u), BUF_ENABLE | BUF_OWNED);
	drv_i915_tc_readout(&tc);
	check(tc.port[0].links == 1u && tc.port[0].cold_held, "stop: TC1 held with a link before the stop");

	/* The stop gives TC1 back although its link was not put back. */
	drv_i915_tc_stop(&tc);
	check(tc.port[0].mode == I915_TC_MODE_NONE && tc.port[0].links == 0u, "stop: TC1 holds nothing");
	check((fake_get(REG_DDI_BUF_CTL(0u)) & BUF_OWNED) == 0u, "stop: TC1's ownership given back");
	check(fake.power[I915_TC_POWER_COLD][0] == 0 && !tc.port[0].cold_held, "stop: TC1's TC cold unblocked");
	check(!tc.port[0].present && !tc.port[2].present, "stop: every port retired");
	check(!tc.live, "stop: the display answers for no port");
	check(power_balanced() && fake.lock_errors == 0, "stop: power and locks balanced");

	/* Nothing after the stop takes a port again, writes or takes power. */
	writes = fake.writes;
	gets = fake.power_gets;
	drv_i915_tc_get_link(&tc, 0u, 4);
	mode = drv_i915_tc_lock(&tc, 0u, 4);
	drv_i915_tc_unlock(&tc, 0u);
	drv_i915_tc_put_link(&tc, 0u);
	drv_i915_tc_stop(&tc);
	check(mode == I915_TC_MODE_NONE, "stop: a lock after the stop holds no mode");
	check(fake.writes == writes && fake.power_gets == gets, "stop: nothing written or powered after the stop");
	check(fake.lock_errors == 0, "stop: no lock taken after the stop");
}
