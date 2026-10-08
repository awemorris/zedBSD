/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-266: the host test of the run log's forwarding of the Type-C ports'
 * hooks.  The resident run lights its output through the run log
 * (drv_i915_lcd_trace_init); the run log must hand the Type-C links, the
 * port's mode, the FIA lanes, the pin assignment and the DKL PHY access
 * to the backend, or a DisplayPort output on a Type-C port in DP-alt mode
 * is lit as if the port were in no mode.
 */

#include "src/drivers/gpu/i915/tests/display/host-test.h"
#include "src/drivers/gpu/i915/display/internal.h"
#include "src/drivers/gpu/i915/display/diagnostics.h"

#include <string.h>

/* What the stand-in backend was asked: each hook's last port and arguments. */
static int fake_put_port;
static int fake_get_port;
static int fake_get_lanes;
static int fake_mode_port;
static int fake_fia_port;
static int fake_fia_lanes;
static int fake_fia_reversal;
static int fake_pin_port;
static int fake_dkl_port;
static uint32_t fake_dkl_address;
static uint32_t fake_dkl_value;
static uint32_t fake_dkl_clear;
static uint32_t fake_dkl_set;

static void fake_tc_put_link(void *ctx, int tc_port);
static void fake_tc_get_link(void *ctx, int tc_port, int lanes);
static int fake_tc_mode(void *ctx, int tc_port);
static void fake_tc_set_fia_lane_count(void *ctx, int tc_port, int lanes, int lane_reversal);
static unsigned fake_tc_pin_assignment(void *ctx, int tc_port);
static uint32_t fake_dkl_read(void *ctx, int tc_port, uint32_t phy_address);
static void fake_dkl_write(void *ctx, int tc_port, uint32_t phy_address, uint32_t value);
static void fake_dkl_rmw(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set);

/*
 * Runs the checks.
 */
int
main(
	void)
{
	static struct i915_lcd_trace trace;
	struct i915_lcd_emit backend;
	int status;

	/* A backend without the Type-C hooks (a model): the run log has none either. */
	memset(&backend, 0, sizeof(backend));
	drv_i915_lcd_trace_init(&trace, &backend);
	i915_host_check(trace.ops.tc_get_link == NULL, "a model's run log has no Type-C link hook");
	i915_host_check(trace.ops.tc_mode == NULL, "a model's run log has no Type-C mode hook");
	i915_host_check(trace.ops.dkl_read == NULL, "a model's run log has no DKL hook");

	/* The kernel's backend: every Type-C and DKL hook. */
	memset(&backend, 0, sizeof(backend));
	backend.ctx = &backend;
	backend.tc_put_link = fake_tc_put_link;
	backend.tc_get_link = fake_tc_get_link;
	backend.tc_mode = fake_tc_mode;
	backend.tc_set_fia_lane_count = fake_tc_set_fia_lane_count;
	backend.tc_pin_assignment = fake_tc_pin_assignment;
	backend.dkl_read = fake_dkl_read;
	backend.dkl_write = fake_dkl_write;
	backend.dkl_rmw = fake_dkl_rmw;
	drv_i915_lcd_trace_init(&trace, &backend);

	/* Each hook reaches the backend with its arguments. */
	i915_host_check(trace.ops.tc_get_link != NULL, "the run log forwards the link");
	i915_host_check(trace.ops.tc_mode != NULL, "the run log forwards the mode");
	i915_host_check(trace.ops.dkl_read != NULL, "the run log forwards the DKL PHY");
	trace.ops.tc_get_link(trace.ops.ctx, 1, 4);
	i915_host_check(fake_get_port == 1 && fake_get_lanes == 4, "tc_get_link reaches the backend");
	i915_host_check(trace.ops.tc_mode(trace.ops.ctx, 1) == 2 && fake_mode_port == 1, "tc_mode answers the backend's mode");
	trace.ops.tc_set_fia_lane_count(trace.ops.ctx, 1, 4, 0);
	i915_host_check(fake_fia_port == 1 && fake_fia_lanes == 4 && fake_fia_reversal == 0, "tc_set_fia_lane_count reaches the backend");
	i915_host_check(trace.ops.tc_pin_assignment(trace.ops.ctx, 1) == 3U && fake_pin_port == 1, "tc_pin_assignment answers pin C");
	trace.ops.tc_put_link(trace.ops.ctx, 1);
	i915_host_check(fake_put_port == 1, "tc_put_link reaches the backend");
	i915_host_check(trace.ops.dkl_read(trace.ops.ctx, 1, 0x2000U) == 0x1234U && fake_dkl_address == 0x2000U, "dkl_read reaches the backend");
	trace.ops.dkl_write(trace.ops.ctx, 1, 0x2004U, 7U);
	i915_host_check(fake_dkl_address == 0x2004U && fake_dkl_value == 7U, "dkl_write reaches the backend");
	trace.ops.dkl_rmw(trace.ops.ctx, 1, 0x2008U, 1U, 2U);
	i915_host_check(fake_dkl_port == 1 && fake_dkl_address == 0x2008U && fake_dkl_clear == 1U && fake_dkl_set == 2U, "dkl_rmw reaches the backend");

	/* The tally. */
	status = i915_host_report("bug266-trace-tc");
	return status;
}

/* Records a link given back. */
static void
fake_tc_put_link(void *ctx, int tc_port)
{
	(void)ctx;
	fake_put_port = tc_port;
}

/* Records a link taken. */
static void
fake_tc_get_link(void *ctx, int tc_port, int lanes)
{
	(void)ctx;
	fake_get_port = tc_port;
	fake_get_lanes = lanes;
}

/* Answers DP-alt (2) for any port. */
static int
fake_tc_mode(void *ctx, int tc_port)
{
	(void)ctx;
	fake_mode_port = tc_port;
	return 2;
}

/* Records the FIA's lanes. */
static void
fake_tc_set_fia_lane_count(void *ctx, int tc_port, int lanes, int lane_reversal)
{
	(void)ctx;
	fake_fia_port = tc_port;
	fake_fia_lanes = lanes;
	fake_fia_reversal = lane_reversal;
}

/* Answers pin assignment C (3). */
static unsigned
fake_tc_pin_assignment(void *ctx, int tc_port)
{
	(void)ctx;
	fake_pin_port = tc_port;
	return 3U;
}

/* Answers 0x1234 for any DKL register. */
static uint32_t
fake_dkl_read(void *ctx, int tc_port, uint32_t phy_address)
{
	(void)ctx;
	fake_dkl_port = tc_port;
	fake_dkl_address = phy_address;
	return 0x1234U;
}

/* Records a DKL register written. */
static void
fake_dkl_write(void *ctx, int tc_port, uint32_t phy_address, uint32_t value)
{
	(void)ctx;
	fake_dkl_port = tc_port;
	fake_dkl_address = phy_address;
	fake_dkl_value = value;
}

/* Records a DKL register changed. */
static void
fake_dkl_rmw(void *ctx, int tc_port, uint32_t phy_address, uint32_t clear, uint32_t set)
{
	(void)ctx;
	fake_dkl_port = tc_port;
	fake_dkl_address = phy_address;
	fake_dkl_clear = clear;
	fake_dkl_set = set;
}
