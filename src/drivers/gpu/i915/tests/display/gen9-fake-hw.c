/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The model of the display 9 hardware the Gen9 host tests drive (see
 * gen9-fake-hw.h).
 */

#include "gen9-fake-hw.h"

#include "gen9-fake-regs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The DP_TP_STATUS bit of the idle patterns' end. */
#define I915_FAKE_IDLE_DONE	(1U << 25)

static uint32_t i915_fake_get(const struct i915_gen9_fake_hw *hw, uint32_t offset);
static void i915_fake_set(struct i915_gen9_fake_hw *hw, uint32_t offset, uint32_t value);
static uint32_t i915_fake_read(void *context, uint32_t offset);
static void i915_fake_write(void *context, uint32_t offset, uint32_t value);
static void i915_fake_udelay(void *context, unsigned microseconds);
static uint32_t i915_fake_pack(const uint8_t *bytes, unsigned count);
static void i915_fake_unpack(uint32_t word, uint8_t *bytes, unsigned count);
static void i915_fake_aux(struct i915_gen9_fake_hw *hw, unsigned port, uint32_t control);
static unsigned i915_fake_aux_native(struct i915_gen9_fake_sink *sink, unsigned request, uint32_t address, const uint8_t *message, unsigned size, uint8_t *reply);
static unsigned i915_fake_aux_i2c(struct i915_gen9_fake_sink *sink, unsigned request, const uint8_t *message, unsigned size, uint8_t *reply);
static void i915_fake_link_status(const struct i915_gen9_fake_sink *sink, uint8_t *status);
static void i915_fake_gmbus1(struct i915_gen9_fake_hw *hw, uint32_t value);
static int i915_fake_gmbus_port(unsigned pin);
static void i915_fake_xd_wait(void *context, unsigned microseconds);
static void i915_fake_golden_scene(void);

/*
 * The model the tests drive.
 *
 * Emptied by i915_gen9_fake_reset() at the start of each check; the test
 * runs on one thread.
 */
struct i915_gen9_fake_hw i915_gen9_fake;

/* The xd register window's hooks, reaching the model.  It never changes. */
const struct i915_xd_io_hooks i915_gen9_fake_xd_hooks = {
	i915_fake_read,
	i915_fake_write,
	i915_fake_xd_wait,
	NULL,
	&i915_gen9_fake
};

/* The MMIO BAR's stand-ins' hooks, reaching the model.  It never changes. */
const struct i915_host_mmio_hooks i915_gen9_fake_mmio = {
	i915_fake_read,
	i915_fake_write,
	&i915_gen9_fake
};

/*
 * The file of the golden record, and the number of the scene written next.
 *
 * NULL until a test opens the record; the close empties both.  The test
 * runs on one thread.
 */
static FILE *i915_fake_golden;
static unsigned i915_fake_golden_scenes;

/*
 * Reports the xd register window that reaches the model, made again on
 * every call (its record starts empty).
 */
struct i915_xd_io *
i915_gen9_fake_xd(void)
{
	static struct i915_xd_io io;

	/* Succeeded: the window on the hooks. */
	drv_i915_xd_io_init(&io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&io, &i915_gen9_fake_xd_hooks);
	return &io;
}

/* Reports a register's stored value, 0 when never written. */
uint32_t
i915_gen9_fake_get(
	uint32_t offset)
{
	uint32_t value;

	/* The model's slot. */
	value = i915_fake_get(&i915_gen9_fake, offset);

	/* Succeeded: the stored value. */
	return value;
}

/* Stores a register's value without the hardware's effects or the log. */
void
i915_gen9_fake_set(
	uint32_t offset,
	uint32_t value)
{
	/* The model's slot. */
	i915_fake_set(&i915_gen9_fake, offset, value);
}

/* Empties the model: no registers, no sinks; power gate 2's fuses distributed. */
void
i915_gen9_fake_reset(void)
{
	/* The check that ends goes to the golden record first. */
	i915_fake_golden_scene();

	/* Everything off. */
	memset(&i915_gen9_fake, 0, sizeof(i915_gen9_fake));

	/* Succeeded: the fuses follow power well 2 at once. */
	i915_fake_set(&i915_gen9_fake, I915_SKL_FUSE_STATUS, I915_SKL_FUSE_PG_DIST(2U));
}

/* Attaches a DP 1.2 sink of HBR2 x4 with TPS3 and enhanced framing, and a 1080p EDID. */
void
i915_gen9_fake_add_dp_sink(
	unsigned port,
	unsigned cr_swing)
{
	static const uint8_t edid_header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	static const uint8_t dtd_1080p[18] = {
		0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
		0x2c, 0x45, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e
	};
	struct i915_gen9_fake_sink *sink;
	unsigned index;

	/* The sink of the port: DP, with the clock recovery's swing. */
	sink = &i915_gen9_fake.sinks[port];
	sink->dp = 1;
	sink->cr_swing = cr_swing;

	/* The receiver capabilities. */
	sink->dpcd[I915_DPCD_REV] = 0x12U;
	sink->dpcd[I915_DPCD_MAX_LINK_RATE] = I915_DPCD_LINK_BW_5_4;
	sink->dpcd[I915_DPCD_MAX_LANE_COUNT] = (uint8_t)(4U | I915_DPCD_ENHANCED_FRAME_CAP | I915_DPCD_TPS3_SUPPORTED);

	/* A pattern, then the base block's header, its 1080p timing and one extension. */
	for (index = 0U; index < sizeof(sink->edid); index++)
		sink->edid[index] = (uint8_t)(index * 7U + port);
	memcpy(sink->edid, edid_header, sizeof(edid_header));
	memset(sink->edid + 54, 0, 72);
	memcpy(sink->edid + 54, dtd_1080p, sizeof(dtd_1080p));

	/* Succeeded: an EDID of two blocks. */
	sink->edid[126] = 1U;
}

/* Attaches an HDMI sink with the same EDID on the port's GMBUS pin. */
void
i915_gen9_fake_add_hdmi_sink(
	unsigned port)
{
	/* The EDID, without a DP side. */
	i915_gen9_fake_add_dp_sink(port, 0U);
	i915_gen9_fake.sinks[port].dp = 0;

	/* Succeeded: the GMBUS answers on the pin. */
	i915_gen9_fake.sinks[port].hdmi = 1;
}

/* Reports the first write to a register whose masked value matches, as its position in the log, or -1. */
int
i915_gen9_fake_written_at(
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	unsigned index;

	/* Each write in order. */
	for (index = 0U; index < i915_gen9_fake.log_count; index++) {
		if (i915_gen9_fake.log_offset[index] == offset && (i915_gen9_fake.log_value[index] & mask) == value)
			return (int)index;
	}

	/* Succeeded: never written so. */
	return -1;
}

/*
 * Starts the golden record of the scenes.
 */
int
i915_gen9_fake_golden_open(
	const char *path)
{
	/* The file the scenes go to. */
	i915_fake_golden = fopen(path, "w");
	if (i915_fake_golden == NULL)
		return errno;

	/* Succeeded: the first scene is the check before the next reset. */
	i915_fake_golden_scenes = 0U;
	return 0;
}

/*
 * Writes the last scene and ends the golden record.
 */
void
i915_gen9_fake_golden_close(void)
{
	/* Nothing was opened. */
	if (i915_fake_golden == NULL)
		return;

	/* The check that ends, then the file. */
	i915_fake_golden_scene();
	fclose(i915_fake_golden);

	/* Succeeded: no record. */
	i915_fake_golden = NULL;
	i915_fake_golden_scenes = 0U;
}

/* Reports a register's value, 0 when never written. */
static uint32_t
i915_fake_get(
	const struct i915_gen9_fake_hw *hw,
	uint32_t offset)
{
	unsigned index;

	/* The register's slot. */
	for (index = 0U; index < hw->used; index++) {
		if (hw->keys[index] == offset)
			return hw->values[index];
	}

	/* Succeeded: a register never written reads 0. */
	return 0U;
}

/* Stores a register's value. */
static void
i915_fake_set(
	struct i915_gen9_fake_hw *hw,
	uint32_t offset,
	uint32_t value)
{
	unsigned index;

	/* The register's slot, or a new one. */
	for (index = 0U; index < hw->used; index++) {
		if (hw->keys[index] == offset) {
			hw->values[index] = value;
			return;
		}
	}

	/* No slot left. */
	if (hw->used == I915_GEN9_FAKE_REGISTERS) {
		fprintf(stderr, "model: register table full at 0x%x\n", offset);
		return;
	}

	/* Succeeded: a new register. */
	hw->keys[hw->used] = offset;
	hw->values[hw->used] = value;
	hw->used++;
}

/* Reads a register of the model: status registers answer as the hardware would. */
static uint32_t
i915_fake_read(
	void *context,
	uint32_t offset)
{
	struct i915_gen9_fake_hw *hw;
	uint32_t value;
	unsigned bytes;

	/* The model the table reaches. */
	hw = context;

	/* The transports' idle patterns are always out. */
	if (offset >= I915_SKL_DP_TP_STATUS_A && offset <= I915_SKL_DP_TP_STATUS_A + 0x300U && ((offset - I915_SKL_DP_TP_STATUS_A) % 0x100U) == 0U)
		return I915_FAKE_IDLE_DONE;

	/* The frame counters move when the pipes run. */
	if (offset >= I915_SKL_PIPE_FRMCOUNT_A && offset <= I915_SKL_PIPE_FRMCOUNT_A + 0x2000U && ((offset - I915_SKL_PIPE_FRMCOUNT_A) % 0x1000U) == 0U) {
		value = i915_fake_get(hw, offset);
		if (hw->frames_run)
			i915_fake_set(hw, offset, value + 1U);
		return value;
	}

	/* A plane's live surface is the one last armed: every flip latches at once. */
	if (offset >= I915_SKL_PLANE_SURFLIVE_1_A && offset <= I915_SKL_PLANE_SURFLIVE_1_A + 0x2000U && ((offset - I915_SKL_PLANE_SURFLIVE_1_A) % 0x1000U) == 0U)
		return i915_fake_get(hw, offset - I915_SKL_PLANE_SURFLIVE_1_A + I915_SKL_PLANE_SURF_1_A);

	/* The GMBUS status: a NAK, a byte group ready, or the wait phase. */
	if (offset == I915_SKL_GMBUS2) {
		if (hw->gmbus_nak)
			return I915_SKL_GMBUS_SATOER;
		if (hw->gmbus_left != 0U)
			return I915_SKL_GMBUS_HW_RDY;
		return I915_SKL_GMBUS_HW_WAIT_PHASE;
	}

	/* The GMBUS data: four bytes of the EDID, the first lowest. */
	if (offset == I915_SKL_GMBUS3) {
		value = 0U;
		for (bytes = 0U; bytes < 4U && hw->gmbus_left != 0U; bytes++) {
			value |= (uint32_t)hw->sinks[hw->gmbus_port].edid[hw->gmbus_position % 256U] << (8U * bytes);
			hw->gmbus_position++;
			hw->gmbus_left--;
		}

		/* Succeeded: the bytes. */
		return value;
	}

	/* Succeeded: a stored register. */
	return i915_fake_get(hw, offset);
}

/* Writes a register of the model, with the effects the hardware has. */
static void
i915_fake_write(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_gen9_fake_hw *hw;
	uint32_t requests;
	unsigned port;
	unsigned id;

	/* The model the table reaches. */
	hw = context;

	/* The log, or the count of what it had no room for. */
	if (hw->log_count < I915_GEN9_FAKE_LOG) {
		hw->log_offset[hw->log_count] = offset;
		hw->log_value[hw->log_count] = value;
		hw->log_count++;
	} else {
		hw->log_dropped++;
	}

	/* The power wells follow their requests (each state bit is below its request). */
	if (offset == I915_SKL_PWR_WELL_CTL2) {
		requests = value & 0xaaaaaaaaU;
		i915_fake_set(hw, offset, requests | (requests >> 1));
		return;
	}

	/* A DPLL locks when enabled. */
	for (id = 1U; id <= 3U; id++) {
		if ((id == 1U && offset == I915_SKL_DPLL1_CTL) || (id == 2U && offset == I915_SKL_DPLL2_CTL) || (id == 3U && offset == I915_SKL_DPLL3_CTL)) {
			i915_fake_set(hw, offset, value);
			requests = i915_fake_get(hw, I915_SKL_DPLL_STATUS) & ~I915_SKL_DPLL_LOCK(id);
			if ((value & I915_SKL_DPLL_ENABLE) != 0U)
				requests |= I915_SKL_DPLL_LOCK(id);
			i915_fake_set(hw, I915_SKL_DPLL_STATUS, requests);
			return;
		}
	}

	/* A transcoder's state follows its enable. */
	if (offset >= I915_SKL_TRANSCONF_A && offset <= I915_SKL_TRANSCONF_A + 0x2000U && ((offset - I915_SKL_TRANSCONF_A) % 0x1000U) == 0U) {
		value &= ~I915_SKL_TRANSCONF_STATE;
		if ((value & I915_SKL_TRANSCONF_ENABLE) != 0U)
			value |= I915_SKL_TRANSCONF_STATE;
		i915_fake_set(hw, offset, value);
		return;
	}

	/* A DDI buffer is idle when off. */
	if (offset >= I915_SKL_DDI_BUF_CTL_A && offset <= I915_SKL_DDI_BUF_CTL_A + 0x300U && ((offset - I915_SKL_DDI_BUF_CTL_A) % 0x100U) == 0U) {
		value &= ~I915_SKL_DDI_BUF_IS_IDLE;
		if ((value & I915_SKL_DDI_BUF_ENABLE) == 0U)
			value |= I915_SKL_DDI_BUF_IS_IDLE;
		i915_fake_set(hw, offset, value);
		return;
	}

	/* An AUX control word: a start runs the transfer; else the status bits written as 1 clear. */
	if (offset >= I915_SKL_AUX_CTL_A && offset <= I915_SKL_AUX_CTL_A + 0x300U && ((offset - I915_SKL_AUX_CTL_A) % 0x100U) == 0U) {
		port = (offset - I915_SKL_AUX_CTL_A) / 0x100U;
		if ((value & I915_SKL_AUX_SEND_BUSY) != 0U) {
			i915_fake_aux(hw, port, value);
			return;
		}

		/* A write without the start clears the status bits it has as one. */
		requests = i915_fake_get(hw, offset) & ~(value & (I915_SKL_AUX_DONE | I915_SKL_AUX_TIME_OUT_ERROR | I915_SKL_AUX_RECEIVE_ERROR));
		i915_fake_set(hw, offset, requests);
		return;
	}

	/* The GMBUS: the pin, and the cycles. */
	if (offset == I915_SKL_GMBUS0) {
		hw->gmbus_pin = value & 0x7U;
		i915_fake_set(hw, offset, value);
		return;
	}

	/* The GMBUS's cycles. */
	if (offset == I915_SKL_GMBUS1) {
		i915_fake_gmbus1(hw, value);
		return;
	}

	/* Succeeded: a plain register. */
	i915_fake_set(hw, offset, value);
}

/* Counts a spin. */
static void
i915_fake_udelay(
	void *context,
	unsigned microseconds)
{
	struct i915_gen9_fake_hw *hw;

	/* The model the table reaches. */
	hw = context;

	/* Succeeded: the time is only counted. */
	hw->waited_us += microseconds;
}

/* Runs one AUX transfer: the message from the data registers, the sink's reply into them, the status. */
static void
i915_fake_aux(
	struct i915_gen9_fake_hw *hw,
	unsigned port,
	uint32_t control)
{
	struct i915_gen9_fake_sink *sink;
	uint8_t message[20];
	uint8_t reply[20];
	uint32_t data;
	uint32_t address;
	unsigned size;
	unsigned request;
	unsigned replied;
	unsigned index;

	/* The port's sink and data registers; the transfer counted. */
	sink = &hw->sinks[port];
	data = I915_SKL_AUX_DATA_A + port * 0x100U;
	hw->aux_transfers++;

	/* No sink: a timeout. */
	if (!sink->dp) {
		i915_fake_set(hw, I915_SKL_AUX_CTL_A + port * 0x100U, I915_SKL_AUX_DONE | I915_SKL_AUX_TIME_OUT_ERROR);
		return;
	}

	/* The message. */
	size = (control >> I915_SKL_AUX_MESSAGE_SIZE_SHIFT) & I915_SKL_AUX_MESSAGE_SIZE_MASK;
	for (index = 0U; index < size; index += 4U)
		i915_fake_unpack(i915_fake_get(hw, data + index), message + index, size - index);
	request = message[0] >> 4;
	address = ((uint32_t)(message[0] & 0xfU) << 16) | ((uint32_t)message[1] << 8) | message[2];

	/* A deferral, a native transaction, or an I2C one. */
	memset(reply, 0, sizeof(reply));
	if (sink->defer_left != 0U) {
		sink->defer_left--;
		reply[0] = (uint8_t)(I915_DP_AUX_NATIVE_REPLY_DEFER << 4);
		replied = 1U;
	} else if ((request & 0x8U) != 0U) {
		replied = i915_fake_aux_native(sink, request, address, message, size, reply);
	} else {
		replied = i915_fake_aux_i2c(sink, request, message, size, reply);
	}

	/* The reply and the status. */
	for (index = 0U; index < replied; index += 4U)
		i915_fake_set(hw, data + index, i915_fake_pack(reply + index, replied - index));
	i915_fake_set(hw, I915_SKL_AUX_CTL_A + port * 0x100U, I915_SKL_AUX_DONE | (replied << I915_SKL_AUX_MESSAGE_SIZE_SHIFT));
}

/* Answers a native AUX read or write; reports the reply's bytes. */
static unsigned
i915_fake_aux_native(
	struct i915_gen9_fake_sink *sink,
	unsigned request,
	uint32_t address,
	const uint8_t *message,
	unsigned size,
	uint8_t *reply)
{
	uint8_t status[6];
	unsigned length;
	unsigned index;

	/* The length the header asks for; an ACK. */
	length = (unsigned)message[3] + 1U;
	reply[0] = 0U;

	/* A write: the bytes after the header. */
	if (request == I915_DP_AUX_NATIVE_WRITE) {
		for (index = 0U; index + 4U < size && address + index < sizeof(sink->dpcd); index++)
			sink->dpcd[address + index] = message[4U + index];
		return 1U;
	}

	/* A read: the link status comes from the training model. */
	i915_fake_link_status(sink, status);
	memcpy(sink->dpcd + I915_DPCD_LANE0_1_STATUS, status, sizeof(status));
	for (index = 0U; index < length && address + index < sizeof(sink->dpcd); index++)
		reply[1U + index] = sink->dpcd[address + index];

	/* Succeeded: the ACK and the bytes. */
	return 1U + length;
}

/* Answers an I2C-over-AUX message to the DDC address; reports the reply's bytes. */
static unsigned
i915_fake_aux_i2c(
	struct i915_gen9_fake_sink *sink,
	unsigned request,
	const uint8_t *message,
	unsigned size,
	uint8_t *reply)
{
	unsigned length;
	unsigned index;

	/* An ACK. */
	reply[0] = 0U;

	/* An address-only message just ends or starts the transaction. */
	if (size == 3U)
		return 1U;

	/* A write of the offset. */
	length = (unsigned)message[3] + 1U;
	if ((request & ~I915_DP_AUX_I2C_MOT) == I915_DP_AUX_I2C_WRITE) {
		sink->i2c_offset = message[4];
		return 1U;
	}

	/* A read from the offset on. */
	for (index = 0U; index < length; index++) {
		reply[1U + index] = sink->edid[sink->i2c_offset % 256U];
		sink->i2c_offset++;
	}

	/* Succeeded: the ACK and the bytes. */
	return 1U + length;
}

/* Builds the lanes' status of the training model: clock recovery at the sink's swing, then equalisation at once. */
static void
i915_fake_link_status(
	const struct i915_gen9_fake_sink *sink,
	uint8_t *status)
{
	unsigned pattern;
	unsigned lanes;
	unsigned lane;
	unsigned swing;
	uint8_t nibble;

	/* Nothing done yet; the pattern and the lanes the sink was set to. */
	memset(status, 0, 6U);
	pattern = sink->dpcd[I915_DPCD_TRAINING_PATTERN_SET] & 0x3U;
	lanes = sink->dpcd[I915_DPCD_LANE_COUNT_SET] & 0x1fU;

	/* Each lane: the clock when its swing reaches the sink's, the rest on patterns 2 and 3 or after. */
	for (lane = 0U; lane < lanes && lane < 4U; lane++) {
		nibble = 0U;
		if ((sink->dpcd[I915_DPCD_TRAINING_LANE0_SET + lane] & 0x3U) >= sink->cr_swing)
			nibble = I915_DPCD_LANE_CR_DONE;
		if (nibble != 0U && pattern != 1U)
			nibble |= I915_DPCD_LANE_CHANNEL_EQ_DONE | I915_DPCD_LANE_SYMBOL_LOCKED;
		status[lane / 2U] |= (uint8_t)(nibble << (4U * (lane & 1U)));

		/* The request: the sink's swing (capped at 3), no pre-emphasis. */
		swing = sink->cr_swing;
		if (swing > 3U)
			swing = 3U;
		status[4U + lane / 2U] |= (uint8_t)(swing << (4U * (lane & 1U)));
	}

	/* The lanes are aligned once equalised. */
	if (pattern != 1U && lanes != 0U && (status[0] & I915_DPCD_LANE_CHANNEL_EQ_DONE) != 0U)
		status[2] = I915_DPCD_INTERLANE_ALIGN_DONE;
}

/* Runs a GMBUS1 write: a clear, a stop, or a read with an index. */
static void
i915_fake_gmbus1(
	struct i915_gen9_fake_hw *hw,
	uint32_t value)
{
	int port;

	/* The cycle as written. */
	i915_fake_set(hw, I915_SKL_GMBUS1, value);

	/* The software clear ends a NAK. */
	if ((value & I915_SKL_GMBUS_SW_CLR_INT) != 0U) {
		hw->gmbus_nak = 0;
		hw->gmbus_left = 0U;
		return;
	}

	/* Only a ready read starts anything. */
	if ((value & I915_SKL_GMBUS_SW_RDY) == 0U || (value & I915_SKL_GMBUS_SLAVE_READ) == 0U)
		return;

	/* A pin without a sink NAKs. */
	port = i915_fake_gmbus_port(hw->gmbus_pin);
	if (port < 0 || !hw->sinks[port].hdmi) {
		hw->gmbus_nak = 1;
		return;
	}

	/* Succeeded: the read from the index. */
	hw->gmbus_port = port;
	hw->gmbus_position = (value >> I915_SKL_GMBUS_SLAVE_INDEX_SHIFT) & 0xffU;
	hw->gmbus_left = (value >> I915_SKL_GMBUS_BYTE_COUNT_SHIFT) & 0x1ffU;
}

/* Reports the port of a GMBUS pin, or -1. */
static int
i915_fake_gmbus_port(
	unsigned pin)
{
	/* The three pins. */
	if (pin == I915_SKL_GMBUS_PIN_DPB)
		return (int)I915_SKL_PORT_B;
	if (pin == I915_SKL_GMBUS_PIN_DPC)
		return (int)I915_SKL_PORT_C;
	if (pin == I915_SKL_GMBUS_PIN_DPD)
		return (int)I915_SKL_PORT_D;

	/* Succeeded: no port. */
	return -1;
}


/* Counts a wait of the xd register window. */
static void
i915_fake_xd_wait(
	void *context,
	unsigned microseconds)
{
	/* Succeeded: the same count as a spin. */
	i915_fake_udelay(context, microseconds);
}

/* Writes the writes of the check that ends to the golden record, as the next scene. */
static void
i915_fake_golden_scene(void)
{
	unsigned index;

	/* No record is kept. */
	if (i915_fake_golden == NULL)
		return;

	/* The scene's number, its count of writes and of the writes the log dropped. */
	fprintf(i915_fake_golden, "scene %u writes %u dropped %u\n", i915_fake_golden_scenes, i915_gen9_fake.log_count, i915_gen9_fake.log_dropped);

	/* Each write in order: the register and the value. */
	for (index = 0U; index < i915_gen9_fake.log_count; index++)
		fprintf(i915_fake_golden, "%08x %08x\n", i915_gen9_fake.log_offset[index], i915_gen9_fake.log_value[index]);

	/* Succeeded: the next check is the next scene. */
	i915_fake_golden_scenes++;
}

/* Packs up to four bytes into an AUX data register's word, the first byte highest. */
static uint32_t
i915_fake_pack(
	const uint8_t *bytes,
	unsigned count)
{
	uint32_t word;
	unsigned index;

	/* Succeeded: from bit 31 down. */
	word = 0U;
	for (index = 0U; index < count && index < 4U; index++)
		word |= (uint32_t)bytes[index] << ((3U - index) * 8U);
	return word;
}

/* Unpacks up to four bytes from an AUX data register's word, the first byte highest. */
static void
i915_fake_unpack(
	uint32_t word,
	uint8_t *bytes,
	unsigned count)
{
	unsigned index;

	/* Succeeded: from bit 31 down. */
	for (index = 0U; index < count && index < 4U; index++)
		bytes[index] = (uint8_t)(word >> ((3U - index) * 8U));
}
