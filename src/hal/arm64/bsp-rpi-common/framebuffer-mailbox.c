/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where a Raspberry Pi's console framebuffer comes from: the VideoCore
 * firmware allocates it when asked through the property mailbox.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "../framebuffer.h"
#include "mailbox.h"

/* The property tags the HAL sends: get the display's size, then set up and allocate. */
#define TAG_GET_PHYSICAL 0x00040003U
#define TAG_PHYSICAL 0x00048003U
#define TAG_VIRTUAL 0x00048004U
#define TAG_DEPTH 0x00048005U
#define TAG_ORDER 0x00048006U
#define TAG_ALLOCATE 0x00040001U
#define TAG_PITCH 0x00040008U

/*
 * Where the firmware's answers lie in the set-up request: the width and
 * the height of the first tag, the pixel order, the base and the size of
 * the allocation, and the pitch.
 */
#define ANSWER_WIDTH 5U
#define ANSWER_HEIGHT 6U
#define ANSWER_ORDER 19U
#define ANSWER_BASE 23U
#define ANSWER_SIZE 24U
#define ANSWER_PITCH 28U

/* The words of the set-up request, and of the size request. */
#define SETUP_WORDS 30U
#define SIZE_WORDS 8U

/* The smallest size taken from the firmware, and the largest. */
#define MIN_WIDTH 640U
#define MIN_HEIGHT 400U
#define MAX_SIZE 4096U

/* The size asked for when the firmware reports none (QEMU). */
#define DEFAULT_WIDTH 640U
#define DEFAULT_HEIGHT 480U

/*
 * The message the property requests are built in, 16-byte aligned as the
 * mailbox requires.  Used only on the boot CPU while the framebuffer is
 * set up.
 */
static uint32_t request[40] __attribute__((aligned(16)));

/*
 * Asks the firmware for a 32-bit framebuffer of the display's size and
 * clears it.
 *
 * The size is the one the firmware set up for the display (config.txt's
 * framebuffer_width and framebuffer_height, or the mode's), so that the
 * framebuffer is scanned out as it is, without scaling; 640x480 when the
 * firmware reports none (QEMU).  The firmware's mailbox is the one the
 * device tree names.  Returns 0 when the framebuffer is ready.
 */
int
arm64_bsp_framebuffer_init(
	void)
{
	const struct arm64_fdt_info *info;
	uintptr_t mailbox_phys;
	uint32_t want_width;
	uint32_t want_height;
	uint64_t phys;
	int error;

	/* Finds the firmware's mailbox the tree names. */
	info = arm64_boot_info();
	mailbox_phys = (uintptr_t)info->mailbox_base;

	/* Asks for the display's size: one tag with room for two words. */
	request[0] = SIZE_WORDS * 4U;
	request[1] = 0;
	request[2] = TAG_GET_PHYSICAL;
	request[3] = 8;
	request[4] = 0;
	request[5] = 0;
	request[6] = 0;
	request[7] = 0;
	error = rpi_mailbox_property(mailbox_phys, request, request[0]);

	/* Takes the firmware's size when it reported a sensible one. */
	want_width = DEFAULT_WIDTH;
	want_height = DEFAULT_HEIGHT;
	if (error == 0) {
		/* A size the console fits in and the firmware can scan out. */
		if (request[5] >= MIN_WIDTH &&
		    request[6] >= MIN_HEIGHT &&
		    request[5] <= MAX_SIZE &&
		    request[6] <= MAX_SIZE) {
			want_width = request[5];
			want_height = request[6];
		}
	}

	/* Starts the set-up request: its size and the request code. */
	request[0] = SETUP_WORDS * 4U;
	request[1] = 0;

	/* The display's size in pixels. */
	request[2] = TAG_PHYSICAL;
	request[3] = 8;
	request[4] = 8;
	request[5] = want_width;
	request[6] = want_height;

	/* The framebuffer's size, the same, so nothing is scaled. */
	request[7] = TAG_VIRTUAL;
	request[8] = 8;
	request[9] = 8;
	request[10] = want_width;
	request[11] = want_height;

	/* 32 bits per pixel. */
	request[12] = TAG_DEPTH;
	request[13] = 4;
	request[14] = 4;
	request[15] = 32;

	/* Red in the low byte, which the firmware may refuse; the answer says which it took. */
	request[16] = TAG_ORDER;
	request[17] = 4;
	request[18] = 4;
	request[19] = 1;

	/* The allocation, aligned to 4096 bytes; the answer is its base and size. */
	request[20] = TAG_ALLOCATE;
	request[21] = 8;
	request[22] = 8;
	request[23] = 4096;
	request[24] = 0;

	/* The row length in bytes. */
	request[25] = TAG_PITCH;
	request[26] = 4;
	request[27] = 4;
	request[28] = 0;

	/* The end tag. */
	request[29] = 0;

	/* Sends the request; a refused one leaves no framebuffer. */
	error = rpi_mailbox_property(mailbox_phys, request, request[0]);
	if (error != 0)
		return -1;

	/*
	 * Reads the answers.  The base is a bus address whose top two bits
	 * name the VideoCore's cache alias; dropping them leaves the ARM's
	 * physical address, which on both boards lies in the first gigabyte.
	 */
	phys = (uint64_t)(request[ANSWER_BASE] & 0x3fffffffU);

	/* Takes the allocation for the console, which checks that it is large enough. */
	error = arm64_framebuffer_adopt(phys,
				      request[ANSWER_SIZE],
				      request[ANSWER_WIDTH],
				      request[ANSWER_HEIGHT],
				      request[ANSWER_PITCH],
				      request[ANSWER_ORDER],
				      0);
	if (error != 0)
		return -1;

	/* Succeeded: the framebuffer is ready for the console. */
	return 0;
}
