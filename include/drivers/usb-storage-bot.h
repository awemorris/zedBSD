/*
 * USB Mass Storage Bulk-Only Transport response helpers
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_USB_STORAGE_BOT_H
#define ZEDBSD_DRIVERS_USB_STORAGE_BOT_H

#include <stddef.h>
#include <stdint.h>

enum drv_usb_bot_csw_result {
	DRV_USB_BOT_CSW_GOOD = 0,
	DRV_USB_BOT_CSW_COMMAND_FAILED,
	DRV_USB_BOT_CSW_PHASE_ERROR,
	DRV_USB_BOT_CSW_INVALID
};

static inline enum drv_usb_bot_csw_result
drv_usb_bot_classify_csw_status(uint8_t status)
{
	switch (status) {
	case 0:
		return DRV_USB_BOT_CSW_GOOD;
	case 1:
		return DRV_USB_BOT_CSW_COMMAND_FAILED;
	case 2:
		return DRV_USB_BOT_CSW_PHASE_ERROR;
	default:
		return DRV_USB_BOT_CSW_INVALID;
	}
}

static inline int
drv_usb_bot_csw_requests_sense(enum drv_usb_bot_csw_result result)
{
	return result == DRV_USB_BOT_CSW_COMMAND_FAILED;
}

/*
 * dCSWDataResidue describes bytes the device did not process.  For an IN
 * transfer it must agree with the USB short-transfer length.  For OUT, the
 * host can have sent every byte even when the device processed fewer bytes;
 * callers therefore receive the processed length and enforce command-specific
 * exactness.
 */
static inline int
drv_usb_bot_processed_length(size_t requested, size_t usb_actual,
	uint32_t residue, int input, size_t *processed)
{
	size_t completed;

	if ((uint64_t)residue > requested)
		return 0;
	completed = requested - residue;
	if ((input != 0 && usb_actual != completed) ||
	    (input == 0 && usb_actual != requested))
		return 0;
	if (processed != NULL)
		*processed = completed;
	return 1;
}

#endif
