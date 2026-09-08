/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD RTL8822B security and station data path
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "rtl8822b-internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define RTL8822B_REG_CR 0x0100U
#define RTL8822B_REG_RCR 0x0608U
#define RTL8822B_REG_PORT0_BSSID 0x0618U
#define RTL8822B_REG_PORT0_AID 0x06a8U
#define RTL8822B_REG_SEC_COMMAND 0x0670U
#define RTL8822B_REG_SEC_WRITE 0x0674U
#define RTL8822B_REG_SEC_CONFIG 0x0680U
#define RTL8822B_REG_FIFO_PAGE_HIGH 0x0230U
#define RTL8822B_REG_FIFO_PAGE_LOW 0x0234U
#define RTL8822B_REG_FIFO_PAGE_NORMAL 0x0238U
#define RTL8822B_REG_FIFO_PAGE_EXTRA 0x023cU

#define RTL8822B_CR_SECURITY_ENABLE 0x0200U
#define RTL8822B_CR_NET_TYPE_MASK 0x00030000U
#define RTL8822B_CR_NET_TYPE_LINKED 0x00020000U
#define RTL8822B_RCR_CHECK_BSSID_DATA 0x0040U
#define RTL8822B_SECURITY_PROFILE 0x00cfU
#define RTL8822B_CAM_WRITE_ENABLE 0x00010000U
#define RTL8822B_CAM_POLLING 0x80000000U
#define RTL8822B_CAM_ENTRY_SHIFT 3U
#define RTL8822B_CAM_AES 4U
#define RTL8822B_CAM_VALID 0x00008000U

static int reg_update(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t mask, uint32_t value, uint64_t deadline);
static int reg_read(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t *value, uint64_t deadline);
static int deadline_valid(const struct rtl8822b_radio *radio, uint64_t deadline);
static int reg_write(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t value, uint64_t deadline);
static int cam_ccmp_arguments_valid(const struct rtl8822b_radio *radio, uint8_t slot, uint8_t key_index, int group, const uint8_t address[6]);
static uint32_t cam_ccmp_word0(uint8_t key_index, int group, const uint8_t address[6]);
static uint32_t load_le32(const uint8_t *bytes);
static int cam_write_word(struct rtl8822b_radio *radio, uint8_t slot, uint8_t word, uint32_t value, uint64_t deadline);
static int cam_wait(struct rtl8822b_radio *radio, uint64_t deadline);
static void secret_erase(void *buffer, size_t length);
static void store_le32(uint8_t *bytes, uint32_t value);
static uint16_t load_le16(const uint8_t *bytes);
static void store_le16(uint8_t *bytes, uint16_t value);

/*
 * Implements the drv rtl8822b security enable operation.
 */
int
drv_rtl8822b_security_enable(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	error = reg_update(radio, RTL8822B_REG_CR, 2U,
			   RTL8822B_CR_SECURITY_ENABLE,
			   RTL8822B_CR_SECURITY_ENABLE, deadline_ticks);

	/* Checks the operation status. */
	if (error == 0) {
		error = reg_update(radio, RTL8822B_REG_SEC_CONFIG, 2U,
				   RTL8822B_SECURITY_PROFILE,
				   RTL8822B_SECURITY_PROFILE, deadline_ticks);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv rtl8822b tx queues empty operation.
 */
int
drv_rtl8822b_tx_queues_empty(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	static const uint16_t queue_registers[] = {
		RTL8822B_REG_FIFO_PAGE_HIGH, RTL8822B_REG_FIFO_PAGE_LOW,
		RTL8822B_REG_FIFO_PAGE_NORMAL, RTL8822B_REG_FIFO_PAGE_EXTRA};
	uint32_t reserved, available;
	unsigned index;
	int busy = 0;
	int error;

	/*
 * rtw8822b uses the 16-bit reserved/available counter pair for every
	 * priority queue.  Read the complete snapshot even after finding a busy
	 * queue so a deadline or transport failure is never hidden as EBUSY. */
	/* Process each remaining element. */
	for (index = 0U;
	     index < sizeof(queue_registers) / sizeof(queue_registers[0]);
	     index++) {
		error = reg_read(radio, queue_registers[index], 2U, &reserved,
				 deadline_ticks);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		error = reg_read(radio, queue_registers[index] + 2U, 2U,
				 &available, deadline_ticks);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Handles the reserved condition. */
		if (reserved != available)
			busy = 1;
	}

	/* Returns the computed result. */
	return busy ? EBUSY : 0;
}

/*
 * Implements the drv rtl8822b security set association operation.
 */
int
drv_rtl8822b_security_set_association(
	struct rtl8822b_radio *radio,
	const uint8_t bssid[6],
	uint16_t aid,
	uint64_t deadline_ticks)
{
	uint32_t low, high;
	int error;

	/* Handles the bssid availability. */
	if (bssid == NULL || (bssid[0] & 1U) != 0U || aid == 0U ||
	    aid > 0x07ffU)

		/* Returns the computed result. */
		return EINVAL;
	low = (uint32_t)bssid[0] | ((uint32_t)bssid[1] << 8) |
	      ((uint32_t)bssid[2] << 16) | ((uint32_t)bssid[3] << 24);
	high = (uint32_t)bssid[4] | ((uint32_t)bssid[5] << 8);
	error = reg_write(radio, RTL8822B_REG_PORT0_BSSID, 4U, low,
			  deadline_ticks);

	/* Checks the operation status. */
	if (error == 0) {
		error = reg_write(radio, RTL8822B_REG_PORT0_BSSID + 4U, 2U,
				  high, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = reg_update(radio, RTL8822B_REG_PORT0_AID, 2U, 0x07ffU,
				   aid, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = reg_update(radio, RTL8822B_REG_CR, 4U,
				   RTL8822B_CR_NET_TYPE_MASK,
				   RTL8822B_CR_NET_TYPE_LINKED, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = reg_update(radio, RTL8822B_REG_RCR, 4U,
				   RTL8822B_RCR_CHECK_BSSID_DATA,
				   RTL8822B_RCR_CHECK_BSSID_DATA,
				   deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0) {
		(void)drv_rtl8822b_security_clear_association(radio,
							      deadline_ticks);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv rtl8822b security clear association operation.
 */
int
drv_rtl8822b_security_clear_association(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int first = 0;
	int error;

	error = reg_update(radio, RTL8822B_REG_RCR, 4U,
			   RTL8822B_RCR_CHECK_BSSID_DATA, 0U, deadline_ticks);

	/* Checks the operation status. */
	if (error != 0)
		first = error;
	error = reg_update(radio, RTL8822B_REG_CR, 4U,
			   RTL8822B_CR_NET_TYPE_MASK, 0U, deadline_ticks);

	/* Checks the operation status. */
	if (first == 0 && error != 0)
		first = error;
	error = reg_update(radio, RTL8822B_REG_PORT0_AID, 2U, 0x07ffU, 0U,
			   deadline_ticks);

	/* Checks the operation status. */
	if (first == 0 && error != 0)
		first = error;
	error = reg_write(radio, RTL8822B_REG_PORT0_BSSID, 4U, 0U,
			  deadline_ticks);

	/* Checks the operation status. */
	if (first == 0 && error != 0)
		first = error;
	error = reg_write(radio, RTL8822B_REG_PORT0_BSSID + 4U, 2U, 0U,
			  deadline_ticks);

	/* Checks the operation status. */
	if (first == 0 && error != 0)
		first = error;

	/* Returns the computed result. */
	return first;
}

/*
 * Implements the drv rtl8822b cam program ccmp operation.
 */
int
drv_rtl8822b_cam_program_ccmp(
	struct rtl8822b_radio *radio,
	uint8_t slot,
	uint8_t key_index,
	int group,
	const uint8_t address[6],
	const uint8_t key[16],
	uint64_t deadline_ticks)
{
	uint32_t words[8];
	int index;
	int error;

	/* Checks the cam ccmp arguments valid result. */
	if (!cam_ccmp_arguments_valid(radio, slot, key_index, group, address) ||
	    key == NULL)

		/* Returns the computed result. */
		return EINVAL;
	memset(words, 0, sizeof(words));
	words[0] = cam_ccmp_word0(key_index, group, address);
	words[1] = (uint32_t)address[2] | ((uint32_t)address[3] << 8) |
		   ((uint32_t)address[4] << 16) | ((uint32_t)address[5] << 24);
	/* Process each remaining element. */
	for (index = 0; index < 4; index++)
		words[2 + index] = load_le32(key + (size_t)index * 4U);

	/*
 * Program the valid word last.  A partial entry therefore cannot match.
	 */
	/* Process each remaining element. */
	for (index = 7; index >= 1; index--) {
		error = cam_write_word(radio, slot, (uint8_t)index,
				       words[index], deadline_ticks);

		/* Checks the operation status. */
		if (error != 0)
			goto rollback;
	}
	error = cam_write_word(radio, slot, 0U, words[0], deadline_ticks);

	/* Checks the operation status. */
	if (error == 0)
		goto out;
rollback:
	(void)cam_write_word(radio, slot, 0U, 0U, deadline_ticks);
out:
	secret_erase(words, sizeof(words));

	/* Returns the computed result. */
	return error;
}

/*
 * Stage a replacement without making it visible to the CAM lookup engine. Word zero contains both the address prefix and VALID bit, so it is cleared before any secret material is written and deliberately remains clear on success.  The owning driver closes TX admission before calling this helper and later performs the fail-closed old-clear/new-activate transition.
 */
int
drv_rtl8822b_cam_stage_ccmp(
	struct rtl8822b_radio *radio,
	uint8_t slot,
	uint8_t key_index,
	int group,
	const uint8_t address[6],
	const uint8_t key[16],
	uint64_t deadline_ticks)
{
	uint32_t words[8];
	int index;
	int error;

	/* Checks the cam ccmp arguments valid result. */
	if (!cam_ccmp_arguments_valid(radio, slot, key_index, group, address) ||
	    key == NULL)

		/* Returns the computed result. */
		return EINVAL;
	memset(words, 0, sizeof(words));
	words[1] = (uint32_t)address[2] | ((uint32_t)address[3] << 8) |
		   ((uint32_t)address[4] << 16) | ((uint32_t)address[5] << 24);
	/* Process each remaining element. */
	for (index = 0; index < 4; index++)
		words[2 + index] = load_le32(key + (size_t)index * 4U);
	error = cam_write_word(radio, slot, 0U, 0U, deadline_ticks);
	/* Process each remaining element. */
	for (index = 7; error == 0 && index >= 1; index--) {
		error = cam_write_word(radio, slot, (uint8_t)index,
				       words[index], deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0)
		(void)cam_write_word(radio, slot, 0U, 0U, deadline_ticks);
	secret_erase(words, sizeof(words));

	/* Returns the computed result. */
	return error;
}

/*
 * Publish an already staged entry.  Callers must first clear every live CAM entry that can match the same traffic; this final word-zero write is the sole transition from inert secret storage to a hardware-visible key.
 */
int
drv_rtl8822b_cam_activate_ccmp(
	struct rtl8822b_radio *radio,
	uint8_t slot,
	uint8_t key_index,
	int group,
	const uint8_t address[6],
	uint64_t deadline_ticks)
{
	int function_result;

	/* Checks the cam ccmp arguments valid result. */
	if (!cam_ccmp_arguments_valid(radio, slot, key_index, group, address))
		return EINVAL;

	/* Obtains the cam write word result. */
	function_result = cam_write_word(
		radio, slot, 0U, cam_ccmp_word0(key_index, group, address),
		deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv rtl8822b cam clear operation.
 */
int
drv_rtl8822b_cam_clear(
	struct rtl8822b_radio *radio,
	uint8_t slot,
	uint64_t deadline_ticks)
{
	int function_result;

	/* Handles the radio availability. */
	if (radio == NULL || slot >= RTL8822B_CAM_ENTRY_COUNT)
		return EINVAL;

	/* Obtains the cam write word result. */
	function_result = cam_write_word(radio, slot, 0U, 0U, deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv rtl8822b data frame prepare operation.
 */
int
drv_rtl8822b_data_frame_prepare(
	const struct rtl8822b_radio *radio,
	uint8_t *wire,
	size_t capacity,
	const uint8_t *frame,
	size_t frame_length,
	int encrypted,
	uint8_t mac_id,
	uint16_t cookie,
	size_t *wire_length)
{
	size_t total;
	uint32_t word0, word1, word2, word3, word4, word6, word8;
	uint16_t checksum = 0U;
	unsigned index;

	/* Handles the wire length availability. */
	if (wire_length == NULL)
		return EINVAL;
	*wire_length = 0U;
	/* Handles the radio availability. */
	if (radio == NULL || radio->state != RTL8822B_RADIO_STARTED ||
	    radio->power_limits_valid == 0U || wire == NULL || frame == NULL ||
	    frame_length < 24U || frame_length > RTL8822B_DATA_MPDU_MAX ||
	    mac_id > 127U || (encrypted != 0 && encrypted != 1) ||
	    cookie > 0x0fffU)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the frame length condition. */
	if (frame_length > SIZE_MAX - RTL8822B_DATA_TX_DESCRIPTOR_SIZE)
		return EOVERFLOW;
	total = RTL8822B_DATA_TX_DESCRIPTOR_SIZE + frame_length;

	/* Avoid full packets on both 512-byte HS and 1024-byte SS pipes. */
	if (total % 512U == 0U) {
		/* Handles the total condition. */
		if (total == SIZE_MAX)
			return EOVERFLOW;
		total++;
	}

	/* Handles the capacity condition. */
	if (capacity < total)
		return ENOSPC;
	memset(wire, 0, total);
	word0 = (uint32_t)frame_length |
		((uint32_t)RTL8822B_DATA_TX_DESCRIPTOR_SIZE << 16) |
		(1U << 26) | (1U << 31);

	/* Handles the frame condition. */
	if ((frame[4U] & 1U) != 0U)
		word0 |= 1U << 24;

	/*
 * QSEL 0 (implicit) is the non-QoS best-effort queue.  RATE_ID 6 is the
	 * rtw88 default for non-HT data; CCMP is hardware security type 3. */
	word1 = mac_id | (6U << 16) | ((encrypted ? 3U : 0U) << 22);
	word2 = 1U << 19;
	word3 = (1U << 8) | (1U << 10);
	word4 = 4U; /* 6 Mbps legacy OFDM, no HT/VHT dependency. */
	word6 = cookie & 0x0fffU;
	word8 = 1U << 15;
	store_le32(wire, word0);
	store_le32(wire + 4U, word1);
	store_le32(wire + 8U, word2);
	store_le32(wire + 12U, word3);
	store_le32(wire + 16U, word4);
	store_le32(wire + 24U, word6);
	store_le32(wire + 32U, word8);
	/* Process each remaining element. */
	for (index = 0U; index < 16U; index++)
		checksum ^= load_le16(wire + index * 2U);
	store_le16(wire + 28U, checksum);
	memcpy(wire + RTL8822B_DATA_TX_DESCRIPTOR_SIZE, frame, frame_length);
	*wire_length = total;
	/* Reports successful completion. */
	return 0;
}

/* Supports the reg update operation. */
static int
reg_update(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t mask,
	uint32_t value,
	uint64_t deadline)
{
	int function_result;
	uint32_t current;
	int error;

	error = reg_read(radio, address, width, &current, deadline);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Obtains the reg write result. */
	function_result =
		reg_write(radio, address, width,
			  (current & ~mask) | (value & mask), deadline);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the reg read operation. */
static int
reg_read(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t *value,
	uint64_t deadline)
{
	int function_result;

	/* Checks the deadline valid result. */
	if (!deadline_valid(radio, deadline))
		return ETIMEDOUT;

	/* Handles the read availability. */
	if (radio->state != RTL8822B_RADIO_STARTED ||
	    radio->transport.read == NULL || value == NULL)

		/* Returns the computed result. */
		return ENETDOWN;

	/* Computes the function result. */
	function_result = radio->transport.read(
		radio->transport.context, address, width, value, deadline);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the deadline valid operation. */
static int
deadline_valid(
	const struct rtl8822b_radio *radio,
	uint64_t deadline)
{
	int function_result;

	/* Computes the function result. */
	function_result =
		radio != NULL && radio->transport.now_ticks != NULL &&
		radio->transport.now_ticks(radio->transport.context) < deadline;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the reg write operation. */
static int
reg_write(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t value,
	uint64_t deadline)
{
	int function_result;

	/* Checks the deadline valid result. */
	if (!deadline_valid(radio, deadline))
		return ETIMEDOUT;

	/* Handles the write availability. */
	if (radio->state != RTL8822B_RADIO_STARTED ||
	    radio->transport.write == NULL)

		/* Returns the computed result. */
		return ENETDOWN;

	/* Computes the function result. */
	function_result = radio->transport.write(
		radio->transport.context, address, width, value, deadline);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cam ccmp arguments valid operation. */
static int
cam_ccmp_arguments_valid(
	const struct rtl8822b_radio *radio,
	uint8_t slot,
	uint8_t key_index,
	int group,
	const uint8_t address[6])
{
	/* Returns the computed result. */
	return radio != NULL && address != NULL &&
	       slot < RTL8822B_CAM_ENTRY_COUNT && key_index <= 3U &&
	       (group == 0 || group == 1);
}

/* Supports the cam ccmp word0 operation. */
static uint32_t
cam_ccmp_word0(
	uint8_t key_index,
	int group,
	const uint8_t address[6])
{
	/* Returns the computed result. */
	return key_index | (RTL8822B_CAM_AES << 2) | ((uint32_t)group << 6) |
	       RTL8822B_CAM_VALID | ((uint32_t)address[0] << 16) |
	       ((uint32_t)address[1] << 24);
}

/* Supports the load le32 operation. */
static uint32_t
load_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Supports the cam write word operation. */
static int
cam_write_word(
	struct rtl8822b_radio *radio,
	uint8_t slot,
	uint8_t word,
	uint32_t value,
	uint64_t deadline)
{
	int function_result;
	uint32_t address;
	int error;

	error = reg_write(radio, RTL8822B_REG_SEC_WRITE, 4U, value, deadline);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	address = ((uint32_t)slot << RTL8822B_CAM_ENTRY_SHIFT) | word;
	error = reg_write(radio, RTL8822B_REG_SEC_COMMAND, 4U,
			  RTL8822B_CAM_WRITE_ENABLE | RTL8822B_CAM_POLLING |
				  address,
			  deadline);

	/* Computes the function result. */
	function_result = error == 0 ? cam_wait(radio, deadline) : error;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cam wait operation. */
static int
cam_wait(
	struct rtl8822b_radio *radio,
	uint64_t deadline)
{
	uint32_t command;
	int error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = reg_read(radio, RTL8822B_REG_SEC_COMMAND, 4U, &command,
				 deadline);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Handles the command condition. */
		if ((command & RTL8822B_CAM_POLLING) == 0U)
			return 0;

		/* Checks the deadline valid result. */
		if (!deadline_valid(radio, deadline))
			return ETIMEDOUT;

		/* Handles the yield availability. */
		if (radio->transport.yield != NULL)
			radio->transport.yield(radio->transport.context);
	}
}

/* Supports the secret erase operation. */
static void
secret_erase(
	void *buffer,
	size_t length)
{
	volatile uint8_t *bytes = buffer;

	/* Process each remaining element. */
	while (length-- != 0U)
		*bytes++ = 0U;
}

/* Supports the store le32 operation. */
static void
store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Supports the load le16 operation. */
static uint16_t
load_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Supports the store le16 operation. */
static void
store_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}
