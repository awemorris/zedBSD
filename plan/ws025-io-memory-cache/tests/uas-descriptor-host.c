/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/usb-uas.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Exact configuration replies from q144's installed QEMU control captures. */
static const char high_hex[] =
    "09023e00010104c0000904000004080662000705010200020004240100"
    "070582020002000424020007058302000200042403000705040200020004240400";
static const char super_hex[] =
    "09025600010105c0000904000004080662000705010200040006300f00000004240100"
    "0705820200040006300f040000042402000705830200040006300f04000004240300"
    "0705040200040006300f04000004240400";
static unsigned checks;

static size_t
decode_hex(const char *hex, unsigned char *bytes)
{
	size_t index;
	unsigned value;

	for (index = 0; hex[index * 2] != '\0'; index++) {
		assert(sscanf(hex + index * 2, "%2x", &value) == 1);
		bytes[index] = value;
	}
	return index;
}

static void
set_length(unsigned char *bytes, size_t length)
{
	bytes[2] = (unsigned char)length;
	bytes[3] = (unsigned char)(length >> 8);
}

static int
probe(const unsigned char *bytes, size_t length,
    enum drv_usb_uas_profile profile, unsigned interface, unsigned alternate,
    int expected)
{
	struct drv_usb_uas_capabilities caps;
	struct drv_usb_uas_capabilities empty;
	unsigned index;
	int error;

	memset(&caps, 0xff, sizeof(caps));
	memset(&empty, 0, sizeof(empty));
	error = drv_usb_uas_decode_configuration(bytes, length, interface,
	    alternate, profile, &caps);
	if (expected >= 0)
		assert(error == expected);
	if (error != 0) {
		assert(memcmp(&caps, &empty, sizeof(caps)) == 0);
	} else {
		assert(caps.interface_number == interface);
		assert(caps.alternate_setting == alternate);
		for (index = 0; index < 4; index++) {
			assert((caps.pipes[index].address & 0x80U) ==
			    ((index == 1 || index == 2) ? 0x80U : 0));
			assert(caps.pipes[index].maximum_packet ==
			    (profile == DRV_USB_UAS_SUPER_SPEED ? 1024 : 512));
		}
	}
	checks++;
	return error;
}

static void
reject_byte(const unsigned char *source, size_t length,
    enum drv_usb_uas_profile profile, unsigned offset, unsigned char value)
{
	unsigned char bytes[512];

	memcpy(bytes, source, length);
	bytes[offset] = value;
	probe(bytes, length, profile, 0, 0, EINVAL);
}

int
main(void)
{
	unsigned char high[512];
	unsigned char super[512];
	unsigned char bytes[512];
	struct drv_usb_uas_capabilities caps;
	size_t high_size;
	size_t super_size;
	size_t length;
	size_t offset;
	unsigned index;
	unsigned round;
	unsigned random;
	unsigned char addresses[4] = {5, 0x86, 0x87, 8};

	high_size = decode_hex(high_hex, high);
	super_size = decode_hex(super_hex, super);
	assert(high_size == 62 && super_size == 86);
	probe(high, high_size, DRV_USB_UAS_HIGH_SPEED, 0, 0, 0);
	probe(super, super_size, DRV_USB_UAS_SUPER_SPEED, 0, 0, 0);
	assert(drv_usb_uas_decode_configuration(super, super_size, 0, 0,
	    DRV_USB_UAS_SUPER_SPEED, &caps) == 0);
	assert(caps.minimum_stream_exponent == 4);
	for (index = 0; index < 4; index++) {
		assert(caps.pipes[index].maximum_burst == 15);
		assert(caps.pipes[index].stream_exponent == (index == 0 ? 0 : 4));
	}
	probe(NULL, 9, DRV_USB_UAS_HIGH_SPEED, 0, 0, EINVAL);
	probe(high, high_size, (enum drv_usb_uas_profile)99, 0, 0, EOPNOTSUPP);
	probe(high, high_size, DRV_USB_UAS_HIGH_SPEED, 256, 0, EINVAL);
	probe(high, high_size, DRV_USB_UAS_HIGH_SPEED, 0, 256, EINVAL);
	probe(high, high_size, DRV_USB_UAS_HIGH_SPEED, 1, 0, ENOENT);
	probe(high, high_size, DRV_USB_UAS_HIGH_SPEED, 0, 1, ENOENT);
	probe(high, high_size, DRV_USB_UAS_SUPER_SPEED, 0, 0, EINVAL);
	probe(super, super_size, DRV_USB_UAS_HIGH_SPEED, 0, 0, EINVAL);

	/* Every truncation fails, even if its outer total length is rewritten. */
	for (length = 0; length < super_size; length++) {
		assert(probe(super, length, DRV_USB_UAS_SUPER_SPEED, 0, 0, -1) != 0);
		memcpy(bytes, super, super_size);
		set_length(bytes, length);
		assert(probe(bytes, length, DRV_USB_UAS_SUPER_SPEED, 0, 0, -1) != 0);
	}

	/* Framing, identity, missing pipe, direction and duplicate-address errors. */
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 0, 8);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 18, 0);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 18, 255);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 13, 3);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 16, 0x50);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 20, 0);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 20, 0x11);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 20, 0x81);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 21, 3);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 26, 0x25);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 27, 5);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 28, 1);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 38, 1);
	reject_byte(high, high_size, DRV_USB_UAS_HIGH_SPEED, 53, 1);

	/* Companion requirements and command/status stream separation. */
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 26, 0x31);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 27, 16);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 28, 4);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 29, 1);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 45, 0);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 45, 17);
	reject_byte(super, super_size, DRV_USB_UAS_SUPER_SPEED, 45, 0x84);

	/* Pipe roles remain correct after physical order and addresses change. */
	memcpy(bytes, high, high_size);
	for (index = 0; index < 4; index++) {
		memcpy(bytes + 18 + (3 - index) * 11, high + 18 + index * 11, 11);
		bytes[20 + (3 - index) * 11] = addresses[index];
	}
	probe(bytes, high_size, DRV_USB_UAS_HIGH_SPEED, 0, 0, 0);
	assert(drv_usb_uas_decode_configuration(bytes, high_size, 0, 0,
	    DRV_USB_UAS_HIGH_SPEED, &caps) == 0);
	for (index = 0; index < 4; index++)
		assert(caps.pipes[index].address == addresses[index]);

	/* Distinct alternates do not borrow one another's endpoints. */
	memcpy(bytes, high, high_size);
	memcpy(bytes + high_size, high + 9, high_size - 9);
	length = high_size * 2 - 9;
	set_length(bytes, length);
	probe(bytes, length, DRV_USB_UAS_HIGH_SPEED, 0, 0, EINVAL);
	bytes[high_size + 3] = 1;
	probe(bytes, length, DRV_USB_UAS_HIGH_SPEED, 0, 0, 0);
	probe(bytes, length, DRV_USB_UAS_HIGH_SPEED, 0, 1, 0);
	bytes[13] = 0;
	probe(bytes, length, DRV_USB_UAS_HIGH_SPEED, 0, 0, EINVAL);
	probe(bytes, length, DRV_USB_UAS_HIGH_SPEED, 0, 1, 0);

	/* Deterministic malformed variants exercise bounds under sanitizers. */
	random = 7;
	for (round = 0; round < 10000; round++) {
		memcpy(bytes, super, super_size);
		random = random * 1664525U + 1013904223U;
		offset = random % super_size;
		random = random * 1664525U + 1013904223U;
		bytes[offset] ^= (unsigned char)((random >> 24) | 1U);
		probe(bytes, super_size, DRV_USB_UAS_SUPER_SPEED, 0, 0, -1);
	}
	printf("UAS descriptor production parser: PASS %u checks\n", checks);
	return 0;
}
