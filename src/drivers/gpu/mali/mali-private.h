/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali Valhall GPU driver's own state, shared by its files.
 *
 * One board has one GPU.  The attach fills the device on the boot CPU and
 * starts the later stage's thread, which alone changes the device from
 * then on; the three interrupt handlers only record what they saw.
 */

#ifndef KERN_DRIVERS_GPU_MALI_PRIVATE_H
#define KERN_DRIVERS_GPU_MALI_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/lock.h>
#include <kern/pmem.h>

#include "drivers/gpu/mali/mali-firmware-image.h"
#include "drivers/gpu/mali/mali-interface.h"
#include "drivers/gpu/mali/mali-pagetable.h"

/* The address space the microcontroller's firmware runs in, the one of the driver's own group, and the render node's. */
#define MALI_MCU_ADDRESS_SPACE		0U
#define MALI_GROUP_ADDRESS_SPACE	1U
#define MALI_RENDER_ADDRESS_SPACE	2U

/* The microcontroller's virtual space: 4 GiB. */
#define MALI_MCU_VIRTUAL_BYTES		0x100000000ULL

/*
 * The part of the microcontroller's space the driver places its own
 * objects in (ring buffer blocks, suspend buffers): after the firmware's
 * sections inside the shared region, up to the region's end.
 */
#define MALI_MCU_KERNEL_END		(MALI_FW_SHARED_START + MALI_FW_SHARED_SIZE)

/* Where the driver's own group places its objects in its space, and the end of that part. */
#define MALI_GROUP_SPACE_START		0x01000000ULL
#define MALI_GROUP_SPACE_END		0x02000000ULL

/* The most translation table pages one address space keeps. */
#define MALI_TABLE_PAGES_MAX		256U

/* The highest physical address the driver lets the GPU name. */
#define MALI_DMA_LIMIT			0xffffffffULL

/* The page the driver allocates and maps in. */
#define MALI_PAGE_BYTES			4096ULL

struct mali_device;

/*
 * The power steps of the SoC around the GPU, and their context.
 *
 * Power up opens the GPU's bus and core clocks at their rate, releases
 * its resets and reports the core clock's rate in hertz; on failure it
 * leaves everything as it found it.  Registers ready, when not NULL, is
 * called once the GPU's registers are mapped and before its soft reset,
 * for what the SoC writes there.  Power down holds the resets.
 */
struct mali_glue {
	int (*power_up)(void *context, uint64_t *core_rate);
	void (*registers_ready)(void *context, struct mali_device *device);
	void (*power_down)(void *context);
	void *context;
};

/*
 * One run of memory the GPU reaches.
 *
 * The run is whole pages below MALI_DMA_LIMIT; view is the processor's
 * uncached view of it (the GPU does not snoop the processor's caches),
 * and gpu_address is where the address space it is mapped in shows it,
 * zero while unmapped.
 */
struct mali_buffer {
	struct kern_pmem run;
	volatile uint8_t *view;
	uint64_t bytes;
	uint64_t gpu_address;
};

/*
 * One address space: its translation tables and the slot it is bound to.
 *
 * The tables live in pages of their own (table_runs), each with its
 * uncached view, so the GPU's walk reads what the builder wrote without
 * cache maintenance.  The pages are kept until the space is destroyed.
 * Enabled is nonzero while the slot translates through these tables, so
 * a later change of them needs the slot's caches flushed.
 */
struct mali_address_space {
	struct mali_pt tables;
	unsigned slot;
	int enabled;
	unsigned table_count;
	struct kern_pmem table_runs[MALI_TABLE_PAGES_MAX];
	uint64_t *table_views[MALI_TABLE_PAGES_MAX];
};

/*
 * The driver's own command stream: stream 0 of group slot 0.
 *
 * The ring buffer lives in the group's address space, where the stream
 * reads its instructions; the ring's two small blocks (the input page,
 * then the output page) and the group's suspend buffers live in the
 * microcontroller's space, where the firmware reads and writes them.
 * Running is nonzero once the firmware acknowledged the start.  space
 * is the address space the group translates through (its slot is what
 * the group is programmed with), inserted the bytes written to the ring
 * since the start, and registers the stream's work registers.
 */
struct mali_queue {
	struct mali_buffer ring;
	struct mali_buffer ring_blocks;
	struct mali_buffer suspend;
	struct mali_buffer protected_suspend;
	struct mali_address_space *space;
	uint64_t inserted;
	unsigned group_slot;
	unsigned stream;
	unsigned doorbell;
	unsigned registers;
	int running;
};

/*
 * What a stream reported while it was waited for: its last fault and
 * fatal words (the exception type in the low byte, its data above) and
 * their 64-bit information words, and how many faults it raised.
 */
struct mali_queue_fault {
	uint32_t fault;
	uint32_t fatal;
	uint64_t fault_info;
	uint64_t fatal_info;
	unsigned faults;
};

struct mali_render;

/*
 * The board's GPU.
 *
 * The identity and the features are read once at attach.  The event
 * words are what the interrupt handlers saw since the later stage last
 * took them, ORed in with atomic operations; nothing else writes them.
 */
struct mali_device {
	/* Where the registers are, and the processor's view of them. */
	uint64_t register_base;
	uint64_t register_bytes;
	volatile uint8_t *registers;

	/* The three interrupt lines: the GPU block, the MMU and the job (firmware) block. */
	int irq_gpu;
	int irq_mmu;
	int irq_job;

	/* The SoC's steps, and the core clock's rate they reported. */
	struct mali_glue glue;
	uint64_t core_rate;

	/* The identity and features. */
	uint32_t gpu_id;
	uint32_t revision;
	uint32_t l2_features;
	uint32_t core_features;
	uint32_t tiler_features;
	uint32_t memory_features;
	uint32_t mmu_features;
	uint32_t address_spaces;
	uint32_t csf_id;
	uint32_t coherency_features;
	uint64_t shader_present;
	uint64_t tiler_present;
	uint64_t l2_present;
	unsigned arch_major;
	unsigned arch_minor;
	unsigned arch_revision;
	unsigned product;
	unsigned version_major;
	unsigned version_minor;
	unsigned version_status;
	unsigned virtual_bits;
	unsigned physical_bits;

	/* What the interrupt handlers saw. */
	uint32_t gpu_events;
	uint32_t job_events;
	uint32_t mmu_faults;
	uint32_t fault_status;
	uint64_t fault_address;

	/* The microcontroller's address space and the next free place for the driver's objects in it. */
	struct mali_address_space mcu;
	uint64_t mcu_next;

	/* The driver's group's address space and the next free place in it. */
	struct mali_address_space group;
	uint64_t group_next;

	/* The firmware: its parsed sections, their memory, and the interfaces in the shared one. */
	struct mali_fw_image image;
	struct mali_buffer sections[MALI_FW_SECTIONS_MAX];
	struct mali_buffer *shared;
	struct mali_interfaces interfaces;
	int firmware_running;

	/* The driver's own command stream, and its self-test's sync object. */
	struct mali_queue queue;
	struct mali_buffer queue_sync;

	/*
	 * Serializes every use of the firmware's interface and of the
	 * address space slots after the later stage's start: the watch's
	 * pings and the render node's submissions.
	 */
	struct mutex lock;

	/* The render node, once published (render.c). */
	struct mali_render *render;

	/* The microcontroller's status last written to the log, so only a change is written. */
	uint32_t mcu_status;
};

/* The register accessors of mali-gpu.c. */
uint32_t mali_read32(const struct mali_device *device, uint32_t offset);
uint64_t mali_read64(const struct mali_device *device, uint32_t offset);
void mali_write32(struct mali_device *device, uint32_t offset, uint32_t value);
void mali_write64(struct mali_device *device, uint32_t offset, uint64_t value);

/* The GPU control steps of mali-gpu.c. */
void mali_gpu_read_identity(struct mali_device *device);
const char *mali_gpu_model(const struct mali_device *device);
int mali_gpu_soft_reset(struct mali_device *device);
int mali_gpu_l2_power_on(struct mali_device *device);
void mali_gpu_l2_power_off(struct mali_device *device);
int mali_gpu_flush_caches(struct mali_device *device, unsigned l2, unsigned load_store, unsigned other);
int mali_gpu_irq_start(struct mali_device *device);
void mali_gpu_report_events(struct mali_device *device);
int mali_gpu_wait_raw(struct mali_device *device, uint32_t block, uint32_t mask, unsigned milliseconds);

/* The memory and the address spaces of mali-mmu.c. */
int mali_buffer_alloc(struct mali_buffer *buffer, uint64_t bytes);
void mali_buffer_free(struct mali_buffer *buffer);
int mali_as_init(struct mali_device *device, struct mali_address_space *space, unsigned slot);
int mali_as_map(struct mali_device *device, struct mali_address_space *space, uint64_t gpu_address, struct mali_buffer *buffer, unsigned flags);
int mali_as_enable(struct mali_device *device, struct mali_address_space *space);
void mali_as_disable(struct mali_device *device, struct mali_address_space *space);
int mali_as_unmap(struct mali_device *device, struct mali_address_space *space, uint64_t gpu_address, uint64_t bytes);
void mali_as_destroy(struct mali_address_space *space);
int mali_as_flush(struct mali_device *device, struct mali_address_space *space, uint64_t gpu_address, uint64_t bytes);
int mali_mcu_alloc(struct mali_device *device, struct mali_buffer *buffer, uint64_t bytes, unsigned flags);
int mali_group_alloc(struct mali_device *device, struct mali_buffer *buffer, uint64_t bytes, unsigned flags);

/* The firmware file of mali-firmware-file.c. */
int mali_firmware_read(const char *path, size_t limit, uint8_t **bytes, size_t *size);

/* The command stream front end of mali-csf.c. */
int mali_csf_load(struct mali_device *device, const uint8_t *bytes, size_t size);
int mali_csf_start(struct mali_device *device);
int mali_csf_ping(struct mali_device *device);
void mali_csf_ring_doorbell(struct mali_device *device, unsigned doorbell);
void mali_csf_ring_groups(struct mali_device *device, uint32_t groups);
int mali_csf_wait_global(struct mali_device *device, uint32_t mask, unsigned milliseconds);
int mali_csf_wait_group(struct mali_device *device, unsigned slot, uint32_t mask, unsigned milliseconds);
void mali_csf_report_status(struct mali_device *device);

/* The streams of mali-queue.c. */
int mali_queue_start(struct mali_device *device);
int mali_queue_open(struct mali_device *device, struct mali_queue *queue);
int mali_queue_stop(struct mali_device *device, struct mali_queue *queue);
int mali_queue_push(struct mali_device *device, struct mali_queue *queue, const uint64_t *words, uint32_t count);
unsigned mali_queue_scratch(const struct mali_queue *queue);
int mali_queue_wait(struct mali_device *device, struct mali_queue *queue, const volatile uint64_t *sync, uint64_t target, unsigned milliseconds, struct mali_queue_fault *fault);
void mali_queue_report(struct mali_device *device, struct mali_queue *queue, const char *what);

/* The render node of render.c, over the running firmware. */
int mali_render_start(struct mali_device *device);

/* The S7D's glue of mali-s7d.c. */
int mali_s7d_glue(const struct drv_fdt *fdt, uint32_t gpu_node, struct mali_glue *glue);

/* The display node of display/display.c, on the boot framebuffer and the VPU's first OSD. */
int mali_display_attach(const struct drv_fdt *fdt);

#endif
