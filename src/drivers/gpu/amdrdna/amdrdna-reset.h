/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reset of an RDNA GPU and its setting up again (reset.c,
 * reset-display.c), and the words and tables it is made of
 * (reset-format.c).
 *
 * A reset asks the security processor for a mode-1 reset of the whole
 * GPU, gives the PCI function back its configuration, runs the video
 * BIOS's post (the routine that initializes the GPU after power-on) on
 * the driver's runner, sets the memory, the security processor, the power
 * unit and the engines up again as far as the stages had, and lights the
 * firmware's display pipe again from the words it had before.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_RESET_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_RESET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/lock.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The dwords of the post routine's parameters (two words and sixteen
 * reserved), and the dwords of zeros the routine and the routines it
 * calls may read: the reference runner bounds the parameters by the
 * structure's size in bytes (72), so the driver gives that many dwords.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (struct asic_init_ps_allocation_v2_1) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c
 * (amdgpu_atomfirmware_asic_init: the size in bytes as the bound).
 */
#define AMDRDNA_RESET_POST_WORDS	18U
#define AMDRDNA_RESET_POST_SPACE_WORDS	72U

/* The resizable BARs whose control words a reset gives back. */
#define AMDRDNA_RESET_RESIZE_ENTRIES	6U

/* The display pipe's named words a reset keeps, and its other words. */
#define AMDRDNA_RESET_PIPE_WORDS	128U
#define AMDRDNA_RESET_EXTRA_WORDS	16U

/* Why a reset runs: a secure OS found at the boot, root's request, or the GPU core's recovery. */
enum amdrdna_reset_reason {
	AMDRDNA_RESET_BOOT,
	AMDRDNA_RESET_REQUEST,
	AMDRDNA_RESET_RECOVERY
};

/*
 * Which instance of the display chain a kept word belongs to: the
 * firmware's plane (its hub, pixel processor and cursor), its blend unit,
 * its output processor's blend tree, its timing generator, its encoder's
 * front and back end, the hub as a whole, or one of the hub's four
 * watermark sets (set gives which).
 */
enum amdrdna_reset_place {
	AMDRDNA_RESET_ON_PLANE,
	AMDRDNA_RESET_ON_BLEND,
	AMDRDNA_RESET_ON_TREE,
	AMDRDNA_RESET_ON_TIMING,
	AMDRDNA_RESET_ON_FRONT,
	AMDRDNA_RESET_ON_BACK,
	AMDRDNA_RESET_ON_HUB,
	AMDRDNA_RESET_ON_SET,
	AMDRDNA_RESET_ON_PROCESSOR
};

/* One named display register a reset keeps: which, where, and the watermark set. */
struct amdrdna_reset_pipe_entry {
	enum amdrdna_dcn_register which;
	enum amdrdna_reset_place place;
	uint8_t set;
};

/*
 * One display register the named table lacks: its window of the display
 * block, its first instance's offset, the distance between instances, and
 * whose instance it is.
 */
struct amdrdna_reset_extra_entry {
	uint8_t window;
	uint16_t offset;
	uint16_t stride;
	enum amdrdna_reset_place place;
};

/*
 * What a reset keeps of the display pipe: whether the words were read and
 * the pipe was lit, the named and the other words in their tables' order,
 * the pixel clock of the running timing in kHz, and the driver-owns bit of
 * the video BIOS's sixth scratch word.
 */
struct amdrdna_reset_pipe {
	bool saved;
	bool lit;
	uint32_t words[AMDRDNA_RESET_PIPE_WORDS];
	uint32_t extra[AMDRDNA_RESET_EXTRA_WORDS];
	uint32_t pixel_khz;
	bool driver_owns;
};

/*
 * The reset's state in the device record.
 *
 * lock serializes the callers that ask for a reset (one waits at a time);
 * the device's state lock covers requested, running, reason, finished
 * and result, which the bring-up thread and the caller share through the
 * device's state wait queue: the caller sets requested and waits for
 * finished to move, the thread runs the reset and sets result.  allowed
 * says the boot line lets a reset run on its own (amdrdna.reset=1).  The
 * rest is the reset's own working state on the bring-up thread: the PCI
 * configuration and the resizable BARs' control words taken before the
 * reset, the copy of the system-memory table, the display pipe, and the
 * counts.
 */
struct amdrdna_reset {
	struct mutex lock;
	bool allowed;
	bool requested;
	bool running;
	enum amdrdna_reset_reason reason;
	uint64_t finished;
	int result;
	struct drv_pci_saved_state pci;
	uint32_t resize_controls[AMDRDNA_RESET_RESIZE_ENTRIES];
	unsigned resize_count;
	uint64_t *table_copy;
	uint64_t table_entries;
	struct amdrdna_reset_pipe pipe;
	uint32_t resets;
	uint32_t last_ms;
};

struct amdrdna_device;

/* reset.c: the reset, run by the bring-up thread. */
void
amdrdna_reset_init(
	struct amdrdna_device *device);

bool
amdrdna_reset_allowed(
	const struct amdrdna_device *device);

int
amdrdna_reset_request(
	struct amdrdna_device *device,
	enum amdrdna_reset_reason reason,
	uint32_t *milliseconds);

unsigned long
amdrdna_reset_serve(
	struct amdrdna_device *device,
	unsigned long irq);

int
amdrdna_reset_boot(
	struct amdrdna_device *device);

/* reset-display.c: the display pipe across a reset. */
void
amdrdna_reset_display_save(
	struct amdrdna_device *device);

int
amdrdna_reset_display_restore(
	struct amdrdna_device *device);

void
amdrdna_reset_display_rearm(
	struct amdrdna_device *device);

/* What the reset asks of the stages' files: gmc.c, ih.c, smu.c, space.c. */
int
amdrdna_gart_save(
	struct amdrdna_device *device,
	uint64_t *copy,
	uint64_t entries);

int
amdrdna_a2_restart(
	struct amdrdna_device *device,
	const uint64_t *copy,
	uint64_t entries);

void
amdrdna_ih_quiet(
	struct amdrdna_device *device);

int
amdrdna_ih_restart(
	struct amdrdna_device *device);

int
amdrdna_power_rearm(
	struct amdrdna_device *device);

int
amdrdna_spaces_restart(
	struct amdrdna_device *device);

/* reset-format.c: the words and tables of a reset. */
void
amdrdna_reset_post_words(
	uint32_t *words,
	uint32_t engine_10khz,
	uint32_t memory_10khz);

int
amdrdna_reset_post_format(
	const uint8_t *image,
	size_t length,
	uint16_t routine);

uint32_t
amdrdna_reset_pixel_khz(
	uint32_t h_total,
	uint32_t v_total,
	uint32_t refresh_millihz);

unsigned
amdrdna_reset_pipe_count(void);

const struct amdrdna_reset_pipe_entry *
amdrdna_reset_pipe_entry(
	unsigned index);

unsigned
amdrdna_reset_extra_count(void);

const struct amdrdna_reset_extra_entry *
amdrdna_reset_extra_entry(
	unsigned index);

uint32_t
amdrdna_reset_timing_stopped(
	uint32_t timing_control);

uint32_t
amdrdna_reset_vertical_word(
	uint32_t vertical,
	bool on);

bool
amdrdna_reset_timing_running(
	uint32_t timing_control);

#endif
