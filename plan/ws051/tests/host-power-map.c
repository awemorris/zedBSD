/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-256: the host test of the display's XE_LPD power map (display/power.c,
 * Linux's xelpd_power_wells): a Type-C port's AUX_USBC domain -- its TC cold
 * block and its AUX transfers -- powers PW_2 and DC_off with its AUX well,
 * as XELPD_DC_OFF_PORT_POWER_DOMAINS has it, so the PHY ownership in
 * DDI_BUF_CTL and the AUX channel's registers stay powered after the
 * connect gives the port's DDI lanes back.  Also the other DC-off port
 * domains (AUX C to E and their I/O, TBT, VGA, audio playback) and that the
 * AUX_A domain of the eDP does not need PW_2.
 *
 * power.c is linked alone; the functions it calls that building the map
 * never reaches stop the test.
 *
 *     host-power-map
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/drivers/gpu/i915/display/modeset-internal.h"
#include "src/drivers/gpu/i915/display/power.h"
#include "src/drivers/gpu/i915/display/clock.h"
#include "src/drivers/gpu/i915/display/phy.h"
#include "src/drivers/gpu/i915/display/dc9.h"
#include "src/drivers/gpu/i915/display/dkl-phy.h"
#include "src/drivers/gpu/i915/mmio.h"
#include "src/drivers/gpu/i915/trace.h"

#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/waitq.h>


/* How many checks failed. */
static int failures;

int main(void);
static void check(int condition, const char *text);
static int well_index(const struct i915_power_domains *pd, const char *name);
static int domain_has(const struct i915_power_domains *pd, enum i915_power_domain domain, const char *name);
static void unreached(const char *name);

/* Runs the checks. */
int
main(void)
{
	static struct i915_power_domains pd;
	static struct i915_trace trace;
	enum i915_power_domain domain;
	int error;
	int index;

	/* The XE_LPD map. */
	memset(&pd, 0, sizeof(pd));
	error = drv_i915_power_domains_init(&pd, 13U, -1, -1, &trace);
	check(error == 0, "the XE_LPD map is built");

	/* Each Type-C AUX_USBC domain: its own AUX well, PW_2 and DC_off. */
	for (index = 0; index < 4; index++) {
		domain = (enum i915_power_domain)(I915_PW_DOMAIN_AUX_USBC1 + index);
		check(domain_has(&pd, domain, "PW_2"), "AUX_USBC powers PW_2");
		check(domain_has(&pd, domain, "DC_off"), "AUX_USBC keeps DC_off");
		check(!domain_has(&pd, domain, "PW_A"), "AUX_USBC does not power PW_A");
	}
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_USBC2, "AUX_USBC2"), "AUX_USBC2 powers its own well");
	check(!domain_has(&pd, I915_PW_DOMAIN_AUX_USBC2, "AUX_USBC1"), "AUX_USBC2 does not power AUX_USBC1");

	/* The Thunderbolt AUX domains and the AUX channels C to E with their I/O. */
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_TBT2, "PW_2"), "AUX_TBT2 powers PW_2");
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_TBT2, "AUX_TBT2"), "AUX_TBT2 powers its own well");
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_C, "PW_2") && domain_has(&pd, I915_PW_DOMAIN_AUX_C, "DC_off"), "AUX_C powers PW_2 and DC_off");
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_E, "PW_2"), "AUX_E powers PW_2");
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_IO_D, "PW_2") && domain_has(&pd, I915_PW_DOMAIN_AUX_IO_D, "AUX_D"), "AUX_IO_D powers PW_2 and AUX_D");
	check(domain_has(&pd, I915_PW_DOMAIN_VGA, "PW_2"), "VGA powers PW_2");
	check(domain_has(&pd, I915_PW_DOMAIN_AUDIO_PLAYBACK, "PW_2"), "audio playback powers PW_2");

	/* The Type-C DDI lanes as before; the eDP's AUX_A keeps DC off without PW_2. */
	check(domain_has(&pd, I915_PW_DOMAIN_PORT_DDI_LANES_TC2, "PW_2") && domain_has(&pd, I915_PW_DOMAIN_PORT_DDI_LANES_TC2, "DC_off"), "DDI lanes TC2 power PW_2 and DC_off");
	check(domain_has(&pd, I915_PW_DOMAIN_AUX_A, "DC_off") && !domain_has(&pd, I915_PW_DOMAIN_AUX_A, "PW_2"), "AUX_A keeps DC_off without PW_2");

	/* The outcome. */
	drv_i915_power_domains_cleanup(&pd);
	if (failures != 0) {
		printf("host-power-map: %d FAILED\n", failures);
		return 1;
	}
	printf("host-power-map: PASS\n");
	return 0;
}

/* Prints a check's outcome. */
static void
check(
	int condition,
	const char *text)
{
	/* Passed. */
	if (condition) {
		printf("ok: %s\n", text);
		return;
	}

	/* Failed. */
	printf("FAIL: %s\n", text);
	failures++;
}

/* Finds a well of the map by its name; -1 when there is none. */
static int
well_index(
	const struct i915_power_domains *pd,
	const char *name)
{
	unsigned index;

	/* Each well. */
	for (index = 0U; index < pd->num_power_wells; index++) {
		if (strcmp(pd->power_wells[index].name, name) == 0)
			return (int)index;
	}

	/* None of that name. */
	return -1;
}

/* Tells whether a domain is provided by a well of a name. */
static int
domain_has(
	const struct i915_power_domains *pd,
	enum i915_power_domain domain,
	const char *name)
{
	uint64_t wells;
	int index;

	/* The well, and the domain's wells. */
	index = well_index(pd, name);
	if (index < 0)
		return 0;
	wells = drv_i915_power_domain_wells(pd, domain);

	/* Whether the well is among them. */
	if ((wells & ((uint64_t)1U << index)) == 0U)
		return 0;
	return 1;
}

/* Stops the test in a function building the map never calls. */
static void
unreached(
	const char *name)
{
	/* Says which, and stops. */
	fprintf(stderr, "host-power-map: %s reached\n", name);
	abort();
}

/* The functions power.c links to; building the map calls none but the logger, the trace and the locks. */
void drv_i915_bxt_get_cdclk(struct i915_cdclk_dev *cd, struct i915_cdclk_config *cfg) { (void)cd; (void)cfg; unreached("bxt_get_cdclk"); }
void drv_i915_cdclk_init_hw(struct i915_cdclk_dev *cd) { (void)cd; unreached("cdclk_init_hw"); }
void drv_i915_cdclk_uninit_hw(struct i915_cdclk_dev *cd) { (void)cd; unreached("cdclk_uninit_hw"); }
int drv_i915_combo_phy_init(struct i915_mmio *mmio, struct i915_trace *trace, unsigned *initialised_out) { (void)mmio; (void)trace; (void)initialised_out; unreached("combo_phy_init"); return 0; }
void drv_i915_combo_phy_uninit(struct i915_mmio *mmio) { (void)mmio; unreached("combo_phy_uninit"); }
int drv_i915_dc9_enter_check(uint32_t dc_state, int pw2_enabled, int irqs_enabled) { (void)dc_state; (void)pw2_enabled; (void)irqs_enabled; unreached("dc9_enter_check"); return 0; }
int drv_i915_dc9_leave_check(uint32_t dc_state, int pw2_enabled) { (void)dc_state; (void)pw2_enabled; unreached("dc9_leave_check"); return 0; }
uint32_t drv_i915_dc9_south_chicken(uint32_t value, int entering) { (void)value; (void)entering; unreached("dc9_south_chicken"); return 0; }
int drv_i915_dkl_phy_wait_set(struct i915_dkl_phy *dkl, unsigned tc_port, uint32_t phy_address, uint32_t mask, unsigned timeout_us) { (void)dkl; (void)tc_port; (void)phy_address; (void)mask; (void)timeout_us; unreached("dkl_phy_wait_set"); return 0; }
void drv_i915_lcd_backend_fault(const char *what) { (void)what; unreached("lcd_backend_fault"); }
void drv_i915_lcd_error(const char *what) { (void)what; unreached("lcd_error"); }
uint32_t drv_i915_raw_read32(struct i915_mmio *mmio, uint32_t offset) { (void)mmio; (void)offset; unreached("raw_read32"); return 0; }
void drv_i915_raw_write32(struct i915_mmio *mmio, uint32_t offset, uint32_t value) { (void)mmio; (void)offset; (void)value; unreached("raw_write32"); }
int drv_i915_time_base_faulted(void) { return 0; }
void drv_i915_trace_init(struct i915_trace *trace) { (void)trace; }
void drv_i915_trace_record(struct i915_trace *trace, uint16_t stage, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1) { (void)trace; (void)stage; (void)op; (void)what; (void)argument0; (void)argument1; }
int drv_i915_udelay(unsigned microseconds) { (void)microseconds; unreached("udelay"); return 0; }
void drv_i915_vga_reset_io_mem(struct i915_vga_client *c) { (void)c; unreached("vga_reset_io_mem"); }
int drv_i915_wait_reg(struct i915_mmio *mmio, uint32_t reg, uint32_t mask, uint32_t value, unsigned fast_us, unsigned slow_ms, uint32_t *last) { (void)mmio; (void)reg; (void)mask; (void)value; (void)fast_us; (void)slow_ms; (void)last; unreached("wait_reg"); return 0; }
void kern_logf(const char *format, ...) { va_list arguments; va_start(arguments, format); vprintf(format, arguments); va_end(arguments); }
int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name) { (void)mutex; (void)rank; (void)name; return 0; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }
void waitq_init(struct wait_queue *queue, const char *name) { (void)queue; (void)name; }
