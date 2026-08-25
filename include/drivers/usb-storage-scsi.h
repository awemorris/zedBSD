/*
 * USB storage SCSI response helpers
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_USB_STORAGE_SCSI_H
#define ZEDBSD_DRIVERS_USB_STORAGE_SCSI_H

#include <stddef.h>
#include <stdint.h>

struct drv_usb_scsi_sense {
	uint8_t key;
	uint8_t asc;
	uint8_t ascq;
	uint8_t valid;
};

static inline int
drv_usb_scsi_parse_sense(const void *buffer, size_t length,
			 struct drv_usb_scsi_sense *sense)
{
	const uint8_t *bytes = buffer;
	uint8_t response;

	if (bytes == NULL || sense == NULL || length == 0)
		return 0;
	sense->valid = 0;
	response = bytes[0] & 0x7fU;
	if ((response == 0x70U || response == 0x71U) && length >= 14U) {
		sense->key = bytes[2] & 0x0fU;
		sense->asc = bytes[12];
		sense->ascq = bytes[13];
	} else if ((response == 0x72U || response == 0x73U) && length >= 4U) {
		sense->key = bytes[1] & 0x0fU;
		sense->asc = bytes[2];
		sense->ascq = bytes[3];
	} else {
		return 0;
	}
	sense->valid = 1;
	return 1;
}

static inline int
drv_usb_scsi_mode_sense6_read_only(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;

	return bytes != NULL && length >= 3U && (bytes[2] & 0x80U) != 0;
}

#endif
