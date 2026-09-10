/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/usb-uas.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct drv_usb_uas_command
begin(enum drv_usb_uas_direction direction)
{
	struct drv_usb_uas_command command;
	unsigned char wire[32];
	unsigned char cdb[10] = { 0x28 };
	unsigned i;

	memset(&command, 0, sizeof(command));
	memset(wire, 0xff, sizeof(wire));
	assert(drv_usb_uas_command_begin(&command, 0x1234, 255, cdb,
	    sizeof(cdb), direction, direction == DRV_USB_UAS_NO_DATA ? 0 : 512,
	    wire) == 0);
	for (i = 0; i < sizeof(wire); i++) {
		unsigned expected = 0;
		if (i == 0) expected = 1;
		if (i == 2) expected = 0x12;
		if (i == 3) expected = 0x34;
		if (i == 9) expected = 255;
		if (i == 16) expected = 0x28;
		assert(wire[i] == expected);
	}
	assert(drv_usb_uas_command_begin(&command, 1, 0, cdb, sizeof(cdb),
	    direction, command.expected, wire) == EBUSY);
	return command;
}

int
main(void)
{
	struct drv_usb_uas_command command;
	unsigned char ready[4] = { 6, 0, 0x12, 0x34 };
	unsigned char sense[34] = { 3, 0, 0x12, 0x34 };
	unsigned char wire[32];
	unsigned char cdb[16] = { 0 };
	unsigned n;
	int direction;

	for (direction = DRV_USB_UAS_READ; direction <= DRV_USB_UAS_WRITE; direction++) {
		command = begin(direction);
		ready[0] = direction == DRV_USB_UAS_READ ? 6 : 7;
		assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
		assert(command.state == DRV_USB_UAS_DATA);
		assert(drv_usb_uas_command_data(&command, 512) == 0);
		assert(drv_usb_uas_command_status(&command, sense, 16) == 0);
		assert(command.state == DRV_USB_UAS_COMPLETE);
		assert(command.transferred == 512);
		assert(drv_usb_uas_command_status(&command, sense, 16) == EIO);
	}
	command = begin(DRV_USB_UAS_NO_DATA);
	assert(drv_usb_uas_command_status(&command, sense, 16) == 0);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, sense, 16) == EIO);

	/* Every truncated or overlong Sense IU fails before completion publication. */
	for (n = 0; n <= sizeof(sense); n++) {
		command = begin(DRV_USB_UAS_NO_DATA);
		if (n == 16) continue;
		assert(drv_usb_uas_command_status(&command, sense, n) == EIO);
		assert(command.state == DRV_USB_UAS_FAILED);
	}
	/* Unknown IUs, task-management responses and malformed READY never grant data. */
	for (n = 0; n < 256; n++) {
		command = begin(DRV_USB_UAS_READ);
		ready[0] = n;
		if (n == 6) continue;
		assert(drv_usb_uas_command_status(&command, ready, 4) == EIO);
	}
	ready[0] = 6;
	for (n = 0; n < 4; n++) {
		command = begin(DRV_USB_UAS_READ);
		assert(drv_usb_uas_command_status(&command, ready, n) == EIO);
	}
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, NULL, 4) == EIO);
	ready[0] = 6;
	command = begin(DRV_USB_UAS_WRITE);
	assert(drv_usb_uas_command_status(&command, ready, 4) == EIO);
	command = begin(DRV_USB_UAS_NO_DATA);
	assert(drv_usb_uas_command_status(&command, ready, 4) == EIO);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
	assert(drv_usb_uas_command_status(&command, sense, 16) == EIO);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_data(&command, 512) == EIO);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
	assert(drv_usb_uas_command_data(&command, 513) == EIO);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
	assert(drv_usb_uas_command_data(&command, 511) == 0);
	assert(drv_usb_uas_command_status(&command, ready, 4) == EIO);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
	assert(drv_usb_uas_command_data(&command, 511) == 0);
	assert(drv_usb_uas_command_status(&command, sense, 16) == 0);
	assert(command.transferred == 511);

	/* CHECK CONDITION can reject a CDB early or explain a short transfer. */
	sense[6] = 2;
	sense[15] = 18;
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, sense, 34) == 0);
	assert(command.scsi_status == 2);
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, ready, 4) == 0);
	assert(drv_usb_uas_command_data(&command, 1) == 0);
	assert(drv_usb_uas_command_status(&command, sense, 34) == 0);
	for (n = 0; n < 256; n++) {
		command = begin(DRV_USB_UAS_READ);
		sense[3] = n;
		if (n == 0x34) continue;
		assert(drv_usb_uas_command_status(&command, sense, 34) == EIO);
	}
	sense[3] = 0x34;
	sense[4] = 1;
	command = begin(DRV_USB_UAS_READ);
	assert(drv_usb_uas_command_status(&command, sense, 34) == EIO);
	command = begin(DRV_USB_UAS_READ);
	drv_usb_uas_command_fail(&command);
	assert(drv_usb_uas_command_status(&command, ready, 4) == EIO);
	assert(drv_usb_uas_command_data(&command, 0) == EIO);
	memset(&command, 0, sizeof(command));
	assert(drv_usb_uas_command_begin(&command, 0, 0, cdb, 16,
	    DRV_USB_UAS_NO_DATA, 0, wire) == EINVAL);
	assert(drv_usb_uas_command_begin(&command, 1, 256, cdb, 16,
	    DRV_USB_UAS_NO_DATA, 0, wire) == EINVAL);
	assert(drv_usb_uas_command_begin(&command, 1, 0, cdb, 17,
	    DRV_USB_UAS_NO_DATA, 0, wire) == EINVAL);
	assert(drv_usb_uas_command_begin(&command, 1, 0, cdb, 16,
	    DRV_USB_UAS_READ, 0, wire) == EINVAL);
	assert(command.state == DRV_USB_UAS_IDLE);
	puts("UAS command protocol: PASS");
	return 0;
}
