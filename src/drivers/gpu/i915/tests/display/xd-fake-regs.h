/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register model the host tests of the xd display run against
 * (xd-fake-regs.c).
 *
 * The model is a set of registers with values, a clock in microseconds,
 * and rules that say how the hardware answers a write: a request bit
 * that shows up as a state bit some time later, or a test's own function
 * for an exchange (the GT driver mailbox), or an AUX sink behind a
 * channel's control word (struct i915_xd_fake_aux), or a DDC target
 * behind the GMBUS controller (struct i915_xd_fake_gmbus).  It is
 * installed into a struct i915_xd_io through its hooks, so the xd code
 * runs unchanged.  It also records the order in which the xd locks are
 * taken and counts the takings that break the documented order.
 *
 * The model is one per test process and is cleared with
 * i915_xd_fake_reset().
 */

#ifndef DRIVERS_GPU_I915_TESTS_DISPLAY_XD_FAKE_REGS_H
#define DRIVERS_GPU_I915_TESTS_DISPLAY_XD_FAKE_REGS_H

#include <stdint.h>

#include "../../display/xd-io.h"

/* A test's answer to a write of one register: the register, the value written, and the test's context. */
typedef void (*i915_xd_fake_write_fn)(uint32_t offset, uint32_t value, void *context);

/* A test's answer to a read of one register: the register, the value it holds, and the test's context. */
typedef uint32_t (*i915_xd_fake_read_fn)(uint32_t offset, uint32_t value, void *context);

/*
 * What the modelled AUX sink does with one transaction
 * (i915_xd_fake_aux_script()).
 *
 *   ACK       answers: a read with its bytes, a write by taking them
 *   NACK      refuses (reply 0x10 native, 0x40 I2C)
 *   DEFER     asks to be asked later (reply 0x20 native, 0x80 I2C)
 *   TIMEOUT   does not answer: done with the timeout error
 *   DAMAGED   answers damaged: done with the receive error
 *   STUCK     the channel stays busy for param microseconds, then ends
 *             with the timeout error
 *   SIZE      done with a reply size of param and no error
 */
#define I915_XD_FAKE_AUX_ACK		0U
#define I915_XD_FAKE_AUX_NACK		1U
#define I915_XD_FAKE_AUX_DEFER		2U
#define I915_XD_FAKE_AUX_TIMEOUT	3U
#define I915_XD_FAKE_AUX_DAMAGED	4U
#define I915_XD_FAKE_AUX_STUCK		5U
#define I915_XD_FAKE_AUX_SIZE		6U

/* The most transactions one script holds, and the room of the sink's two memories. */
#define I915_XD_FAKE_AUX_SCRIPT		32U
#define I915_XD_FAKE_AUX_DPCD_BYTES	0x1000U
#define I915_XD_FAKE_AUX_I2C_BYTES	256U

/*
 * The AUX sink behind one channel of the model.
 *
 * The test fills dpcd (the native address space) and i2c (an EEPROM on
 * the I2C bus, read from an offset a one-byte I2C write sets) and
 * scripts the answers of the next transactions; a transaction past the
 * script is answered with ACK.  The model records every request it was
 * sent and how many transactions it saw.
 */
struct i915_xd_fake_aux {
	unsigned channel;
	uint8_t dpcd[I915_XD_FAKE_AUX_DPCD_BYTES];
	uint8_t i2c[I915_XD_FAKE_AUX_I2C_BYTES];
	unsigned i2c_offset;
	unsigned answers[I915_XD_FAKE_AUX_SCRIPT];
	unsigned params[I915_XD_FAKE_AUX_SCRIPT];
	unsigned script_length;
	unsigned script_next;
	unsigned transactions;
	uint8_t last_request[20];
	unsigned last_request_bytes;
};

/*
 * What the modelled GMBUS target does with the next data cycle
 * (i915_xd_fake_gmbus_script()).
 *
 *   ACK      answers: a read with its bytes, a write by taking them
 *   NAK      refuses the cycle
 *   STUCK    the controller never reports ready, nor a refusal
 */
#define I915_XD_FAKE_GMBUS_ACK		0U
#define I915_XD_FAKE_GMBUS_NAK		1U
#define I915_XD_FAKE_GMBUS_STUCK	2U

/* The most cycles one script holds, and the room of the target's memory. */
#define I915_XD_FAKE_GMBUS_SCRIPT	16U
#define I915_XD_FAKE_GMBUS_BYTES	1024U

/*
 * The DDC behind the GMBUS controller of the model.
 *
 * An EEPROM answers at address (0x50 for an EDID) from an offset that
 * an index cycle, a two-byte index or a one-byte write sets, inside the
 * 256-byte segment the segment pointer (address 0x30) selects; any
 * other address refuses.  The test fills memory and scripts the answers
 * of the next data cycles; a cycle past the script is answered with
 * ACK.  stays_active keeps the bus active after a refusal, lingers keeps
 * it active after a stop.  The model counts the data cycles it saw,
 * keeps the command words of the first ones, and keeps the bytes the
 * last write cycle sent.
 */
struct i915_xd_fake_gmbus {
	unsigned address;
	uint8_t memory[I915_XD_FAKE_GMBUS_BYTES];
	unsigned segment;
	unsigned offset;
	unsigned left;
	int writing;
	int active;
	int refused;
	int stuck;
	int stays_active;
	int lingers;
	unsigned answers[I915_XD_FAKE_GMBUS_SCRIPT];
	unsigned script_length;
	unsigned script_next;
	unsigned cycles;
	uint32_t commands[I915_XD_FAKE_GMBUS_SCRIPT];
	uint8_t written[64];
	unsigned written_bytes;
};

void i915_xd_fake_reset(void);
void i915_xd_fake_hooks(struct i915_xd_io_hooks *hooks);
void i915_xd_fake_set(uint32_t offset, uint32_t value);
uint32_t i915_xd_fake_get(uint32_t offset);
uint64_t i915_xd_fake_now(void);
void i915_xd_fake_later(unsigned delay_us, uint32_t offset, uint32_t clear, uint32_t set);
void i915_xd_fake_rule_mirror(uint32_t offset, uint32_t source_mask, uint32_t target, int shift, unsigned delay_us);
void i915_xd_fake_rule_call(uint32_t offset, i915_xd_fake_write_fn function, void *context);
void i915_xd_fake_rule_read(uint32_t offset, i915_xd_fake_read_fn function, void *context);
uint32_t i915_xd_fake_previous(void);
void i915_xd_fake_aux_attach(struct i915_xd_fake_aux *sink, unsigned channel);
void i915_xd_fake_aux_script(struct i915_xd_fake_aux *sink, unsigned answer, unsigned param, unsigned count);
void i915_xd_fake_gmbus_attach(struct i915_xd_fake_gmbus *target, unsigned address);
void i915_xd_fake_gmbus_script(struct i915_xd_fake_gmbus *target, unsigned answer, unsigned count);
unsigned i915_xd_fake_reads(void);
unsigned i915_xd_fake_writes(void);
unsigned i915_xd_fake_lock_violations(void);
unsigned i915_xd_fake_locks_held(void);

#endif /* DRIVERS_GPU_I915_TESTS_DISPLAY_XD_FAKE_REGS_H */
