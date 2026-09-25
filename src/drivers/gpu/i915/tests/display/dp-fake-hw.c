/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register model of the eDP first stage (see dp-fake-hw.h).
 *
 * Register offsets and bit positions are written out here independently of
 * the driver's definitions on purpose: a wrong definition on the driver side
 * must show up as a test failure, not cancel out.  Sources: the PCH PPS
 * block at 0xC7200 and DDI AUX channel A at 0x64010 as documented for Gen12
 * display, and the DisplayPort AUX message format (4-bit command, 20-bit
 * address, length - 1).
 */

#include "dp-fake-hw.h"
#include <kern/kcrt.h>

#include "../../display/dp-sink.h"


/* PPS 0 and the PCH clock gating around it. */
#define I915_DP_FAKE_PP_STATUS          0xC7200U
#define I915_DP_FAKE_PP_CONTROL         0xC7204U
#define I915_DP_FAKE_PP_ON              0xC7208U
#define I915_DP_FAKE_PP_OFF             0xC720CU
#define I915_DP_FAKE_PPS1_FIRST         0xC7300U
#define I915_DP_FAKE_PPS1_LAST          0xC730CU
#define I915_DP_FAKE_SOUTH_CHICKEN1     0xC2000U
#define I915_DP_FAKE_SOUTH_DSPCLK_GATE  0xC2020U

/* AUX channel A. */
#define I915_DP_FAKE_AUX_CTL            0x64010U
#define I915_DP_FAKE_AUX_DATA0          0x64014U

/* PP_CONTROL and PP_STATUS bits. */
#define I915_DP_FAKE_PPC_POWER_ON       (1U << 0)
#define I915_DP_FAKE_PPC_FORCE_VDD      (1U << 3)
#define I915_DP_FAKE_PPS_ON             (1U << 31)

/* AUX_CTL bits. */
#define I915_DP_FAKE_AUX_BUSY           (1U << 31)
#define I915_DP_FAKE_AUX_DONE           (1U << 30)
#define I915_DP_FAKE_AUX_TIMEOUT        (1U << 28)
#define I915_DP_FAKE_AUX_RXERR          (1U << 25)
#define I915_DP_FAKE_AUX_SIZE_SHIFT     20
#define I915_DP_FAKE_AUX_SIZE_MASK      (0x1fU << I915_DP_FAKE_AUX_SIZE_SHIFT)
#define I915_DP_FAKE_AUX_STATUS         (I915_DP_FAKE_AUX_BUSY | I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_TIMEOUT | \
					 I915_DP_FAKE_AUX_RXERR | I915_DP_FAKE_AUX_SIZE_MASK)

/* The I2C addresses of the EDID EEPROM and its segment pointer. */
#define I915_DP_FAKE_I2C_EDID           0x50U
#define I915_DP_FAKE_I2C_SEGMENT        0x30U

/*
 * One AUX request as the model decoded it from the data registers.
 */
struct i915_dp_fake_request {
	uint8_t msg[20];
	unsigned size;          /* the message size written to AUX_CTL */
	unsigned command;
	uint32_t address;
	unsigned length;        /* the data length the header asks for; 0 for an address-only request */
	int is_i2c;
	int is_read;
};

static void i915_dp_fake_aux_finish(struct i915_dp_fake_hw *hw, uint32_t control, uint32_t status);
static void i915_dp_fake_aux_reply(struct i915_dp_fake_hw *hw, uint32_t control, const uint8_t *reply, unsigned count);
static void i915_dp_fake_aux_decode(const struct i915_dp_fake_hw *hw, uint32_t control, struct i915_dp_fake_request *request);
static int i915_dp_fake_sink_on(const struct i915_dp_fake_hw *hw);
static unsigned i915_dp_fake_next_fault(struct i915_dp_fake_hw *hw, const struct i915_dp_fake_request *request, unsigned *param);
static int i915_dp_fake_aux_fault(struct i915_dp_fake_hw *hw, uint32_t control, unsigned fault, unsigned param);
static unsigned i915_dp_fake_aux_native(struct i915_dp_fake_hw *hw, const struct i915_dp_fake_request *request, uint8_t *reply);
static unsigned i915_dp_fake_aux_i2c(struct i915_dp_fake_hw *hw, const struct i915_dp_fake_request *request, uint8_t *reply);
static void i915_dp_fake_aux_transaction(struct i915_dp_fake_hw *hw, uint32_t control);
static uint32_t i915_dp_fake_pp_status(const struct i915_dp_fake_hw *hw);
static uint32_t i915_dp_fake_read(void *ctx, uint32_t reg);
static void i915_dp_fake_write_aux_ctl(struct i915_dp_fake_hw *hw, uint32_t value);
static void i915_dp_fake_write_pp_control(struct i915_dp_fake_hw *hw, uint32_t value);
static void i915_dp_fake_write(void *ctx, uint32_t reg, uint32_t value);
static int i915_dp_fake_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned fast_us, unsigned slow_ms, uint32_t *out);
static void i915_dp_fake_sleep_us(void *ctx, unsigned us);
static uint64_t i915_dp_fake_now_ms(void *ctx);
static int i915_dp_fake_power_get(void *ctx, int domain);
static void i915_dp_fake_power_put(void *ctx, int domain);
static void i915_dp_fake_power_put_async(void *ctx, int domain);
static void i915_dp_fake_lock(void *ctx, int which);
static void i915_dp_fake_unlock(void *ctx, int which);
static int i915_dp_fake_delayed_queue(void *ctx, int which, unsigned delay_ms);
static int i915_dp_fake_delayed_cancel(void *ctx, int which, int sync);
static int i915_dp_fake_delayed_pending(void *ctx, int which);
static void i915_dp_fake_release_parked(struct i915_dp_fake_hw *hw, unsigned slot);

/*
 * Starts the model afresh with the sink's DPCD pages and EDID.
 *
 * Any of the DPCD pages may be NULL (left zero).  The clock starts at an
 * arbitrary non-zero boot time.
 */
void
drv_i915_dp_fake_init(
	struct i915_dp_fake_hw *hw,
	const uint8_t *dpcd_000,
	const uint8_t *dpcd_100,
	const uint8_t *dpcd_700,
	const uint8_t *edid,
	unsigned edid_size)
{
	/* Starts every register, counter and fault at zero, 5 s after boot. */
	kern_memset(hw, 0, sizeof(*hw));
	hw->now_us = 5000000U;

	/* Loads the sink's receiver caps, link configuration and eDP display-control pages. */
	if (dpcd_000 != NULL)
		kern_memcpy(hw->dpcd + 0x000U, dpcd_000, 256U);
	if (dpcd_100 != NULL)
		kern_memcpy(hw->dpcd + 0x100U, dpcd_100, 256U);
	if (dpcd_700 != NULL)
		kern_memcpy(hw->dpcd + 0x700U, dpcd_700, 256U);

	/* Loads the EDID EEPROM, as much as it holds. */
	if (edid_size > sizeof(hw->edid))
		edid_size = sizeof(hw->edid);

	if (edid_size != 0U)
		kern_memcpy(hw->edid, edid, edid_size);

	hw->edid_size = edid_size;
}

/*
 * Scripts the faults of the next AUX transactions, one per transaction.
 *
 * params may be NULL (all zero).  After the script every transaction is
 * served normally.
 */
void
drv_i915_dp_fake_script(
	struct i915_dp_fake_hw *hw,
	unsigned count,
	const uint8_t *faults,
	const uint8_t *params)
{
	unsigned i;

	/* A script longer than the model holds is cut. */
	if (count > I915_DP_FAKE_SCRIPT_MAX)
		count = I915_DP_FAKE_SCRIPT_MAX;

	/* Copies the script. */
	for (i = 0U; i < count; i++) {
		hw->script[i].fault = faults[i];
		hw->script[i].param = 0U;
		if (params != NULL)
			hw->script[i].param = params[i];
	}

	/* Starts it at the next transaction. */
	hw->script_len = count;
	hw->script_pos = 0U;
}

/*
 * Hands the model to the driver as its environment, and names the world
 * whose delayed work the model runs.
 */
void
drv_i915_dp_fake_bind_env(
	struct i915_dp_fake_hw *hw,
	struct i915_dp_env *env,
	struct i915_dp_world *world)
{
	/* Starts the environment's bookkeeping at zero. */
	kern_memset(env, 0, sizeof(*env));

	/* The registers, the clock and the waits. */
	env->ctx = hw;
	env->read32 = i915_dp_fake_read;
	env->write32 = i915_dp_fake_write;
	env->wait_reg = i915_dp_fake_wait_reg;
	env->sleep_us = i915_dp_fake_sleep_us;
	env->now_ms = i915_dp_fake_now_ms;

	/* The power-domain references. */
	env->power_get = i915_dp_fake_power_get;
	env->power_put = i915_dp_fake_power_put;
	env->power_put_async = i915_dp_fake_power_put_async;

	/* The locks and the delayed work. */
	env->lock = i915_dp_fake_lock;
	env->unlock = i915_dp_fake_unlock;
	env->delayed_queue = i915_dp_fake_delayed_queue;
	env->delayed_cancel = i915_dp_fake_delayed_cancel;
	env->delayed_pending = i915_dp_fake_delayed_pending;

	/* The world the due work runs on; no channel is stuck. */
	hw->world = world;
	hw->stuck_until_us = 0U;
}

/*
 * Runs what is due at the model's current time: the delayed work, then the
 * release of the parked power references.
 *
 * Returns the number of work bodies that ran.
 */
unsigned
drv_i915_dp_fake_run_due(
	struct i915_dp_fake_hw *hw)
{
	unsigned ran;
	unsigned slot;

	/* Runs the delayed VDD-off when it is due. */
	ran = 0U;
	if (hw->work_pending && hw->now_us >= hw->work_due_us) {
		hw->work_pending = 0;
		hw->work_ran++;
		drv_i915_edp_work_run(hw->world, I915_DP_WORK_VDD_OFF);
		ran++;
	}

	/* Releases every parked reference whose 100 ms have passed. */
	for (slot = 0U; slot < 2U; slot++) {
		if (hw->parked[slot] && hw->now_us >= hw->parked_due_us[slot])
			i915_dp_fake_release_parked(hw, slot);
	}

	/* Succeeded: reports how many bodies ran. */
	return ran;
}

/*
 * Releases every parked power reference now
 * (intel_display_power_flush_work()).
 */
void
drv_i915_dp_fake_flush_async(
	struct i915_dp_fake_hw *hw)
{
	unsigned slot;

	/* Releases the parked references of both domains. */
	for (slot = 0U; slot < 2U; slot++) {
		if (hw->parked[slot])
			i915_dp_fake_release_parked(hw, slot);
	}
}

/* Ends an AUX transaction with a status, keeping the control bits written. */
static void
i915_dp_fake_aux_finish(
	struct i915_dp_fake_hw *hw,
	uint32_t control,
	uint32_t status)
{
	/* The status bits replace those of the request. */
	hw->aux_ctl = (control & ~I915_DP_FAKE_AUX_STATUS) | status;
}

/* Ends an AUX transaction with a reply packed big-endian into the data registers. */
static void
i915_dp_fake_aux_reply(
	struct i915_dp_fake_hw *hw,
	uint32_t control,
	const uint8_t *reply,
	unsigned count)
{
	unsigned i;

	/* Clears the data registers. */
	for (i = 0U; i < 5U; i++)
		hw->aux_data[i] = 0U;

	/* Packs the reply, most significant byte first. */
	for (i = 0U; i < count && i < 20U; i++)
		hw->aux_data[i >> 2] |= (uint32_t)reply[i] << (24U - 8U * (i & 3U));

	/* Reports done with the reply's size. */
	i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | ((uint32_t)count << I915_DP_FAKE_AUX_SIZE_SHIFT));
}

/* Decodes the request the driver wrote into the data registers. */
static void
i915_dp_fake_aux_decode(
	const struct i915_dp_fake_hw *hw,
	uint32_t control,
	struct i915_dp_fake_request *request)
{
	unsigned i;

	/* Unpacks the message, most significant byte first. */
	for (i = 0U; i < 20U; i++)
		request->msg[i] = (uint8_t)(hw->aux_data[i >> 2] >> (24U - 8U * (i & 3U)));

	/* The header: command nibble, 20-bit address, length - 1 when the message carries it. */
	request->size = (control & I915_DP_FAKE_AUX_SIZE_MASK) >> I915_DP_FAKE_AUX_SIZE_SHIFT;
	request->command = request->msg[0] >> 4;
	request->address = ((uint32_t)(request->msg[0] & 0xfU) << 16) | ((uint32_t)request->msg[1] << 8) | request->msg[2];
	request->length = 0U;
	if (request->size >= 4U)
		request->length = (unsigned)request->msg[3] + 1U;

	/* Bit 3 of the command is set for native transactions; bits 1..0 = 1 for a read. */
	request->is_i2c = 0;
	if ((request->command & 0x8U) == 0U)
		request->is_i2c = 1;

	request->is_read = 0;
	if ((request->command & 0x3U) == 1U)
		request->is_read = 1;
}

/* Tells whether the sink is powered: panel power on, or VDD on for long enough. */
static int
i915_dp_fake_sink_on(
	const struct i915_dp_fake_hw *hw)
{
	/* Panel power powers the sink. */
	if ((hw->pp_control & I915_DP_FAKE_PPC_POWER_ON) != 0U)
		return 1;

	/* So does VDD, once the sink had its power-up time. */
	if ((hw->pp_control & I915_DP_FAKE_PPC_FORCE_VDD) != 0U &&
	    hw->now_us - hw->vdd_on_since_us >= hw->sink_power_up_us)
		return 1;

	/* Succeeded: the sink is unpowered. */
	return 0;
}

/* Takes the fault the transaction meets from the script, or from the persistent I2C fault. */
static unsigned
i915_dp_fake_next_fault(
	struct i915_dp_fake_hw *hw,
	const struct i915_dp_fake_request *request,
	unsigned *param)
{
	unsigned fault;

	/* The script comes first; after it, the fault of every I2C data read. */
	fault = I915_DP_FAKE_OK;
	*param = 0U;
	if (hw->script_pos < hw->script_len) {
		fault = hw->script[hw->script_pos].fault;
		*param = hw->script[hw->script_pos].param;
		hw->script_pos++;
	} else if (hw->fault_every_i2c_read != 0 &&
		   request->is_i2c &&
		   request->is_read &&
		   request->length != 0U) {
		fault = (unsigned)hw->fault_every_i2c_read;
	}

	/* An I2C reply code on a native transaction does not exist: served normally. */
	if (fault == I915_DP_FAKE_I2C_DEFER || fault == I915_DP_FAKE_I2C_NACK) {
		if (!request->is_i2c)
			fault = I915_DP_FAKE_OK;
	}

	/* Succeeded: reports the fault. */
	return fault;
}

/* Ends a transaction on a fault that replaces the reply; returns 1 when it did. */
static int
i915_dp_fake_aux_fault(
	struct i915_dp_fake_hw *hw,
	uint32_t control,
	unsigned fault,
	unsigned param)
{
	uint8_t reply[1];

	/* Faults of the channel end with a status; faults of the sink with a reply code. */
	switch (fault) {
	case I915_DP_FAKE_HW_TIMEOUT:
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_TIMEOUT);
		return 1;
	case I915_DP_FAKE_RECEIVE_ERROR:
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_RXERR);
		return 1;
	case I915_DP_FAKE_STUCK_BUSY:
		/* The channel stays busy; reading it after the release time turns it into a hardware timeout. */
		hw->aux_ctl = (control & ~I915_DP_FAKE_AUX_STATUS) | I915_DP_FAKE_AUX_BUSY;
		if (param == 0U)
			param = 25U;

		hw->stuck_until_us = hw->now_us + (uint64_t)param * 1000U;
		return 1;
	case I915_DP_FAKE_BAD_SIZE_ZERO:
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE);
		return 1;
	case I915_DP_FAKE_BAD_SIZE_BIG:
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | (21U << I915_DP_FAKE_AUX_SIZE_SHIFT));
		return 1;
	case I915_DP_FAKE_NATIVE_DEFER:
		reply[0] = 0x20U;
		i915_dp_fake_aux_reply(hw, control, reply, 1U);
		return 1;
	case I915_DP_FAKE_NATIVE_NACK:
		reply[0] = 0x10U;
		i915_dp_fake_aux_reply(hw, control, reply, 1U);
		return 1;
	case I915_DP_FAKE_I2C_DEFER:
		reply[0] = 0x80U;
		i915_dp_fake_aux_reply(hw, control, reply, 1U);
		return 1;
	case I915_DP_FAKE_I2C_NACK:
		reply[0] = 0x40U;
		i915_dp_fake_aux_reply(hw, control, reply, 1U);
		return 1;
	case I915_DP_FAKE_INVALID_REPLY:
		reply[0] = 0x30U;
		i915_dp_fake_aux_reply(hw, control, reply, 1U);
		return 1;
	default:
		break;
	}

	/* Succeeded: the transaction is served normally. */
	return 0;
}

/* Serves a native DPCD read or write; returns the reply's length. */
static unsigned
i915_dp_fake_aux_native(
	struct i915_dp_fake_hw *hw,
	const struct i915_dp_fake_request *request,
	uint8_t *reply)
{
	unsigned length;
	unsigned i;

	/* A read returns up to 16 DPCD bytes; beyond the model's DPCD they read 0. */
	if (request->is_read) {
		hw->aux_native_reads++;
		length = request->length;
		if (length > 16U)
			length = 16U;

		for (i = 0U; i < length; i++) {
			reply[1U + i] = 0U;
			if (request->address + i < sizeof(hw->dpcd))
				reply[1U + i] = hw->dpcd[request->address + i];
		}

		return 1U + length;
	}

	/* A write stores the bytes the message carries. */
	hw->aux_native_writes++;
	for (i = 0U; i < request->length && 4U + i < request->size; i++) {
		if (request->address + i < sizeof(hw->dpcd))
			hw->dpcd[request->address + i] = request->msg[4U + i];
	}

	/* The owner of the sink's behaviour sees the write after it is stored. */
	if (hw->on_dpcd_write != NULL)
		hw->on_dpcd_write(hw->on_dpcd_write_ctx, request->address, request->length);

	/* Succeeded: the reply is the ACK alone. */
	return 1U;
}

/* Serves an I2C-over-AUX read or write of the EDID EEPROM; returns the reply's length. */
static unsigned
i915_dp_fake_aux_i2c(
	struct i915_dp_fake_hw *hw,
	const struct i915_dp_fake_request *request,
	uint8_t *reply)
{
	unsigned length;
	unsigned at;
	unsigned i;

	/* A read returns up to 16 EEPROM bytes from the pointer; past the EEPROM they read 0xff. */
	if (request->is_read) {
		hw->aux_i2c_reads++;
		if (request->address != I915_DP_FAKE_I2C_EDID) {
			reply[0] = 0x40U;
			return 1U;
		}

		length = request->length;
		if (length > 16U)
			length = 16U;

		for (i = 0U; i < length; i++) {
			at = (unsigned)hw->i2c_segment * 256U + hw->i2c_offset;
			reply[1U + i] = 0xffU;
			if (at < hw->edid_size)
				reply[1U + i] = hw->edid[at];

			hw->i2c_offset++;
		}

		return 1U + length;
	}

	/* A write sets the EEPROM pointer (its last byte) or the segment; other addresses NACK. */
	hw->aux_i2c_writes++;
	if (request->address == I915_DP_FAKE_I2C_EDID) {
		if (request->length != 0U && request->size > 4U)
			hw->i2c_offset = request->msg[4U + (request->size - 5U)];
	} else if (request->address == I915_DP_FAKE_I2C_SEGMENT) {
		if (request->length != 0U && request->size > 4U)
			hw->i2c_segment = request->msg[4U];
	} else {
		reply[0] = 0x40U;
	}

	/* Succeeded: the reply is the reply code alone. */
	return 1U;
}

/* Runs one AUX transaction the driver started by setting SEND_BUSY. */
static void
i915_dp_fake_aux_transaction(
	struct i915_dp_fake_hw *hw,
	uint32_t control)
{
	struct i915_dp_fake_request request;
	uint8_t reply[21];
	unsigned fault;
	unsigned param;
	unsigned count;
	int sink_on;
	int faulted;

	/* Decodes the request; a size outside 3..20 is a receive error. */
	hw->aux_transactions++;
	i915_dp_fake_aux_decode(hw, control, &request);
	if (request.size < 3U || request.size > 20U) {
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_RXERR);
		return;
	}

	/* Counts every power the transaction lacks. */
	sink_on = i915_dp_fake_sink_on(hw);
	if (hw->refs_core <= 0)
		hw->aux_without_core_power++;
	if (hw->refs_aux <= 0)
		hw->aux_without_aux_power++;
	if (!sink_on)
		hw->aux_without_sink_power++;

	/* Without its power nobody answers: a hardware timeout. */
	if (hw->refs_core <= 0 || hw->refs_aux <= 0 || !sink_on) {
		i915_dp_fake_aux_finish(hw, control, I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_TIMEOUT);
		return;
	}

	/* A fault that replaces the reply ends the transaction. */
	fault = i915_dp_fake_next_fault(hw, &request, &param);
	faulted = i915_dp_fake_aux_fault(hw, control, fault, param);
	if (faulted)
		return;

	/* Serves the request; the reply starts with ACK. */
	reply[0] = 0x00U;
	if (!request.is_i2c) {
		count = i915_dp_fake_aux_native(hw, &request, reply);
	} else {
		count = i915_dp_fake_aux_i2c(hw, &request, reply);
	}

	/* A short reply keeps only param data bytes. */
	if (fault == I915_DP_FAKE_SHORT_REPLY && request.is_read && count > 1U + param)
		count = 1U + param;

	/* Corrupt data inverts the first data byte. */
	if (fault == I915_DP_FAKE_CORRUPT_DATA && request.is_read && count > 1U)
		reply[1] = (uint8_t)~reply[1];

	i915_dp_fake_aux_reply(hw, control, reply, count);
}

/* Returns PP_STATUS: panel power is never requested in this stage, so VDD alone leaves it idle-off. */
static uint32_t
i915_dp_fake_pp_status(
	const struct i915_dp_fake_hw *hw)
{
	/* Panel power on: on, and idle in the power-on state. */
	if ((hw->pp_control & I915_DP_FAKE_PPC_POWER_ON) != 0U)
		return I915_DP_FAKE_PPS_ON | 0x8U;

	/* Succeeded: off. */
	return 0U;
}

/* Reads a register of the model. */
static uint32_t
i915_dp_fake_read(
	void *ctx,
	uint32_t reg)
{
	struct i915_dp_fake_hw *hw;

	hw = ctx;

	/* AUX_CTL: a stuck channel whose time has passed turns into a hardware timeout. */
	if (reg == I915_DP_FAKE_AUX_CTL) {
		if ((hw->aux_ctl & I915_DP_FAKE_AUX_BUSY) != 0U && hw->now_us >= hw->stuck_until_us)
			i915_dp_fake_aux_finish(hw, hw->aux_ctl, I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_TIMEOUT);

		return hw->aux_ctl;
	}

	/* The five AUX data registers. */
	if (reg >= I915_DP_FAKE_AUX_DATA0 &&
	    reg < I915_DP_FAKE_AUX_DATA0 + 20U &&
	    ((reg - I915_DP_FAKE_AUX_DATA0) & 3U) == 0U)
		return hw->aux_data[(reg - I915_DP_FAKE_AUX_DATA0) >> 2];

	/* PPS 0 and the PCH clock gating. */
	switch (reg) {
	case I915_DP_FAKE_PP_STATUS:
		return i915_dp_fake_pp_status(hw);
	case I915_DP_FAKE_PP_CONTROL:
		return hw->pp_control;
	case I915_DP_FAKE_PP_ON:
		return hw->pp_on_delays;
	case I915_DP_FAKE_PP_OFF:
		return hw->pp_off_delays;
	case I915_DP_FAKE_SOUTH_CHICKEN1:
		return hw->south_chicken1;
	case I915_DP_FAKE_SOUTH_DSPCLK_GATE:
		return hw->south_dspclk_gate_d;
	default:
		break;
	}

	/* PPS 1 reads as never programmed. */
	if (reg >= I915_DP_FAKE_PPS1_FIRST && reg <= I915_DP_FAKE_PPS1_LAST)
		return 0U;

	/* Anything else is outside what this stage may touch. */
	hw->unknown_reg_reads++;
	hw->last_unknown_reg = reg;

	/* Succeeded: an unknown register reads 0. */
	return 0U;
}

/* Writes AUX_CTL: clears the write-one-to-clear bits and starts a transaction on SEND_BUSY. */
static void
i915_dp_fake_write_aux_ctl(
	struct i915_dp_fake_hw *hw,
	uint32_t value)
{
	uint32_t current;

	/* A busy channel ignores writes. */
	current = i915_dp_fake_read(hw, I915_DP_FAKE_AUX_CTL);
	if ((current & I915_DP_FAKE_AUX_BUSY) != 0U)
		return;

	/* DONE, TIME_OUT_ERROR and RECEIVE_ERROR are write-one-to-clear. */
	hw->aux_ctl &= ~(value & (I915_DP_FAKE_AUX_DONE | I915_DP_FAKE_AUX_TIMEOUT | I915_DP_FAKE_AUX_RXERR));

	/* SEND_BUSY starts a transaction; otherwise the control bits are stored. */
	if ((value & I915_DP_FAKE_AUX_BUSY) != 0U) {
		i915_dp_fake_aux_transaction(hw, value);
	} else {
		hw->aux_ctl = (hw->aux_ctl & I915_DP_FAKE_AUX_STATUS) | (value & ~I915_DP_FAKE_AUX_STATUS);
	}
}

/* Writes PP_CONTROL and counts the VDD edges and a write without the core power. */
static void
i915_dp_fake_write_pp_control(
	struct i915_dp_fake_hw *hw,
	uint32_t value)
{
	/* The sequencer needs the display core's power. */
	if (hw->refs_core <= 0)
		hw->pp_writes_without_core_power++;

	/* VDD switched on: from now on the sink counts its power-up time. */
	if ((value & I915_DP_FAKE_PPC_FORCE_VDD) != 0U && (hw->pp_control & I915_DP_FAKE_PPC_FORCE_VDD) == 0U) {
		hw->vdd_on_events++;
		hw->vdd_on_since_us = hw->now_us;
	}

	/* VDD switched off. */
	if ((value & I915_DP_FAKE_PPC_FORCE_VDD) == 0U && (hw->pp_control & I915_DP_FAKE_PPC_FORCE_VDD) != 0U)
		hw->vdd_off_events++;

	hw->pp_control = value;
}

/* Writes a register of the model. */
static void
i915_dp_fake_write(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_dp_fake_hw *hw;

	hw = ctx;

	/* AUX_CTL runs the transactions. */
	if (reg == I915_DP_FAKE_AUX_CTL) {
		i915_dp_fake_write_aux_ctl(hw, value);
		return;
	}

	/* The five AUX data registers. */
	if (reg >= I915_DP_FAKE_AUX_DATA0 &&
	    reg < I915_DP_FAKE_AUX_DATA0 + 20U &&
	    ((reg - I915_DP_FAKE_AUX_DATA0) & 3U) == 0U) {
		hw->aux_data[(reg - I915_DP_FAKE_AUX_DATA0) >> 2] = value;
		return;
	}

	/* PPS 0 and the PCH clock gating. */
	switch (reg) {
	case I915_DP_FAKE_PP_CONTROL:
		i915_dp_fake_write_pp_control(hw, value);
		return;
	case I915_DP_FAKE_PP_ON:
		hw->pp_on_delays = value;
		return;
	case I915_DP_FAKE_PP_OFF:
		hw->pp_off_delays = value;
		return;
	case I915_DP_FAKE_SOUTH_CHICKEN1:
		hw->south_chicken1 = value;
		return;
	case I915_DP_FAKE_SOUTH_DSPCLK_GATE:
		hw->south_dspclk_gate_d = value;
		return;
	default:
		break;
	}

	/* Anything else is outside what this stage may touch. */
	hw->unknown_reg_writes++;
	hw->last_unknown_reg = reg;
}

/*
 * Waits for a register value, polling every 500 us of model time
 * (__intel_wait_for_register()); 0, or -ETIMEDOUT (Linux numbering).
 */
static int
i915_dp_fake_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *out)
{
	struct i915_dp_fake_hw *hw;
	uint64_t deadline;
	uint32_t current;

	hw = ctx;

	/* Polls until the value is there or both waits have passed. */
	deadline = hw->now_us + fast_us + (uint64_t)slow_ms * 1000U;
	for (;;) {
		current = i915_dp_fake_read(hw, reg);
		if (out != NULL)
			*out = current;
		if ((current & mask) == value)
			return 0;
		if (hw->now_us >= deadline)
			break;

		hw->now_us += 500U;
	}

	/* Reports the timeout. */
	hw->wait_timeouts++;
	return -I915_EDP_ETIMEDOUT;
}

/* Sleeps in model time. */
static void
i915_dp_fake_sleep_us(
	void *ctx,
	unsigned us)
{
	struct i915_dp_fake_hw *hw;

	/* Advances the clock. */
	hw = ctx;
	hw->now_us += us;
}

/* Returns the model's clock in milliseconds. */
static uint64_t
i915_dp_fake_now_ms(
	void *ctx)
{
	struct i915_dp_fake_hw *hw;

	hw = ctx;

	/* Succeeded: reports the time. */
	return hw->now_us / 1000U;
}

/* Takes a power-domain reference; a parked one is handed back without a hardware change. */
static int
i915_dp_fake_power_get(
	void *ctx,
	int domain)
{
	struct i915_dp_fake_hw *hw;
	int slot;

	hw = ctx;

	/* A failing power layer refuses every get. */
	if (hw->fail_power_get)
		return -I915_EDP_EIO;

	/* Slot 0 is the display core, slot 1 the AUX domain. */
	slot = 1;
	if (domain == 0)
		slot = 0;

	/* A parked reference of the domain is grabbed back. */
	if (hw->parked[slot]) {
		hw->parked[slot] = 0;
		hw->async_grabbed++;
		return 0;
	}

	/* Otherwise a new reference is counted. */
	if (slot == 0) {
		hw->refs_core++;
	} else {
		hw->refs_aux++;
	}

	/* Succeeded: the reference is held. */
	return 0;
}

/* Returns a power-domain reference at once. */
static void
i915_dp_fake_power_put(
	void *ctx,
	int domain)
{
	struct i915_dp_fake_hw *hw;

	/* The reference is gone from the hardware side. */
	hw = ctx;
	if (domain == 0) {
		hw->refs_core--;
	} else {
		hw->refs_aux--;
	}
}

/*
 * Returns a power-domain reference asynchronously: an ordinary put unless it
 * is the last one, which is parked for 100 ms.
 */
static void
i915_dp_fake_power_put_async(
	void *ctx,
	int domain)
{
	struct i915_dp_fake_hw *hw;
	int *refs;
	int slot;

	hw = ctx;

	/* Slot 0 is the display core, slot 1 the AUX domain. */
	slot = 1;
	refs = &hw->refs_aux;
	if (domain == 0) {
		slot = 0;
		refs = &hw->refs_core;
	}

	/* Not the last reference: an ordinary put. */
	if (*refs > 1) {
		(*refs)--;
		return;
	}

	/* The last one stays counted while it is parked. */
	hw->parked[slot] = 1;
	hw->parked_due_us[slot] = hw->now_us + 100000U;
	hw->async_parked++;
}

/* Takes a lock; a second acquisition would deadlock a real mutex and is counted. */
static void
i915_dp_fake_lock(
	void *ctx,
	int which)
{
	struct i915_dp_fake_hw *hw;

	hw = ctx;

	/* A held lock taken again is a deadlock on the real mutex. */
	if (hw->lock_held[which])
		hw->lock_errors++;

	hw->lock_held[which] = 1;
	hw->lock_acquisitions[which]++;
}

/* Releases a lock; releasing a free one is counted. */
static void
i915_dp_fake_unlock(
	void *ctx,
	int which)
{
	struct i915_dp_fake_hw *hw;

	hw = ctx;

	/* A free lock released is a driver bug. */
	if (!hw->lock_held[which])
		hw->lock_errors++;

	hw->lock_held[which] = 0;
}

/* Queues the delayed work; returns 1 when newly queued, 0 when already pending. */
static int
i915_dp_fake_delayed_queue(
	void *ctx,
	int which,
	unsigned delay_ms)
{
	struct i915_dp_fake_hw *hw;

	UNUSED_PARAMETER(which);

	hw = ctx;

	/* A pending work keeps its deadline. */
	if (hw->work_pending)
		return 0;

	/* Queues it delay_ms from now. */
	hw->work_pending = 1;
	hw->work_due_us = hw->now_us + (uint64_t)delay_ms * 1000U;
	hw->work_queued++;

	/* Succeeded: newly queued. */
	return 1;
}

/*
 * Cancels the delayed work; returns 1 when it was pending.  A synchronous
 * cancel under the lock the body takes would deadlock and is counted.
 */
static int
i915_dp_fake_delayed_cancel(
	void *ctx,
	int which,
	int sync)
{
	struct i915_dp_fake_hw *hw;
	int was_pending;

	UNUSED_PARAMETER(which);

	hw = ctx;

	/* A synchronous cancel waits for the body, which takes the PPS lock. */
	if (sync) {
		hw->work_cancel_syncs++;
		if (hw->lock_held[I915_DP_LOCK_PPS])
			hw->lock_errors++;
	}

	/* Counts a cancelled work and drops it. */
	was_pending = hw->work_pending;
	if (was_pending)
		hw->work_cancelled++;

	hw->work_pending = 0;

	/* Succeeded: reports whether it was pending. */
	return was_pending;
}

/* Tells whether the delayed work is pending. */
static int
i915_dp_fake_delayed_pending(
	void *ctx,
	int which)
{
	struct i915_dp_fake_hw *hw;

	UNUSED_PARAMETER(which);

	hw = ctx;

	/* Succeeded: reports the pending flag. */
	return hw->work_pending;
}

/* Releases a parked power reference. */
static void
i915_dp_fake_release_parked(
	struct i915_dp_fake_hw *hw,
	unsigned slot)
{
	/* The parked reference leaves the hardware side. */
	hw->parked[slot] = 0;
	if (slot == 0U) {
		hw->refs_core--;
	} else {
		hw->refs_aux--;
	}

	hw->async_released++;
}
