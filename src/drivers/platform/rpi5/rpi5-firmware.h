/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 5 firmware property interface.
 *
 * The VideoCore firmware answers requests the ARM side posts through the
 * property channel of the BCM2835 mailbox.  On the Pi 5 the kernel asks it
 * about and changes the clocks it owns (the GPU's, the HEVC decoder's, the
 * compositor's and the HDMI ports'), and asks it about the displays it set
 * up.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_FIRMWARE_H
#define KERN_DRIVERS_PLATFORM_RPI5_FIRMWARE_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/* The most value words one tag of drv_rpi5_firmware_property() carries. */
#define DRV_RPI5_FIRMWARE_MAX_VALUES	8U

/* The largest message drv_rpi5_firmware_message() sends, in bytes. */
#define DRV_RPI5_FIRMWARE_MESSAGE_BYTES	4096U

/*
 * The words of a property message before its first tag's values: the
 * message's size in bytes, its request or response code, and the first
 * tag's code, value buffer size in bytes and request or response code.
 */
#define DRV_RPI5_FIRMWARE_WORD_SIZE	0U
#define DRV_RPI5_FIRMWARE_WORD_CODE	1U
#define DRV_RPI5_FIRMWARE_WORD_TAG	2U
#define DRV_RPI5_FIRMWARE_WORD_CAPACITY	3U
#define DRV_RPI5_FIRMWARE_WORD_TAG_CODE	4U
#define DRV_RPI5_FIRMWARE_WORD_VALUES	5U

/* The response code of a message the firmware handled. */
#define DRV_RPI5_FIRMWARE_SUCCESS	0x80000000U

/*
 * Finds the mailbox in the device tree and prepares the request buffer.
 * Returns an errno; a call after a successful one does nothing.  Callers on
 * different CPUs may race: one prepares and the others wait for it, and a
 * failed preparation is retried by the next call.  Thread context only.
 */
int drv_rpi5_firmware_init(const struct drv_fdt *fdt);

/*
 * Sends one property tag and waits for the answer.
 *
 * values holds request_count words on entry and the answer on return;
 * capacity is how many words the tag's value buffer has, at least one and
 * at most DRV_RPI5_FIRMWARE_MAX_VALUES, and answered reports how many bytes
 * the firmware wrote.  Returns ENODEV before the mailbox is prepared, EIO
 * when the firmware refused the request, ENOTSUP when it did not understand
 * the tag and ETIMEDOUT when it did not answer.  Thread context only.
 */
int drv_rpi5_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered);

/*
 * Sends a whole property message the caller laid out (header, tags, end
 * tag) and copies the firmware's answer back over it.
 *
 * words is the message's length in 32-bit words, its first word the
 * length in bytes, at most DRV_RPI5_FIRMWARE_MESSAGE_BYTES.  Returns 0 when
 * the firmware handed the message back; the caller reads the response code
 * and each tag's own code itself.  Thread context only.
 */
int drv_rpi5_firmware_message(uint32_t *words, unsigned count);

/*
 * Asks one value about a firmware clock: its state, rate or limit, by the
 * tag that asks it.  Returns EIO for an answer about another clock or one
 * too short to hold the value.  Thread context only.
 */
int drv_rpi5_firmware_clock_get(uint32_t tag, uint32_t clock_id, uint32_t *value);

/*
 * Changes one setting of a firmware clock: its state or rate, by the tag
 * that sets it.  The firmware's turbo policy is left alone.  Returns EIO
 * for an answer about another clock.  Thread context only.
 */
int drv_rpi5_firmware_clock_set(uint32_t tag, uint32_t clock_id, uint32_t value);

#endif /* KERN_DRIVERS_PLATFORM_RPI5_FIRMWARE_H */
