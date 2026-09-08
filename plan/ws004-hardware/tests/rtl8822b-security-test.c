/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../../../src/drivers/wifi/rtl8822b/rtl8822b-internal.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_transport {
	uint32_t registers[0x700U / 4U];
	uint64_t now;
	uint64_t expected_deadline;
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
static void test_data_usb_boundaries(void);

static int
fake_read(void *context, uint16_t address, unsigned width, uint32_t *value,
	uint64_t deadline_ticks)
{
	struct fake_transport *fake = context;
	uint32_t full;
	unsigned shift;

	/* Require the security owner to retain its absolute register-transfer deadline. */
	if (fake->expected_deadline != 0U)
		assert(deadline_ticks == fake->expected_deadline);
	if (fake->now >= deadline_ticks)
		return ETIMEDOUT;
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
	assert(drv_rtl8822b_tx_queues_empty(&radio, 100U) == 0);
	assert(fake.reads == 8U);
	fake.registers[0x0234U / 4U] = 0x00210022U;
	assert(drv_rtl8822b_tx_queues_empty(&radio, 100U) == EBUSY);
	/* A later transport error wins over an earlier mismatch: callers must not
	 * mistake an incomplete snapshot for the expected retry state. */
	fake.fail_read_address = 0x023aU;
	assert(drv_rtl8822b_tx_queues_empty(&radio, 100U) == EIO);
	fake.now = 100U;
	assert(drv_rtl8822b_tx_queues_empty(&radio, 100U) == ETIMEDOUT);
}

static int
fake_write(void *context, uint16_t address, unsigned width, uint32_t value,
	uint64_t deadline_ticks)
{
	struct fake_transport *fake = context;
	uint32_t mask;
	unsigned shift = (address & 3U) * 8U;

	/* Check writes and CAM transactions against the original security deadline. */
	if (fake->expected_deadline != 0U)
		assert(deadline_ticks == fake->expected_deadline);
	if (fake->now >= deadline_ticks)
		return ETIMEDOUT;
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
	radio->transport.usb_bulk_max_packet_size = 512U;
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

	/* Keep the supplied deadline across association and every CAM transaction. */
	fake.expected_deadline = 100U;
	assert(drv_rtl8822b_security_enable(&radio, 100U) == 0);
	assert((fake.registers[0x100U / 4U] & 0x200U) != 0U);
	assert((fake.registers[0x680U / 4U] & 0xcfU) == 0xcfU);
	assert(drv_rtl8822b_security_set_association(&radio, bssid, 0x345U,
	    100U) == 0);
	assert(fake.registers[0x618U / 4U] == 0x03020102U);
	assert((fake.registers[0x61cU / 4U] & 0xffffU) == 0x0504U);
	assert((fake.registers[0x6a8U / 4U] & 0x7ffU) == 0x345U);
	assert((fake.registers[0x100U / 4U] & 0x30000U) == 0x20000U);
	assert((fake.registers[0x608U / 4U] & 0x40U) != 0U);
	assert(drv_rtl8822b_cam_program_ccmp(&radio,
	    RTL8822B_CAM_PAIRWISE_SLOT, 0U, 0, bssid, key, 100U) == 0);
	assert(fake.cam_count == 8U);
	assert(fake.cam_address[0] == RTL8822B_CAM_PAIRWISE_SLOT * 8U + 7U);
	assert(fake.cam_address[7] == RTL8822B_CAM_PAIRWISE_SLOT * 8U);
	assert((fake.cam_value[7] & 0x8000U) != 0U);
	assert(((fake.cam_value[7] >> 2) & 7U) == 4U);
	assert(drv_rtl8822b_cam_program_ccmp(&radio, 2U, 2U, 1, broadcast,
	    key, 100U) == 0);
	assert((fake.cam_value[15] & 0x40U) != 0U);
	assert(drv_rtl8822b_cam_clear(&radio, 2U, 100U) == 0);
	assert(fake.cam_value[16] == 0U);
	assert(drv_rtl8822b_cam_stage_ccmp(&radio, 5U, 0U, 0, bssid, key,
	    100U) == 0);
	/* Stage first invalidates word zero, writes only words 7..1, and leaves
	 * the replacement invisible until the explicit activation barrier. */
	assert(fake.cam_count == 25U);
	assert(fake.cam_address[17] == 5U * 8U);
	assert(fake.cam_value[17] == 0U);
	assert(fake.cam_address[24] == 5U * 8U + 1U);
	assert(drv_rtl8822b_cam_activate_ccmp(&radio, 5U, 0U, 0, bssid,
	    100U) == 0);
	assert(fake.cam_address[25] == 5U * 8U);
	assert((fake.cam_value[25] & 0x8000U) != 0U);
	assert(drv_rtl8822b_cam_clear(&radio, 5U, 100U) == 0);
	assert(fake.cam_value[26] == 0U);

	/* Accept a later caller's different absolute deadline without reusing the old one. */
	fake.expected_deadline = 137U;
	assert(drv_rtl8822b_security_clear_association(&radio, 137U) == 0);
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
	assert(drv_rtl8822b_cam_program_ccmp(&radio, 4U, 0U, 0, address, key,
	    100U) == EIO);
	assert(fake.cam_count >= 3U);
	assert(fake.cam_address[fake.cam_count - 1U] == 4U * 8U);
	assert(fake.cam_value[fake.cam_count - 1U] == 0U);
	fake.now = 100U;
	assert(drv_rtl8822b_cam_clear(&radio, 4U, 100U) == ETIMEDOUT);
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
	assert(drv_rtl8822b_data_frame_prepare(&radio, wire, sizeof(wire), frame,
	    sizeof(frame), 1, 7U, 0x321U, &length) == EINVAL);
	radio.power_limits_valid = 1U;
	assert(drv_rtl8822b_data_frame_prepare(&radio, wire, sizeof(wire), frame,
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
	assert(drv_rtl8822b_data_frame_prepare(&radio, wire, 10U, frame,
	    sizeof(frame), 1, 0U, 0U, &length) == ENOSPC);
}

/* Checks payload integrity and short USB packets with and without CCMP. */
static void
test_data_usb_boundaries(
	void)
{
	struct rtl8822b_radio radio;
	struct fake_transport fake;
	uint8_t frame[1536];
	uint8_t wire[1540];
	size_t boundary;
	size_t base;
	size_t frame_length;
	size_t expected;
	size_t length;
	unsigned neighbor;
	unsigned speed;
	unsigned encrypted;
	unsigned index;
	uint16_t checksum;

	/* Retain a recognizable payload behind a valid unicast data header. */
	memset(&fake, 0, sizeof(fake));
	radio_init(&radio, &fake);
	memset(frame, 0x5a, sizeof(frame));
	frame[0] = 0x08U;
	frame[1] = 0x41U;
	frame[4] = 2U;

	/* Exercise every adjacent size around the first three HS boundaries. */
	for (boundary = 512U; boundary <= 1536U; boundary += 512U) {
		/* Include the 1024-byte SuperSpeed boundary in the same matrix. */
		for (neighbor = 0U; neighbor < 3U; neighbor++) {
			base = boundary - 1U + neighbor;
			frame_length = base - 48U;
			expected = base + (neighbor == 1U ? 1U : 0U);

			/* Check the encoder at both transport packet sizes. */
			for (speed = 0U; speed < 2U; speed++) {
				radio.transport.usb_bulk_max_packet_size =
				    speed == 0U ? 512U : 1024U;

				/* Preserve the MAC frame size with either security mode. */
				for (encrypted = 0U; encrypted < 2U; encrypted++) {
					memset(wire, 0xa5, sizeof(wire));
					assert(drv_rtl8822b_data_frame_prepare(&radio, wire,
					    sizeof(wire), frame, frame_length,
					    (int)encrypted, 7U, 0x321U, &length) == 0);
					assert(length == expected);
					assert(length % radio.transport.
					    usb_bulk_max_packet_size != 0U);
					assert((size_t)(wire[0] |
					    ((uint16_t)wire[1] << 8)) ==
					    frame_length);
					assert(((wire[6] >> 6) & 3U) ==
					    (encrypted ? 3U : 0U));
					assert(memcmp(wire + 48U, frame,
					    frame_length) == 0);
					assert(wire[length] == 0xa5U);

					/* Check the one-byte terminator on exact boundaries. */
					if (neighbor == 1U)
						assert(wire[length - 1U] == 0U);

					/* Verify checksum and refuse a buffer missing one byte. */
					checksum = 0U;
					for (index = 0U; index < 16U; index++) {
						checksum ^= (uint16_t)wire[index * 2U] |
						    ((uint16_t)wire[index * 2U + 1U] << 8);
					}
					assert(checksum == 0U);
					assert(drv_rtl8822b_data_frame_prepare(&radio, wire,
					    expected - 1U, frame, frame_length,
					    (int)encrypted, 7U, 0x321U, &length) == ENOSPC);
					assert(length == 0U);
				}
			}
		}
	}
}

int
main(void)
{
	test_association_and_cam();
	test_cam_failure_rollback();
	test_tx_queue_empty_snapshot();
	test_descriptor();
	test_data_usb_boundaries();
	puts("rtl8822b security fixture: PASS");
	return 0;
}
