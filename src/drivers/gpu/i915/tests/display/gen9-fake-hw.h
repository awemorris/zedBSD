/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The model of the display 9 hardware the Gen9 host tests drive
 * (gen9-fake-hw.c): a register file that answers the status
 * registers as the hardware does, the AUX channel with a DP sink
 * (DPCD, deferrals, I2C-over-AUX EDID, link training), the GMBUS with an
 * HDMI sink, the power wells, the DPLLs, the transcoders, the DDI
 * buffers, the frame counters and the planes' live surfaces.
 *
 * The model checks what the code writes and in which order, not the
 * hardware's timing.  Host only: built by run-*.sh
 * scripts.
 */

#ifndef DRIVERS_GPU_I915_TESTS_DISPLAY_GEN9_FAKE_HW_H
#define DRIVERS_GPU_I915_TESTS_DISPLAY_GEN9_FAKE_HW_H

#include "host-test.h"

#include "../../display/xd-io.h"
#include "gen9-fake-regs.h"

#include <stdint.h>

/* How many registers and writes the model keeps. */
#define I915_GEN9_FAKE_REGISTERS	1024U
#define I915_GEN9_FAKE_LOG		8192U

/*
 * One sink on a port: a DP sink on the AUX channel, or an HDMI sink on
 * the GMBUS.  It stays attached until the model is reset or the test
 * clears it.
 */
struct i915_gen9_fake_sink {
	int dp;
	int hdmi;
	uint8_t dpcd[0x700];
	uint8_t edid[256];

	/* The deferrals left, the I2C offset, and the swing clock recovery needs. */
	unsigned defer_left;
	unsigned i2c_offset;
	unsigned cr_swing;
};

/*
 * The model of the display hardware.
 *
 * One instance (i915_gen9_fake) for the whole test; i915_gen9_fake_reset()
 * empties it between the checks.
 */
struct i915_gen9_fake_hw {
	uint32_t keys[I915_GEN9_FAKE_REGISTERS];
	uint32_t values[I915_GEN9_FAKE_REGISTERS];
	unsigned used;

	/* Every write, in order. */
	uint32_t log_offset[I915_GEN9_FAKE_LOG];
	uint32_t log_value[I915_GEN9_FAKE_LOG];
	unsigned log_count;

	/* The writes the log had no room for. */
	unsigned log_dropped;

	/* The sinks of ports A to D, and the AUX transfers started. */
	struct i915_gen9_fake_sink sinks[I915_SKL_PORTS];
	unsigned aux_transfers;

	/* The GMBUS: the pin, the device's port, the bytes left and the position, a NAK. */
	unsigned gmbus_pin;
	unsigned gmbus_left;
	unsigned gmbus_position;
	int gmbus_port;
	int gmbus_nak;

	/* Whether the frame counters run, and the time waited. */
	int frames_run;
	uint64_t waited_us;
};

/* The model the tests drive. */
extern struct i915_gen9_fake_hw i915_gen9_fake;

/*
 * The same model behind the xd register window's hooks and behind the
 * MMIO BAR's stand-ins, so code that reaches the registers either way
 * writes into the one log.
 */
extern const struct i915_xd_io_hooks i915_gen9_fake_xd_hooks;
extern const struct i915_host_mmio_hooks i915_gen9_fake_mmio;

/* Reports the xd register window that reaches the model. */
struct i915_xd_io *i915_gen9_fake_xd(void);

/* Reports a register's stored value, 0 when never written. */
uint32_t i915_gen9_fake_get(uint32_t offset);

/* Stores a register's value without the hardware's effects or the log. */
void i915_gen9_fake_set(uint32_t offset, uint32_t value);

/* Empties the model: no registers, no sinks; power gate 2's fuses distributed. */
void i915_gen9_fake_reset(void);

/* Attaches a DP 1.2 sink of HBR2 x4 with TPS3 and enhanced framing, and a 1080p EDID. */
void i915_gen9_fake_add_dp_sink(unsigned port, unsigned cr_swing);

/* Attaches an HDMI sink with the same EDID on the port's GMBUS pin. */
void i915_gen9_fake_add_hdmi_sink(unsigned port);

/* Reports the first write to a register whose masked value matches, as its position in the log, or -1. */
int i915_gen9_fake_written_at(uint32_t offset, uint32_t mask, uint32_t value);

/*
 * Starts the golden record: every reset from now on, and the close, write
 * the writes of the check that ends to the file, as one numbered scene.
 * Reports 0, or the error of the file's creation.
 */
int i915_gen9_fake_golden_open(const char *path);

/* Writes the last scene and ends the golden record. */
void i915_gen9_fake_golden_close(void);

#endif /* DRIVERS_GPU_I915_TESTS_DISPLAY_GEN9_FAKE_HW_H */
