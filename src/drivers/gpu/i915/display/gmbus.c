/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GMBUS I2C controller of the hotplug path (see gmbus.h).
 *
 * The transfer follows Linux v6.8.12 intel_gmbus.c: one controller serves
 * every DDC pin, a transfer is a sequence of GMBUS1 cycles whose data moves
 * through GMBUS3 four bytes at a time, and GMBUS2 reports ready, wait phase,
 * NAK and activity.  Linux makes every valid pin's bus at display init
 * (intel_gmbus_setup()); here a bus is made on the first request for its
 * pin.  The bit-banging fallback over GPIO is not ported: a transfer that
 * falls back to it is a recorded step answering -EIO.
 *
 * The transfer functions answer Linux errno values (-I915_HPD_ENXIO for a
 * NAK, -I915_HPD_ETIMEDOUT, -I915_HPD_EAGAIN), because the EDID reader of
 * the DP environment compares them.
 *
 * The Linux text this file follows carries this notice:
 *
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2006-2008,2010 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *	Eric Anholt <eric@anholt.net>
 *	Chris Wilson <chris@chris-wilson.co.uk>
 */

#include "hotplug-internal.h"
#include "gmbus.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/lock.h>

#include <stddef.h>

/* The force_bit bit a transfer sets when GMBUS asked to be retried over GPIO. */
#define GMBUS_FORCE_BIT_RETRY (1U << 31)

/* The longest read one burst of a display version 10+ controller moves. */
#define INTEL_GMBUS_BURST_READ_MAX_LEN 767U

/* The length of the adapter name prefix "i915 gmbus ". */
#define I915_GMBUS_NAME_PREFIX_LENGTH 11u

static struct intel_gmbus *i915_to_intel_gmbus(struct i2c_adapter *i2c);
static bool i915_has_gmbus_irq(struct drm_i915_private *i915);
static int i915_gmbus_wait(struct drm_i915_private *i915, u32 status, u32 irq_en);
static int i915_gmbus_wait_idle(struct drm_i915_private *i915);
static unsigned int i915_gmbus_max_xfer_size(struct drm_i915_private *i915);
static int i915_gmbus_xfer_read_chunk(struct drm_i915_private *i915, unsigned short addr, u8 *buf, unsigned int len, u32 gmbus0_reg, u32 gmbus1_index);
static int i915_gmbus_xfer_read(struct drm_i915_private *i915, struct i2c_msg *msg, u32 gmbus0_reg, u32 gmbus1_index);
static int i915_gmbus_xfer_write_chunk(struct drm_i915_private *i915, unsigned short addr, u8 *buf, unsigned int len, u32 gmbus1_index);
static int i915_gmbus_xfer_write(struct drm_i915_private *i915, struct i2c_msg *msg, u32 gmbus1_index);
static bool i915_gmbus_is_index_xfer(struct i2c_msg *msgs, int i, int num);
static int i915_gmbus_index_xfer(struct drm_i915_private *i915, struct i2c_msg *msgs, u32 gmbus0_reg);
static int i915_gmbus_xfer_messages(struct drm_i915_private *i915, struct i2c_msg *msgs, int num, u32 gmbus0_reg, int *index);
static int i915_gmbus_clear_error(struct drm_i915_private *i915, struct i2c_adapter *adapter, struct i2c_msg *msgs, int index);
static int i915_do_gmbus_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num, u32 gmbus0_source);
static int i915_gmbus_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num);
static int i915_hpd_gmbus_xfer_locked(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num);
static int i915_hpd_bit_xfer_step(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

/*
 * The algorithm a GMBUS adapter carries: the transfer under the controller
 * lock.
 *
 * It never changes, so every adapter of every world shares it.
 */
static const struct i2c_algorithm i915_hpd_gmbus_algorithm = {
	i915_hpd_gmbus_xfer_locked,
	NULL
};

/*
 * The bit-banging algorithm over GPIO, which is not ported.
 *
 * A transfer that falls back to it is a recorded step answering -EIO.  It
 * never changes and is shared by every adapter.
 */
const struct i2c_algorithm i2c_bit_algo = {
	i915_hpd_bit_xfer_step,
	NULL
};

/*
 * Resets the controller: no pin selected and no interrupt enabled.
 */
void
i915_hpd_intel_gmbus_reset(
	struct drm_i915_private *i915)
{
	/* Deselects the pin, then disables the controller's interrupts. */
	i915_hpd_intel_de_write(i915, GMBUS0(i915), 0);
	i915_hpd_intel_de_write(i915, GMBUS4(i915), 0);
}

/*
 * Counts one more (or one fewer) request for GPIO bit-banging on a bus.
 */
void
i915_hpd_intel_gmbus_force_bit(
	struct i2c_adapter *adapter,
	bool force_bit)
{
	struct intel_gmbus *bus;
	struct drm_i915_private *i915;

	/* Resolves the bus the adapter is embedded in and its device. */
	bus = i915_to_intel_gmbus(adapter);
	i915 = bus->i915;

	/* Moves the request count under the controller lock. */
	mutex_lock(&i915->display.gmbus.mutex);

	/*
	 * A nonzero count makes every transfer on this bus use bit-banging;
	 * the retry bit a timed-out transfer set stays as it is.
	 */
	if (force_bit) {
		bus->force_bit += 1;
	} else {
		bus->force_bit += -1;
	}

	/* Logs the new count. */
	I915_HPD_DRM_DBG_KMS(&i915->drm, "%sabling bit-banging on %s. force bit now %d\n", force_bit ? "en" : "dis", adapter->name, bus->force_bit);

	mutex_unlock(&i915->display.gmbus.mutex);
}

/*
 * Tells whether a bus transfers over GPIO bit-banging instead of GMBUS.
 */
bool
i915_hpd_intel_gmbus_is_forced_bit(
	struct i2c_adapter *adapter)
{
	struct intel_gmbus *bus;

	/* Resolves the bus the adapter is embedded in. */
	bus = i915_to_intel_gmbus(adapter);

	/* A bus with no bit-banging request uses GMBUS. */
	if (bus->force_bit == 0)
		return false;

	/* Succeeded: the bus is forced onto bit-banging. */
	return true;
}

/*
 * Handles the GMBUS interrupt: wakes the transfer waiting on the controller.
 *
 * The transfer polls GMBUS2 and never sleeps on the wait queue, so the
 * wake-up is only counted in the world.
 */
void
i915_hpd_intel_gmbus_irq_handler(
	struct drm_i915_private *i915)
{
	/* Counts the wake-up of the GMBUS wait queue (the Linux wake_up_all()). */
	drv_i915_hpd_gmbus_woken(i915_hpd_world_of(i915));
}

/*
 * Returns the GMBUS I2C adapter of a DDC pin, making the bus on first use.
 *
 * Follows Linux intel_gmbus_setup() and intel_gmbus_get_adapter() for the
 * ICP pin table; returns NULL for a pin that is not valid there.
 */
struct i2c_adapter *
drv_i915_hpd_gmbus_adapter(
	struct drm_i915_private *i915,
	unsigned int pin)
{
	static const char *const names[GMBUS_NUM_PINS] = {
		NULL, "dpa", "dpb", "dpc", NULL, NULL, NULL, NULL, NULL,
		"tc1", "tc2", "tc3", "tc4", "tc5", "tc6"
	};
	struct i915_hpd_world *world;
	struct intel_gmbus *bus;
	const char *name;
	unsigned position;

	/* Refuses a pin outside the ICP table (the Linux intel_gmbus_is_valid_pin()). */
	if (pin >= GMBUS_NUM_PINS)
		return NULL;
	if (names[pin] == NULL)
		return NULL;

	/* Finds the pin's bus in the world. */
	world = i915_hpd_world_of(i915);
	bus = &world->hpd_gmbus_bus[pin];

	/* A bus already made is handed out as it is. */
	if (bus->i915 != NULL)
		return &bus->adapter;

	/* Prepares the controller lock once for every bus of the world. */
	if (world->hpd_gmbus_mutex_inited == 0) {
		(void)mutex_init(&i915->display.gmbus.mutex, LOCK_RANK_DEVICE, "i915-gmbus");
		world->hpd_gmbus_mutex_inited = 1;
	}

	/* The PCH-split controller sits at the PCH display base. */
	i915->display.gmbus.mmio_base = PCH_DISPLAY_BASE;

	/* Names the adapter "i915 gmbus <pin name>". */
	kern_memset(bus, 0, sizeof(*bus));
	kern_memcpy(bus->adapter.name, "i915 gmbus ", I915_GMBUS_NAME_PREFIX_LENGTH);
	name = names[pin];
	position = I915_GMBUS_NAME_PREFIX_LENGTH;
	while (*name != '\0' && position < sizeof(bus->adapter.name) - 1u) {
		bus->adapter.name[position] = *name;
		name++;
		position++;
	}
	bus->adapter.name[position] = '\0';

	/*
	 * Binds the locked GMBUS transfer, one retry, and the pin at the
	 * conservative 100 kHz rate.  A nonzero i915 marks the bus as made.
	 */
	bus->adapter.algo = &i915_hpd_gmbus_algorithm;
	bus->adapter.retries = 1;
	bus->reg0 = pin | GMBUS_RATE_100KHZ;
	bus->i915 = i915;

	/* Leaves the controller idle before the bus is used. */
	i915_hpd_intel_gmbus_reset(i915);

	/* Succeeded: the pin's bus is made. */
	return &bus->adapter;
}

/*
 * Forgets every bus of the world, so a new hotplug path makes them again.
 */
void
drv_i915_hpd_gmbus_forget(
	struct i915_hpd_world *world)
{
	/* Clears the buses and marks the controller lock as not prepared. */
	kern_memset(world->hpd_gmbus_bus, 0, sizeof(world->hpd_gmbus_bus));
	world->hpd_gmbus_mutex_inited = 0;
}

/* Returns the bus an adapter is embedded in (the Linux to_intel_gmbus()). */
static struct intel_gmbus *
i915_to_intel_gmbus(
	struct i2c_adapter *i2c)
{
	/* The adapter is the first member of the bus. */
	return container_of(i2c, struct intel_gmbus, adapter);
}

/* Tells whether a transfer may use the GMBUS interrupt. */
static bool
i915_has_gmbus_irq(
	struct drm_i915_private *i915)
{
	int version;
	bool enabled;

	/*
	 * encoder->shutdown() may want to use GMBUS after irqs have already
	 * been disabled.
	 */
	version = i915_hpd_display_ver(i915);
	if (version < 4)
		return false;

	/* The interrupt helps only while the device's interrupts are on. */
	enabled = i915_hpd_intel_irqs_enabled(i915);
	if (!enabled)
		return false;

	/* Succeeded: the controller interrupt is usable. */
	return true;
}

/*
 * Waits until GMBUS2 reports one of the status bits or a NAK.
 *
 * Answers 0, -I915_HPD_ENXIO for a NAK, or -I915_HPD_ETIMEDOUT.
 */
static int
i915_gmbus_wait(
	struct drm_i915_private *i915,
	u32 status,
	u32 irq_en)
{
	u32 gmbus2;
	int ret;
	bool irq_usable;

	/*
	 * Important: The hw handles only the first bit, so set only one! Since
	 * we also need to check for NAKs besides the hw ready/idle signal, we
	 * need to wake up periodically and check that ourselves.
	 */
	irq_usable = i915_has_gmbus_irq(i915);
	if (!irq_usable)
		irq_en = 0;

	/*
	 * Enables the one interrupt source; the wait queue is not slept on
	 * (the poll below wakes itself), so nothing joins it.
	 */
	i915_hpd_intel_de_write_fw(i915, GMBUS4(i915), irq_en);

	/* Polls for the status or a NAK: 2 microseconds, then up to 50 ms. */
	status |= GMBUS_SATOER;
	gmbus2 = 0;
	ret = I915_HPD_WAIT_FOR_US((gmbus2 = i915_hpd_intel_de_read_fw(i915, GMBUS2(i915))) & status, 2);
	if (ret != 0)
		ret = I915_HPD_WAIT_FOR((gmbus2 = i915_hpd_intel_de_read_fw(i915, GMBUS2(i915))) & status, 50);

	/* Disables the interrupt again. */
	i915_hpd_intel_de_write_fw(i915, GMBUS4(i915), 0);

	/* A NAK from the slave wins over the wait's own outcome. */
	if ((gmbus2 & GMBUS_SATOER) != 0)
		return -I915_HPD_ENXIO;

	/* Reports a timeout of the poll. */
	if (ret != 0)
		return ret;

	/* Succeeded: GMBUS2 reported the status. */
	return 0;
}

/*
 * Waits until the controller is no longer active.
 *
 * Answers 0 or -I915_HPD_ETIMEDOUT.
 */
static int
i915_gmbus_wait_idle(
	struct drm_i915_private *i915)
{
	u32 irq_enable;
	int ret;
	bool irq_usable;

	/* Important: The hw handles only the first bit, so set only one! */
	irq_enable = 0;
	irq_usable = i915_has_gmbus_irq(i915);
	if (irq_usable)
		irq_enable = GMBUS_IDLE_EN;

	/* Enables the idle interrupt; the wait queue is not slept on. */
	i915_hpd_intel_de_write_fw(i915, GMBUS4(i915), irq_enable);

	/* Polls GMBUS2 until the controller is idle, for up to 10 ms. */
	ret = intel_de_wait_for_register_fw(i915, GMBUS2(i915), GMBUS_ACTIVE, 0, 10);

	/* Disables the interrupt again. */
	i915_hpd_intel_de_write_fw(i915, GMBUS4(i915), 0);

	/* Reports a controller that stayed active. */
	if (ret != 0)
		return ret;

	/* Succeeded: the controller is idle. */
	return 0;
}

/* Returns the longest transfer one GMBUS cycle moves. */
static unsigned int
i915_gmbus_max_xfer_size(
	struct drm_i915_private *i915)
{
	int version;

	/* Display version 9 widened the byte count field. */
	version = i915_hpd_display_ver(i915);
	if (version >= 9)
		return GEN9_GMBUS_BYTE_COUNT_MAX;

	/* Older controllers move at most 256 bytes. */
	return GMBUS_BYTE_COUNT_MAX;
}

/*
 * Reads one chunk from the slave: one GMBUS1 read cycle, then four bytes
 * per GMBUS3 read.
 */
static int
i915_gmbus_xfer_read_chunk(
	struct drm_i915_private *i915,
	unsigned short addr,
	u8 *buf,
	unsigned int len,
	u32 gmbus0_reg,
	u32 gmbus1_index)
{
	unsigned int size;
	unsigned int max_size;
	bool burst_read;
	bool extra_byte_added;
	int ret;
	u32 val;
	u32 loop;

	/* A chunk longer than one cycle moves is a burst read. */
	size = len;
	max_size = i915_gmbus_max_xfer_size(i915);
	burst_read = false;
	if (len > max_size)
		burst_read = true;
	extra_byte_added = false;

	/* Programs the burst: the byte count override in GMBUS0. */
	if (burst_read) {
		/*
		 * As per HW Spec, for 512Bytes need to read extra Byte and
		 * Ignore the extra byte read.
		 */
		if (len == 512) {
			extra_byte_added = true;
			len++;
		}
		size = len % 256 + 256;
		i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), gmbus0_reg | GMBUS_BYTE_CNT_OVERRIDE);
	}

	/* Starts the read cycle with the index, the size and the slave address. */
	i915_hpd_intel_de_write_fw(i915, GMBUS1(i915), gmbus1_index | GMBUS_CYCLE_WAIT | (size << GMBUS_BYTE_COUNT_SHIFT) | (addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_READ | GMBUS_SW_RDY);

	/* Collects the bytes four at a time as the controller has them ready. */
	while (len != 0) {
		loop = 0;

		/* Waits until GMBUS3 holds the next bytes. */
		ret = i915_gmbus_wait(i915, GMBUS_HW_RDY, GMBUS_HW_RDY_EN);
		if (ret != 0)
			return ret;

		/* Stores up to four bytes, dropping the extra byte of a 512-byte burst. */
		val = i915_hpd_intel_de_read_fw(i915, GMBUS3(i915));
		for (;;) {
			if (extra_byte_added && len == 1) {
				len--;
				break;
			}

			/* Stores the lowest byte and moves the next one down. */
			*buf = (u8)(val & 0xff);
			buf++;
			val >>= 8;

			/* Stops after the last byte or after four bytes of this read. */
			len--;
			if (len == 0)
				break;
			loop++;
			if (loop >= 4)
				break;
		}

		/* Resets the override bit once four bytes of the burst remain. */
		if (burst_read && len == size - 4)
			i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), gmbus0_reg);
	}

	/* Succeeded: the chunk has been read. */
	return 0;
}

/* Reads one message from the slave, chunk by chunk. */
static int
i915_gmbus_xfer_read(
	struct drm_i915_private *i915,
	struct i2c_msg *msg,
	u32 gmbus0_reg,
	u32 gmbus1_index)
{
	u8 *buf;
	unsigned int rx_size;
	unsigned int len;
	unsigned int max_size;
	int version;
	int ret;

	/* Reads the message in chunks of what one burst or one cycle moves. */
	buf = msg->buf;
	rx_size = msg->len;
	do {
		version = i915_hpd_display_ver(i915);
		if (version >= 10) {
			/* HAS_GMBUS_BURST_READ: a burst moves up to 767 bytes. */
			len = min(rx_size, INTEL_GMBUS_BURST_READ_MAX_LEN);
		} else {
			/* Without bursts one cycle is the limit. */
			max_size = i915_gmbus_max_xfer_size(i915);
			len = min(rx_size, max_size);
		}

		/* Reads the chunk. */
		ret = i915_gmbus_xfer_read_chunk(i915, msg->addr, buf, len, gmbus0_reg, gmbus1_index);
		if (ret != 0)
			return ret;

		/* Moves past the bytes read. */
		rx_size -= len;
		buf += len;
	} while (rx_size != 0);

	/* Succeeded: the whole message has been read. */
	return 0;
}

/*
 * Writes one chunk to the slave: the first four bytes are loaded into
 * GMBUS3 before the cycle starts, the rest four at a time as it runs.
 */
static int
i915_gmbus_xfer_write_chunk(
	struct drm_i915_private *i915,
	unsigned short addr,
	u8 *buf,
	unsigned int len,
	u32 gmbus1_index)
{
	unsigned int chunk_size;
	u32 val;
	u32 loop;
	int ret;

	/* Packs the first four bytes into one GMBUS3 value. */
	chunk_size = len;
	val = 0;
	loop = 0;
	while (len != 0 && loop < 4) {
		val |= (u32)*buf << (8 * loop);
		buf++;
		loop++;
		len -= 1;
	}

	/* Loads the first bytes and starts the write cycle. */
	i915_hpd_intel_de_write_fw(i915, GMBUS3(i915), val);
	i915_hpd_intel_de_write_fw(i915, GMBUS1(i915), gmbus1_index | GMBUS_CYCLE_WAIT | (chunk_size << GMBUS_BYTE_COUNT_SHIFT) | (addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_WRITE | GMBUS_SW_RDY);

	/* Feeds the remaining bytes four at a time. */
	while (len != 0) {
		/* Packs up to four more bytes. */
		val = 0;
		loop = 0;
		for (;;) {
			val |= (u32)*buf << (8 * loop);
			buf++;

			/* Stops after the last byte or after four bytes of this value. */
			len--;
			if (len == 0)
				break;
			loop++;
			if (loop >= 4)
				break;
		}

		/* Hands them to the controller. */
		i915_hpd_intel_de_write_fw(i915, GMBUS3(i915), val);

		/* Waits until the controller took them. */
		ret = i915_gmbus_wait(i915, GMBUS_HW_RDY, GMBUS_HW_RDY_EN);
		if (ret != 0)
			return ret;
	}

	/* Succeeded: the chunk has been written. */
	return 0;
}

/* Writes one message to the slave, chunk by chunk. */
static int
i915_gmbus_xfer_write(
	struct drm_i915_private *i915,
	struct i2c_msg *msg,
	u32 gmbus1_index)
{
	u8 *buf;
	unsigned int tx_size;
	unsigned int len;
	unsigned int max_size;
	int ret;

	/* Writes the message in chunks of what one cycle moves. */
	buf = msg->buf;
	tx_size = msg->len;
	do {
		max_size = i915_gmbus_max_xfer_size(i915);
		len = min(tx_size, max_size);

		/* Writes the chunk. */
		ret = i915_gmbus_xfer_write_chunk(i915, msg->addr, buf, len, gmbus1_index);
		if (ret != 0)
			return ret;

		/* Moves past the bytes written. */
		buf += len;
		tx_size -= len;
	} while (tx_size != 0);

	/* Succeeded: the whole message has been written. */
	return 0;
}

/*
 * Tells whether message i and the next one form one INDEX cycle.
 *
 * The gmbus controller can combine a 1 or 2 byte write with another
 * read/write that immediately follows it by using an "INDEX" cycle.
 */
static bool
i915_gmbus_is_index_xfer(
	struct i2c_msg *msgs,
	int i,
	int num)
{
	/* There must be a following message. */
	if (i + 1 >= num)
		return false;

	/* Both messages must address the same slave. */
	if (msgs[i].addr != msgs[i + 1].addr)
		return false;

	/* The first message must be a write. */
	if ((msgs[i].flags & I2C_M_RD) != 0)
		return false;

	/* The first message must be a one or two byte offset. */
	if (msgs[i].len != 1 && msgs[i].len != 2)
		return false;

	/* The following message must move data. */
	if (msgs[i + 1].len == 0)
		return false;

	/* Succeeded: the pair is one INDEX transfer. */
	return true;
}

/*
 * Transfers an offset write and the message after it as one INDEX cycle.
 */
static int
i915_gmbus_index_xfer(
	struct drm_i915_private *i915,
	struct i2c_msg *msgs,
	u32 gmbus0_reg)
{
	u32 gmbus1_index;
	u32 gmbus5;
	int ret;

	/* A two-byte offset goes to GMBUS5, a one-byte offset into GMBUS1. */
	gmbus1_index = 0;
	gmbus5 = 0;
	if (msgs[0].len == 2)
		gmbus5 = GMBUS_2BYTE_INDEX_EN | msgs[0].buf[1] | (msgs[0].buf[0] << 8);
	if (msgs[0].len == 1)
		gmbus1_index = GMBUS_CYCLE_INDEX | (msgs[0].buf[0] << GMBUS_SLAVE_INDEX_SHIFT);

	/* GMBUS5 holds 16-bit index */
	if (gmbus5 != 0)
		i915_hpd_intel_de_write_fw(i915, GMBUS5(i915), gmbus5);

	/* Transfers the data message with the offset. */
	if ((msgs[1].flags & I2C_M_RD) != 0) {
		ret = i915_gmbus_xfer_read(i915, &msgs[1], gmbus0_reg, gmbus1_index);
	} else {
		ret = i915_gmbus_xfer_write(i915, &msgs[1], gmbus1_index);
	}

	/* Clear GMBUS5 after each index transfer */
	if (gmbus5 != 0)
		i915_hpd_intel_de_write_fw(i915, GMBUS5(i915), 0);

	/* Reports a failed data transfer. */
	if (ret != 0)
		return ret;

	/* Succeeded: both messages have been transferred. */
	return 0;
}

/*
 * Selects the pin and transfers the messages from *index on, waiting for
 * the wait phase after each.
 *
 * On failure *index names the message that failed.  Answers 0 or the
 * failed step's Linux errno.
 */
static int
i915_gmbus_xfer_messages(
	struct drm_i915_private *i915,
	struct i2c_msg *msgs,
	int num,
	u32 gmbus0_reg,
	int *index)
{
	int i;
	int inc;
	int ret;
	bool index_xfer;

	/* Selects the pin and the rate. */
	i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), gmbus0_reg);

	/* Transfers every message, pairing an offset write with what follows. */
	ret = 0;
	for (i = *index; i < num; i += inc) {
		inc = 1;
		index_xfer = i915_gmbus_is_index_xfer(msgs, i, num);
		if (index_xfer) {
			/* An index transmission is two msgs. */
			ret = i915_gmbus_index_xfer(i915, &msgs[i], gmbus0_reg);
			inc = 2;
		} else if ((msgs[i].flags & I2C_M_RD) != 0) {
			ret = i915_gmbus_xfer_read(i915, &msgs[i], gmbus0_reg, 0);
		} else {
			ret = i915_gmbus_xfer_write(i915, &msgs[i], 0);
		}

		/* Waits for the wait phase that ends the message. */
		if (ret == 0)
			ret = i915_gmbus_wait(i915, GMBUS_HW_WAIT_PHASE, GMBUS_HW_WAIT_EN);

		/* Stops at the failed message. */
		if (ret != 0) {
			*index = i;
			return ret;
		}
	}

	/* Succeeded: every message went through; the index passed the last one. */
	*index = i;
	return 0;
}

/*
 * Recovers the controller from a NAK and reports it.
 *
 * Answers -I915_HPD_ENXIO, or -I915_HPD_ETIMEDOUT when the bus did not go
 * idle.
 */
static int
i915_gmbus_clear_error(
	struct drm_i915_private *i915,
	struct i2c_adapter *adapter,
	struct i2c_msg *msgs,
	int index)
{
	int ret;
	int idle;

	/*
	 * Wait for bus to IDLE before clearing NAK.
	 * If we clear the NAK while bus is still active, then it will stay
	 * active and the next transaction may fail.
	 *
	 * If no ACK is received during the address phase of a transaction, the
	 * adapter must report -ENXIO. It is not clear what to return if no ACK
	 * is received at other times. But we have to be careful to not return
	 * spurious -ENXIO because that will prevent i2c and drm edid functions
	 * from retrying. So return -ENXIO only when gmbus properly quiescents -
	 * timing out seems to happen when there _is_ a ddc chip present, but
	 * it's slow responding and only answers on the 2nd retry.
	 */
	ret = -I915_HPD_ENXIO;
	idle = i915_gmbus_wait_idle(i915);
	if (idle != 0) {
		I915_HPD_DRM_DBG_KMS(&i915->drm, "GMBUS [%s] timed out after NAK\n", adapter->name);
		ret = -I915_HPD_ETIMEDOUT;
	}

	/*
	 * Toggle the Software Clear Interrupt bit. This has the effect
	 * of resetting the GMBUS controller and so clearing the
	 * BUS_ERROR raised by the slave's NAK.
	 */
	i915_hpd_intel_de_write_fw(i915, GMBUS1(i915), GMBUS_SW_CLR_INT);
	i915_hpd_intel_de_write_fw(i915, GMBUS1(i915), 0);
	i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), 0);

	/* Logs the message the slave refused. */
	I915_HPD_DRM_DBG_KMS(&i915->drm, "GMBUS [%s] NAK for addr: %04x %c(%d)\n", adapter->name, msgs[index].addr, (msgs[index].flags & I2C_M_RD) ? 'r' : 'w', msgs[index].len);

	/* Reports the NAK, or the bus that stayed busy after it. */
	return ret;
}

/*
 * Transfers the messages over GMBUS (the Linux do_gmbus_xfer()).
 *
 * Answers the number of messages transferred, or a Linux errno:
 * -I915_HPD_ENXIO for a NAK, -I915_HPD_ETIMEDOUT for a bus that did not go
 * idle, -I915_HPD_EAGAIN to have the transfer retried over bit-banging.
 */
static int
i915_do_gmbus_xfer(
	struct i2c_adapter *adapter,
	struct i2c_msg *msgs,
	int num,
	u32 gmbus0_source)
{
	struct intel_gmbus *bus;
	struct drm_i915_private *i915;
	int i;
	int try;
	int ret;
	int idle;

	/* Resolves the bus and its device. */
	bus = i915_to_intel_gmbus(adapter);
	i915 = bus->i915;
	i = 0;
	try = 0;

	/* Display WA #0868: skl,bxt,kbl,cfl,glk */
	if (IS_GEMINILAKE(i915) || IS_BROXTON(i915)) {
		i915_bxt_gmbus_clock_gating(i915, false);
	} else if (HAS_PCH_SPT(i915) || HAS_PCH_CNP(i915)) {
		i915_pch_gmbus_clock_gating(i915, false);
	}

	/* Transfers the messages, retrying once when the first one was NAKed. */
	for (;;) {
		ret = i915_gmbus_xfer_messages(i915, msgs, num, gmbus0_source | bus->reg0, &i);
		if (ret == 0) {
			/*
			 * Generate a STOP condition on the bus. Note that gmbus can't
			 * generata a STOP on the very first cycle. To simplify the code
			 * we unconditionally generate the STOP condition with an
			 * additional gmbus cycle.
			 */
			i915_hpd_intel_de_write_fw(i915, GMBUS1(i915), GMBUS_CYCLE_STOP | GMBUS_SW_RDY);

			/*
			 * Mark the GMBUS interface as disabled after waiting for idle.
			 * We will re-enable it at the start of the next xfer,
			 * till then let it sleep.
			 */
			idle = i915_gmbus_wait_idle(i915);
			if (idle != 0) {
				I915_HPD_DRM_DBG_KMS(&i915->drm, "GMBUS [%s] timed out waiting for idle\n", adapter->name);
				ret = -I915_HPD_ETIMEDOUT;
			}
			i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), 0);

			/* Answers the number of messages, unless the bus stayed busy. */
			if (ret == 0)
				ret = i;
			break;
		}

		/* A stuck bus falls back to bit-banging. */
		if (ret == -I915_HPD_ETIMEDOUT) {
			I915_HPD_DRM_DBG_KMS(&i915->drm, "GMBUS [%s] timed out, falling back to bit banging on pin %d\n", bus->adapter.name, bus->reg0 & 0xff);
			i915_hpd_intel_de_write_fw(i915, GMBUS0(i915), 0);

			/*
			 * Hardware may not support GMBUS over these pins? Try GPIO
			 * bitbanging instead. Use EAGAIN to have i2c core retry.
			 */
			ret = -I915_HPD_EAGAIN;
			break;
		}

		/* Clears the NAK and learns whether the bus went idle after it. */
		ret = i915_gmbus_clear_error(i915, adapter, msgs, i);

		/*
		 * Passive adapters sometimes NAK the first probe. Retry the first
		 * message once on -ENXIO for GMBUS transfers; the bit banging
		 * algorithm has retries internally. See also the retry loop in
		 * drm_do_probe_ddc_edid, which bails out on the first -ENXIO.
		 */
		if (ret == -I915_HPD_ENXIO && i == 0) {
			if (try == 0) {
				try++;
				I915_HPD_DRM_DBG_KMS(&i915->drm, "GMBUS [%s] NAK on first message, retry\n", adapter->name);
				continue;
			}
			try++;
		}
		break;
	}

	/* Display WA #0868: skl,bxt,kbl,cfl,glk */
	if (IS_GEMINILAKE(i915) || IS_BROXTON(i915)) {
		i915_bxt_gmbus_clock_gating(i915, true);
	} else if (HAS_PCH_SPT(i915) || HAS_PCH_CNP(i915)) {
		i915_pch_gmbus_clock_gating(i915, true);
	}

	/* Reports a failed transfer. */
	if (ret < 0)
		return ret;

	/* Succeeded: reports the number of messages transferred. */
	return ret;
}

/*
 * Transfers the messages with a GMBUS power reference, over GMBUS or, when
 * forced, over bit-banging.
 */
static int
i915_gmbus_xfer(
	struct i2c_adapter *adapter,
	struct i2c_msg *msgs,
	int num)
{
	struct intel_gmbus *bus;
	struct drm_i915_private *i915;
	intel_wakeref_t wakeref;
	int ret;

	/* Resolves the bus and its device. */
	bus = i915_to_intel_gmbus(adapter);
	i915 = bus->i915;

	/* Holds the GMBUS power domain across the transfer. */
	wakeref = drv_i915_hpd_intel_display_power_get(i915, POWER_DOMAIN_GMBUS);

	/*
	 * A forced bus uses bit-banging, and a failure there clears the
	 * retry request; a GMBUS transfer that asks to be retried sets it.
	 */
	if (bus->force_bit != 0) {
		ret = i2c_bit_algo.master_xfer(adapter, msgs, num);
		if (ret < 0)
			bus->force_bit &= ~GMBUS_FORCE_BIT_RETRY;
	} else {
		ret = i915_do_gmbus_xfer(adapter, msgs, num, 0);
		if (ret == -I915_HPD_EAGAIN)
			bus->force_bit |= GMBUS_FORCE_BIT_RETRY;
	}

	/* Drops the power reference. */
	drv_i915_hpd_intel_display_power_put(i915, POWER_DOMAIN_GMBUS, wakeref);

	/* Reports a failed transfer. */
	if (ret < 0)
		return ret;

	/* Succeeded: reports the number of messages transferred. */
	return ret;
}

/*
 * Transfers under the controller lock (the Linux gmbus_lock_bus()).
 *
 * The single GMBUS controller is shared by every pin; the lock also keeps
 * the hotplug work's detection and a test thread's read apart.
 */
static int
i915_hpd_gmbus_xfer_locked(
	struct i2c_adapter *adapter,
	struct i2c_msg *msgs,
	int num)
{
	struct intel_gmbus *bus;
	int ret;

	/* Resolves the bus whose device holds the controller lock. */
	bus = i915_to_intel_gmbus(adapter);

	/* Runs the transfer alone on the controller. */
	mutex_lock(&bus->i915->display.gmbus.mutex);

	ret = i915_gmbus_xfer(adapter, msgs, num);

	mutex_unlock(&bus->i915->display.gmbus.mutex);

	/* Reports a failed transfer. */
	if (ret < 0)
		return ret;

	/* Succeeded: reports the number of messages transferred. */
	return ret;
}

/* Records a transfer that fell back to the unported GPIO bit-banging. */
static int
i915_hpd_bit_xfer_step(
	struct i2c_adapter *adap,
	struct i2c_msg *msgs,
	int num)
{
	UNUSED_PARAMETER(msgs);
	UNUSED_PARAMETER(num);

	/* Names the step and answers the I/O error the fallback stands for. */
	kern_logf("i915: hpd step i2c_bit_algo.master_xfer (GPIO bit-banging not ported): %s -> -EIO\n", adap->name);
	return -I915_HPD_EIO;
}
