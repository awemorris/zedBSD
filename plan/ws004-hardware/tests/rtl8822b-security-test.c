/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../../../src/drivers/rtl8822b-internal.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_transport {
	uint32_t registers[0x700U / 4U];
	uint64_t now;
	uint16_t fail_read_address;
	unsigned reads;
	unsigned writes;
	unsigned fail_write;
	uint32_t cam_address[128];
	uint32_t cam_value[128];
	unsigned cam_count;
	uint32_t staged_value;
};

static void radio_init(struct rtl8822b_radio *, struct fake_transport *);

static int
fake_read(void *context, uint16_t address, unsigned width, uint32_t *value)
{
	struct fake_transport *fake = context;
	uint32_t full;
	unsigned shift;

	fake->reads++;
	if (fake->fail_read_address == address) {
		fake->fail_read_address = 0U;
		return EIO;
	}
	assert(address < sizeof(fake->registers));
	full = fake->registers[address / 4U];
	shift = (address & 3U) * 8U;
	if (width == 1U)
		*value = (full >> shift) & 0xffU;
	else if (width == 2U)
		*value = (full >> shift) & 0xffffU;
	else {
		assert(width == 4U && shift == 0U);
		*value = full;
	}
	if (address == 0x0670U && (*value & 0x80000000U) != 0U) {
		fake->registers[address / 4U] &= ~0x80000000U;
		*value &= ~0x80000000U;
	}
	return 0;
}

static void
test_tx_queue_empty_snapshot(void)
{
	struct rtl8822b_radio radio;
	struct fake_transport fake;

	memset(&fake, 0, sizeof(fake));
	radio_init(&radio, &fake);
	/* Each 32-bit fixture word contains the adjacent 16-bit reserved and
	 * available counters from the 8822B priority-queue register pair. */
	fake.registers[0x0230U / 4U] = 0x00110011U;
	fake.registers[0x0234U / 4U] = 0x00220022U;
	fake.registers[0x0238U / 4U] = 0x00330033U;
	fake.registers[0x023cU / 4U] = 0x00440044U;
	assert(rtl8822b_tx_queues_empty(&radio, 100U) == 0);
	assert(fake.reads == 8U);
	fake.registers[0x0234U / 4U] = 0x00210022U;
	assert(rtl8822b_tx_queues_empty(&radio, 100U) == EBUSY);
	/* A later transport error wins over an earlier mismatch: callers must not
	 * mistake an incomplete snapshot for the expected retry state. */
	fake.fail_read_address = 0x023aU;
	assert(rtl8822b_tx_queues_empty(&radio, 100U) == EIO);
	fake.now = 100U;
	assert(rtl8822b_tx_queues_empty(&radio, 100U) == ETIMEDOUT);
}

static int
fake_write(void *context, uint16_t address, unsigned width, uint32_t value)
{
	struct fake_transport *fake = context;
	uint32_t mask;
	unsigned shift = (address & 3U) * 8U;

	fake->writes++;
	if (fake->fail_write != 0U && fake->writes == fake->fail_write)
		return EIO;
	assert(address < sizeof(fake->registers));
	if (width == 4U) {
		assert(shift == 0U);
		fake->registers[address / 4U] = value;
	} else {
		mask = width == 1U ? 0xffU : 0xffffU;
		fake->registers[address / 4U] &= ~(mask << shift);
		fake->registers[address / 4U] |= (value & mask) << shift;
	}
	if (address == 0x0674U)
		fake->staged_value = value;
	if (address == 0x0670U && fake->cam_count < 128U) {
		fake->cam_address[fake->cam_count] = value & 0xffU;
		fake->cam_value[fake->cam_count] = fake->staged_value;
		fake->cam_count++;
	}
	return 0;
}

static uint64_t
fake_now(void *context)
{
	return ((struct fake_transport *)context)->now;
}

static void
fake_yield(void *context)
{
	((struct fake_transport *)context)->now++;
}

static void
radio_init(struct rtl8822b_radio *radio, struct fake_transport *fake)
{
	memset(radio, 0, sizeof(*radio));
	radio->state = RTL8822B_RADIO_STARTED;
	radio->channel = 1U;
	radio->power_limits_valid = 1U;
	radio->transport.context = fake;
	radio->transport.read = fake_read;
	radio->transport.write = fake_write;
	radio->transport.now_ticks = fake_now;
	radio->transport.yield = fake_yield;
}

static void
test_association_and_cam(void)
{
	static const uint8_t bssid[6] = { 0x02U, 1U, 2U, 3U, 4U, 5U };
	static const uint8_t broadcast[6] = {
	    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	uint8_t key[16];
	struct rtl8822b_radio radio;
	struct fake_transport fake;
	unsigned index;

	memset(&fake, 0, sizeof(fake));
	radio_init(&radio, &fake);
	for (index = 0U; index < sizeof(key); index++)
		key[index] = (uint8_t)(index + 1U);
	assert(rtl8822b_security_enable(&radio, 100U) == 0);
	assert((fake.registers[0x100U / 4U] & 0x200U) != 0U);
	assert((fake.registers[0x680U / 4U] & 0xcfU) == 0xcfU);
	assert(rtl8822b_security_set_association(&radio, bssid, 0x345U,
	    100U) == 0);
	assert(fake.registers[0x618U / 4U] == 0x03020102U);
	assert((fake.registers[0x61cU / 4U] & 0xffffU) == 0x0504U);
	assert((fake.registers[0x6a8U / 4U] & 0x7ffU) == 0x345U);
	assert((fake.registers[0x100U / 4U] & 0x30000U) == 0x20000U);
	assert((fake.registers[0x608U / 4U] & 0x40U) != 0U);
	assert(rtl8822b_cam_program_ccmp(&radio,
	    RTL8822B_CAM_PAIRWISE_SLOT, 0U, 0, bssid, key, 100U) == 0);
	assert(fake.cam_count == 8U);
	assert(fake.cam_address[0] == RTL8822B_CAM_PAIRWISE_SLOT * 8U + 7U);
	assert(fake.cam_address[7] == RTL8822B_CAM_PAIRWISE_SLOT * 8U);
	assert((fake.cam_value[7] & 0x8000U) != 0U);
	assert(((fake.cam_value[7] >> 2) & 7U) == 4U);
	assert(rtl8822b_cam_program_ccmp(&radio, 2U, 2U, 1, broadcast,
	    key, 100U) == 0);
	assert((fake.cam_value[15] & 0x40U) != 0U);
	assert(rtl8822b_cam_clear(&radio, 2U, 100U) == 0);
	assert(fake.cam_value[16] == 0U);
	assert(rtl8822b_security_clear_association(&radio, 100U) == 0);
	assert((fake.registers[0x608U / 4U] & 0x40U) == 0U);
	assert((fake.registers[0x100U / 4U] & 0x30000U) == 0U);
}

static void
test_cam_failure_rollback(void)
{
	uint8_t key[16] = { 0 };
	uint8_t address[6] = { 2U, 1U, 2U, 3U, 4U, 5U };
	struct rtl8822b_radio radio;
	struct fake_transport fake;

	memset(&fake, 0, sizeof(fake));
	radio_init(&radio, &fake);
	/* Two register writes per word.  Fail the fourth word's data write. */
	fake.fail_write = 7U;
	assert(rtl8822b_cam_program_ccmp(&radio, 4U, 0U, 0, address, key,
	    100U) == EIO);
	assert(fake.cam_count >= 3U);
	assert(fake.cam_address[fake.cam_count - 1U] == 4U * 8U);
	assert(fake.cam_value[fake.cam_count - 1U] == 0U);
	fake.now = 100U;
	assert(rtl8822b_cam_clear(&radio, 4U, 100U) == ETIMEDOUT);
}

static void
test_descriptor(void)
{
	uint8_t frame[128], wire[256];
	struct rtl8822b_radio radio;
	struct fake_transport fake;
	size_t length;
	uint16_t checksum = 0U;
	unsigned index;

	memset(&fake, 0, sizeof(fake));
	radio_init(&radio, &fake);
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x08U;
	frame[1] = 0x41U;
	frame[4] = 2U;
	radio.power_limits_valid = 0U;
	assert(rtl8822b_data_frame_prepare(&radio, wire, sizeof(wire), frame,
	    sizeof(frame), 1, 7U, 0x321U, &length) == EINVAL);
	radio.power_limits_valid = 1U;
	assert(rtl8822b_data_frame_prepare(&radio, wire, sizeof(wire), frame,
	    sizeof(frame), 1, 7U, 0x321U, &length) == 0);
	assert(length == 48U + sizeof(frame));
	assert(((wire[6] >> 6) & 3U) == 3U);
	assert((wire[10] & 0x08U) != 0U);
	assert((wire[24] | ((uint16_t)wire[25] << 8)) == 0x321U);
	for (index = 0U; index < 16U; index++)
		checksum ^= (uint16_t)wire[index * 2U] |
		    ((uint16_t)wire[index * 2U + 1U] << 8);
	assert(checksum == 0U);
	assert(memcmp(wire + 48U, frame, sizeof(frame)) == 0);
	assert(rtl8822b_data_frame_prepare(&radio, wire, 10U, frame,
	    sizeof(frame), 1, 0U, 0U, &length) == ENOSPC);
}

int
main(void)
{
	test_association_and_cam();
	test_cam_failure_rollback();
	test_tx_queue_empty_snapshot();
	test_descriptor();
	puts("rtl8822b security fixture: PASS");
	return 0;
}
