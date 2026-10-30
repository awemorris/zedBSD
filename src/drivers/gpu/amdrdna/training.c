/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A3t: the second, long stage of the GDDR6 memory training.
 *
 * The board firmware trains the memory well enough to show its picture;
 * a board whose video BIOS asks for it leaves the long stage to the
 * driver, which asks the security processor's boot loader to run it on a
 * cold boot, before the secure OS starts.  The training overwrites the
 * lowest 32 MiB of VRAM, which hold the firmware's picture, so the stage
 * stops the text console, copies those 32 MiB to system memory, blanks the
 * picture's plane, runs the training, copies the 32 MiB back, unblanks the
 * plane and lets the console draw again.  It keeps the 4 KiB of training
 * data the security processor leaves at the top of VRAM.
 *
 * On a board that does not ask for it, or once a secure OS runs, the stage
 * only says why it skips.  Either way it ends by giving the lowest 32 MiB
 * back to the VRAM pools.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The VRAM the training overwrites (the lowest 32 MiB), and the training
 * data's size.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h
 * (BIST_MEM_TRAINING_ENCROACHED_SIZE,
 * GDDR6_MEM_TRAINING_DATA_SIZE_IN_BYTES).
 */
#define TRAINING_OVERWRITTEN_BYTES	0x2000000U
#define TRAINING_DATA_BYTES		0x1000U

/*
 * How long the training may take: 30 polls of 100 ms (3 s).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c
 * (psp_v11_0_memory_training_send_msg: MEM_TRAIN_SEND_MSG_TIMEOUT_US over
 * the 100 ms of one wait).
 */
#define TRAINING_POLLS			30U
#define TRAINING_POLL_US		100000U

/*
 * The plane's control word: bit 0 blanks the plane.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_sh_mask.h
 * (HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK).
 */
#define TRAINING_PLANE_BLANK		0x00000001U

static unsigned training_skip_reason(struct amdrdna_device *device);
static const char *training_reason_name(unsigned reason);
static int training_run(struct amdrdna_device *device);
static void training_copy(struct amdrdna_device *device, uint8_t *saved, bool back);
static void training_blank(struct amdrdna_device *device, bool blank);
static int training_finish(struct amdrdna_device *device);

/*
 * Runs stage A3t: the long memory training when the board asks for it,
 * else a skip with its reason; then the lowest 32 MiB of VRAM become
 * ordinary VRAM.
 *
 * Returns 0 for a training that finished or was skipped, ETIMEDOUT when
 * the training did not finish in time (the stage machine stops: the boot
 * loader may still be training), or ENOMEM.
 */
int
amdrdna_a3t_training(
	struct amdrdna_device *device)
{
	unsigned reason;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A3t begin");

	/* A board that does not ask for the training, or one already past it, skips it. */
	reason = training_skip_reason(device);
	if (reason != AMDRDNA_TRAINING_SKIP_NONE) {
		device->training.result = AMDRDNA_TRAINING_SKIPPED;
		device->training.reason = reason;
		amdrdna_mark("A3t skip: %s", training_reason_name(reason));

		/* Gives the lowest 32 MiB back to the pools. */
		error = training_finish(device);
		if (error != 0)
			return error;

		/* Succeeded: the stage is done without training. */
		amdrdna_mark("A3t ok train=skip");
		return 0;
	}

	/* Runs the training. */
	error = training_run(device);
	if (error != 0)
		return error;

	/* Gives the lowest 32 MiB back to the pools. */
	error = training_finish(device);
	if (error != 0)
		return error;

	/* Succeeded: the memory is trained and the picture is back. */
	amdrdna_mark("A3t ok train=long %ums restored=%uMiB",
	    (unsigned)device->training.milliseconds,
	    (unsigned)(TRAINING_OVERWRITTEN_BYTES >> 20));
	return 0;
}

/*
 * Reports why the training is skipped, or AMDRDNA_TRAINING_SKIP_NONE when
 * it runs: the boot line turned it off, there is no video BIOS image, the
 * board does not ask for it, or a secure OS already runs (the boot loader
 * takes no training then).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c (psp_v11_0_memory_training: a
 * live secure OS skips it) and amdgpu_atomfirmware.c
 * (amdgpu_atomfirmware_mem_training_supported).
 */
static unsigned
training_skip_reason(
	struct amdrdna_device *device)
{
	unsigned asked;
	bool allowed;
	uint32_t word;

	/* The boot line can turn the training off. */
	asked = amdrdna_parameter_number("amdrdna.train", 1U);
	if (asked == 0U)
		return AMDRDNA_TRAINING_SKIP_ASKED;

	/* Without the video BIOS image there is no capability to read. */
	if (device->vbios == NULL)
		return AMDRDNA_TRAINING_SKIP_NO_VBIOS;

	/* The image's capability bit, and the boot database's when there is one. */
	allowed = amdrdna_training_allowed(device);
	if (!allowed)
		return AMDRDNA_TRAINING_SKIP_NOT_ASKED;

	/* A secure OS that runs already means the boot is not cold. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_SECURE_OS_ALIVE);
	if (word != 0U)
		return AMDRDNA_TRAINING_SKIP_SECURE_OS;

	/* The training runs. */
	return AMDRDNA_TRAINING_SKIP_NONE;
}

/* Names a skip reason for the stage mark. */
static const char *
training_reason_name(
	unsigned reason)
{
	/* Each reason the stage gives. */
	switch (reason) {
	case AMDRDNA_TRAINING_SKIP_ASKED:
		return "amdrdna.train=0";
	case AMDRDNA_TRAINING_SKIP_NO_VBIOS:
		return "no vbios";
	case AMDRDNA_TRAINING_SKIP_NOT_ASKED:
		return "the vbios does not ask for the long training";
	case AMDRDNA_TRAINING_SKIP_SECURE_OS:
		return "a secure OS already runs";
	default:
		break;
	}

	/* No reason to name. */
	return "?";
}

/*
 * Runs the long training with the lowest 32 MiB saved: the console
 * stopped, the 32 MiB copied out, the plane blanked, the boot loader asked
 * to train and waited for, the 32 MiB copied back, the plane unblanked,
 * the console drawing again, and the training data kept.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_v11_0.c (psp_v11_0_memory_training,
 * psp_v11_0_memory_training_send_msg) and amdgpu_ttm.c
 * (amdgpu_ttm_training_data_block_init).
 */
static int
training_run(
	struct amdrdna_device *device)
{
	uint8_t *saved;
	uint8_t *data;
	uint64_t driver_copy;
	uint64_t processor_copy;
	uint64_t started;
	uint64_t ended;
	uint32_t word;
	unsigned poll;
	unsigned blank;
	bool done;

	/* Takes the memory the 32 MiB are saved in before anything changes. */
	saved = kern_malloc(TRAINING_OVERWRITTEN_BYTES);
	if (saved == NULL) {
		amdrdna_mark("A3t fail: no memory to save the low VRAM");
		return ENOMEM;
	}

	/* Takes the memory the training data is kept in. */
	data = kern_malloc(TRAINING_DATA_BYTES);
	if (data == NULL) {
		kern_free(saved);
		amdrdna_mark("A3t fail: no memory for the training data");
		return ENOMEM;
	}

	/* Where the boot loader reads the driver's copy of the data and writes its own. */
	amdrdna_training_place(device->vram_bytes, (uint64_t)device->vbios_info.reserved_top_kib << 10, &driver_copy, &processor_copy);
	blank = amdrdna_parameter_number("amdrdna.train.blank", 1U);
	amdrdna_mark("A3t training c2p=0x%llx p2c=0x%llx blank=%u; the console stops until it ends",
	    (unsigned long long)driver_copy,
	    (unsigned long long)processor_copy,
	    blank);

	/* Stops the console, so nothing writes to the picture while it is overwritten. */
	kern_text_suspend();

	/* Saves the lowest 32 MiB. */
	amdrdna_host_path_drop(device);
	training_copy(device, saved, false);

	/* Blanks the plane, so the overwritten picture is not shown. */
	if (blank != 0U)
		training_blank(device, true);

	/* Asks the boot loader for the long training. */
	started = sched_ticks();
	amdrdna_block_write(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_BOOT_ARGUMENT, (uint32_t)(driver_copy >> AMDRDNA_PSP_ADDRESS_SHIFT));
	amdrdna_block_write(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_BOOT_COMMAND, AMDRDNA_PSP_BOOT_LONG_TRAINING);

	/* Waits for the done bit, in steps of 100 ms. */
	done = false;
	for (poll = 0U; poll < TRAINING_POLLS; poll++) {
		/* Sleeps one step, then looks. */
		kern_usleep_range(TRAINING_POLL_US, TRAINING_POLL_US + TRAINING_POLL_US / 10U);
		word = amdrdna_block_read(device, AMDRDNA_BLOCK_PSP, AMDRDNA_PSP_WINDOW_MAILBOX, AMDRDNA_PSP_REG_BOOT_COMMAND);
		if ((word & AMDRDNA_PSP_DONE) != 0U) {
			done = true;
			break;
		}
	}

	/* Notes when the wait ended. */
	ended = sched_ticks();

	/* Puts the 32 MiB back and makes them visible to the display. */
	training_copy(device, saved, true);
	amdrdna_host_path_flush(device);

	/* Shows the picture again, and lets the console draw. */
	if (blank != 0U)
		training_blank(device, false);
	kern_text_resume();
	kern_free(saved);

	/* Reports a training that did not finish. */
	if (!done) {
		kern_free(data);
		amdrdna_mark("A3t fail: the long training did not finish in %ums (boot word 0x%08x)",
		    (unsigned)(TRAINING_POLLS * (TRAINING_POLL_US / 1000U)),
		    (unsigned)word);
		return ETIMEDOUT;
	}

	/* Keeps the training data the security processor left. */
	amdrdna_host_path_drop(device);
	amdrdna_vram_read(device, processor_copy, data, TRAINING_DATA_BYTES);
	device->training.data = data;

	/* Records the result. */
	device->training.result = AMDRDNA_TRAINING_RAN;
	device->training.reason = AMDRDNA_TRAINING_SKIP_NONE;
	device->training.milliseconds = (uint32_t)kern_ticks_to_ms(ended - started);

	/* Succeeded: the training finished and the low VRAM is restored. */
	return 0;
}

/*
 * Copies the lowest 32 MiB of VRAM out to the saved buffer, or back
 * (back true).  The part below the CPU's window (the firmware's picture,
 * which the console maps on its own) goes through the indirect VRAM
 * window; the rest through the window.
 */
static void
training_copy(
	struct amdrdna_device *device,
	uint8_t *saved,
	bool back)
{
	volatile uint8_t *window;
	uint64_t below;
	uint64_t offset;
	uint32_t word;

	/* The part below the window. */
	below = device->window_from;
	if (below > TRAINING_OVERWRITTEN_BYTES)
		below = TRAINING_OVERWRITTEN_BYTES;

	/* Copies that part one dword at a time through the indirect window. */
	if (below != 0U) {
		if (back) {
			amdrdna_vram_write(device, 0U, saved, (size_t)below);
		} else {
			amdrdna_vram_read(device, 0U, saved, (size_t)below);
		}
	}

	/* Copies the rest through the window, one dword at a time. */
	for (offset = below; offset < TRAINING_OVERWRITTEN_BYTES; offset += 4U) {
		/* One dword of the window, in the direction asked for. */
		window = amdrdna_vram_cpu(device, offset);
		if (back) {
			kern_memcpy(&word, saved + offset, sizeof(word));
			kern_mmio_write32(window, word);
		} else {
			word = kern_mmio_read32(window);
			kern_memcpy(saved + offset, &word, sizeof(word));
		}
	}
}

/* Blanks or unblanks the plane that shows the firmware's picture. */
static void
training_blank(
	struct amdrdna_device *device,
	bool blank)
{
	uint32_t index;
	uint32_t word;

	/* The plane's control word; a display without it is left alone. */
	index = amdrdna_dcn_index(&device->blocks.blocks[AMDRDNA_BLOCK_DCN], AMDRDNA_DCN_PLANE_CONTROL, device->gop.plane);
	if (index == 0U)
		return;

	/* Sets or clears the blank bit only. */
	word = amdrdna_read(device, index);
	if (blank) {
		word |= TRAINING_PLANE_BLANK;
	} else {
		word &= ~TRAINING_PLANE_BLANK;
	}

	/* Writes the word back. */
	amdrdna_write(device, index, word);
}

/* Gives the lowest 32 MiB back to the pools, reporting a failure in the stage mark. */
static int
training_finish(
	struct amdrdna_device *device)
{
	int error;

	/* The range is ordinary VRAM from now on. */
	error = amdrdna_training_range_release(device);
	if (error != 0) {
		amdrdna_mark("A3t fail: the low VRAM not given back to the pools (%d)", error);
		return error;
	}

	/* Succeeded: the pools hold the low VRAM. */
	return 0;
}
