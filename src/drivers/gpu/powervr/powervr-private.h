/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR driver's own state, shared by its early stage, the memory
 * the firmware runs in and the later stage that starts the firmware.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_PRIVATE_H
#define KERN_DRIVERS_GPU_POWERVR_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/lock.h>
#include <kern/pmem.h>

#include "drivers/gpu/powervr/powervr.h"
#include "drivers/gpu/powervr/firmware-image.h"
#include "drivers/gpu/powervr/mips-table.h"
#include "drivers/gpu/powervr/stage.h"
#include "drivers/gpu/powervr/interface.h"

/*
 * Whether the render check takes the firmware's whole trace of its first
 * geometry kick (1): every trace group, the debug group too, the words
 * from the kick to the recovery, and no other dump or kick after it, so
 * the kernel's message ring keeps them.  0 is the usual check.
 */
/*
 * Whether the start prints the firmware's trace from its first word,
 * after the health check (up to 2048 words, every group with the full
 * trace's log type), and the compute and render checks are left out so
 * the kernel's message ring keeps it (1).
 */
/*
 * Whether the driver prints its long diagnostics (1): the register check
 * and map at start, the return slot probe, the render check's register
 * probes and its target, list, context and command dumps, every refused
 * kick, and the Vulkan executor's per-dispatch layout.  0 (the default)
 * keeps a working boot's messages short enough for the kernel's message
 * ring to hold the start, the stages and gpu-selftest; failures still say
 * their step, their reason and a short firmware trace.
 */
#ifndef POWERVR_DIAGNOSTICS
#define POWERVR_DIAGNOSTICS	0
#endif

#ifndef POWERVR_FIRMWARE_BOOT_TRACE
#define POWERVR_FIRMWARE_BOOT_TRACE	0
#endif

#ifndef POWERVR_RENDER_CHECK_FULL_TRACE
#define POWERVR_RENDER_CHECK_FULL_TRACE	0
#endif

/*
 * One block of memory the firmware's processor sees.
 *
 * The block is one physically contiguous run below the core's DMA limit,
 * mapped at heap_offset in the processor's page table.  The host reads
 * and writes it only through view, its uncached alias, with plain loads
 * and stores, so the core and the host see the same bytes without cache
 * maintenance.  A block with no view holds nothing.
 */
struct powervr_object {
	struct kern_pmem run;
	volatile uint8_t *view;
	uint32_t heap_offset;
	uint32_t size;
};

/*
 * The firmware's heap: the processor's page table and which pages of its
 * general part blocks hold.
 *
 * Blocks at a place of their own (the code, the data, the three
 * configuration records) are placed first; the rest take the first free
 * run of the general part, and give it back when they go.  Once the
 * firmware runs, blocks come and go only under the core's lock.
 */
struct powervr_heap {
	struct powervr_object table;
	uint64_t pages[POWERVR_HEAP_MAP_WORDS];
	unsigned bus_bits;
	uint64_t dma_limit;
};

/*
 * One command of the kernel's buffer in 32-bit words, and how long the
 * firmware has to execute one.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_check.h
 * (struct rogue_fwif_kccb_cmd, 88 bytes); pvr_mmu.c (pvr_mmu_flush_exec:
 * one second).
 */
#define POWERVR_COMMAND_WORDS		22U
#define POWERVR_COMMAND_WAIT_MS		1000U

struct powervr_render;

/* Asks for a block anywhere in the heap's general part. */
#define POWERVR_HEAP_ANYWHERE		0xffffffffU

/*
 * Everything the running firmware needs from the host.
 *
 * File holds the firmware file as read, kept for as long as the core so
 * that a reset can load the program again without the root file system.
 * The command buffers and records are blocks of the heap; the interrupt
 * count is moved only by the interrupt handler and read by the later
 * stage.
 */
struct powervr_firmware {
	uint8_t *file;
	size_t file_size;
	struct powervr_fw_image image;
	struct powervr_heap heap;
	struct powervr_object code;
	struct powervr_object data;
	struct powervr_object connection;
	struct powervr_object os_init;
	struct powervr_object system_init;
	struct powervr_object kccb_control;
	struct powervr_object kccb;
	struct powervr_object kccb_returns;
	struct powervr_object fwccb_control;
	struct powervr_object fwccb;
	struct powervr_object os_data;
	struct powervr_object system_data;
	struct powervr_object power_sync;
	struct powervr_object hwr_info;
	struct powervr_object mmu_sync;
	struct powervr_object fault_page;
	struct powervr_object utilisation;
	struct powervr_object runtime;
	struct powervr_object trace_control;
	struct powervr_object trace_buffer;
	uint32_t fwccb_read;
	volatile uint32_t interrupts;
	uint32_t kccb_sent;
	uint32_t kccb_expect[POWERVR_KERNEL_CCB_SLOTS];
	bool return_fallback_noted;
	int interrupt_installed;
};

/*
 * The board's one core.
 *
 * Filled by the early stage on the boot CPU; registers is NULL until the
 * core is kept.  The later stage owns the firmware state from the moment
 * its thread starts, and nothing else touches it until then.  Once the
 * render node runs, lock serializes the kernel's command buffer, the
 * firmware's heap and every context's firmware records; render is set
 * before the node is published and read by the interrupt handler.
 */
struct powervr_core {
	struct drv_powervr_description description;
	volatile uint8_t *registers;
	uint64_t core_rate;
	uint32_t branch;
	uint32_t version;
	uint32_t units;
	uint32_t config;
	struct powervr_firmware firmware;
	struct mutex lock;
	struct powervr_render *volatile render;
};

/* Reads and writes the core's registers. */
uint32_t powervr_read32(const struct powervr_core *core, uint32_t offset);
uint64_t powervr_read64(const struct powervr_core *core, uint32_t offset);
void powervr_write32(struct powervr_core *core, uint32_t offset, uint32_t value);
void powervr_write64(struct powervr_core *core, uint32_t offset, uint64_t value);

/*
 * Releases the core's registers and powers it down: the resets held, the
 * clocks stopped in reverse and the domain off.
 */
void powervr_power_down(struct powervr_core *core);

/*
 * Powers the core up again after powervr_power_down() and maps its
 * registers.
 */
int powervr_power_up(struct powervr_core *core);

/*
 * Starts the firmware again from the kept file after a reset; on failure
 * the core is left powered down with nothing kept.
 */
int powervr_firmware_restart(struct powervr_core *core);

/* Writes one command into the kernel's buffer and kicks the firmware; EBUSY when full. */
int powervr_command_send(struct powervr_core *core, const uint32_t command[POWERVR_COMMAND_WORDS], uint32_t *slot);

/* Waits, bounded, for the command in a slot to be executed, and reports its answer. */
int powervr_command_wait(struct powervr_core *core, uint32_t slot, unsigned milliseconds, uint32_t *answer);
int powervr_command_wait_executed(struct powervr_core *core, uint32_t slot, unsigned milliseconds);
void powervr_firmware_dump(struct powervr_core *core, uint32_t slot);
void powervr_firmware_trace_mark(struct powervr_core *core);
void powervr_firmware_trace_since_mark(struct powervr_core *core);
void powervr_firmware_return_probe(struct powervr_core *core, uint32_t slot);

/* Sends one command and waits, bounded, for its answer. */
int powervr_command_run(struct powervr_core *core, const uint32_t command[POWERVR_COMMAND_WORDS], unsigned milliseconds, uint32_t *answer);

/* Has the firmware drop what the core's MMU cached of the page tables, and waits. */
int powervr_command_mmu_flush(struct powervr_core *core);

/* Has the firmware write back and drop every line of the system level cache; the core's lock is held. */
int powervr_command_slc_flush(struct powervr_core *core);

/*
 * Takes every notice the firmware queued, handing each well-formed one to
 * handle (which may be NULL), and reports how many there were.
 */
unsigned powervr_notices_take(struct powervr_core *core, void (*handle)(void *context, uint32_t type, const struct powervr_object *buffer, uint32_t offset), void *context);

/*
 * Checks, on the running firmware, whether the processor's cached view
 * sees what the firmware's processor writes, and reports the outcome on
 * one line.  It changes nothing of how memory is shared.
 */
void powervr_coherency_check(struct powervr_core *core);

/*
 * Publishes the render node over the running firmware.  On failure it
 * has undone its own part; the caller stops the firmware.
 */
int powervr_render_start(struct powervr_core *core);

/*
 * Frees what the render node kept for a reset once the core is stopped
 * for good: nothing reaches it any more.
 */
void powervr_render_abandon(struct powervr_core *core);

/* Wakes the render node's service from the interrupt handler, when there is one. */
void powervr_render_interrupt(struct powervr_core *core);

/*
 * Starts the later stage on a thread of its own: it waits for the root
 * file system, loads the firmware and starts it.
 */
int powervr_late_start(struct powervr_core *core);

/* Sets up the heap and its empty page table. */
int powervr_heap_init(struct powervr_heap *heap, unsigned bus_bits, uint64_t dma_limit);

/* Releases the page table; every block must be gone first. */
void powervr_heap_fini(struct powervr_heap *heap);

/*
 * Makes a zeroed block of at least size bytes and maps it, at
 * heap_offset or anywhere (POWERVR_HEAP_ANYWHERE).
 */
int powervr_object_create(struct powervr_heap *heap, uint32_t size, uint32_t heap_offset, enum powervr_mips_caching caching, struct powervr_object *object);

/* Unmaps a block and returns its memory; a block that was never made is left alone. */
void powervr_object_destroy(struct powervr_heap *heap, struct powervr_object *object);

/*
 * Returns a block's memory without touching the page table, for a block
 * that outlived its heap.
 */
void powervr_object_drop(struct powervr_object *object);

/* Reports the firmware processor's address of a byte of a block. */
uint32_t powervr_object_address(const struct powervr_object *object, uint32_t offset);

/* Reports the physical address of a byte of a block. */
uint64_t powervr_object_physical(const struct powervr_object *object, uint32_t offset);

/* Stores a 32-bit field into a block. */
void powervr_object_put32(struct powervr_object *object, uint32_t offset, uint32_t value);

/* Stores a 64-bit field into a block. */
void powervr_object_put64(struct powervr_object *object, uint32_t offset, uint64_t value);

/* Loads a 32-bit field from a block. */
uint32_t powervr_object_get32(const struct powervr_object *object, uint32_t offset);

/* Copies bytes of the kernel's into a block. */
void powervr_object_fill(struct powervr_object *object, uint32_t offset, const uint8_t *bytes, size_t length);

/* Readies the stage record and hw.gpu.powervr.stages (stage-log.c); once, while attaching. */
void powervr_stage_init(void);

/* Notes that a stage succeeded; its line "powervr: <stage>: ok" is printed once a boot. */
void powervr_stage_ok(enum powervr_stage stage);

/* Notes that a stage failed; "powervr: <stage>: fail(<reason>)" is printed the first three times. */
void powervr_stage_fail(enum powervr_stage stage, enum powervr_stage_reason reason);

/* Notes a failure with its value: "fail(fault 0x...)" or "fail(reset <reason>)". */
void powervr_stage_fail_value(enum powervr_stage stage, enum powervr_stage_reason reason, uint64_t value);

#endif
