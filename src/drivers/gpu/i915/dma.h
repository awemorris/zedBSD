/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * DMA mapping.
 *
 * The single place through which the driver reaches DMA.  It keeps three
 * things apart that are easy to blur:
 *
 *   1. Address kinds are distinct types.  A CPU physical address (a
 *      guest-physical one inside a VM) is mapped to a device DMA address
 *      before it can reach a GPU page table.  Under QEMU with VFIO the two
 *      happen to be numerically equal, so feeding one where the other
 *      belongs goes unnoticed; the types make the compiler refuse it.
 *   2. Each entry point has its own return contract; there is no generic
 *      "nonzero means failure":
 *        drv_i915_dma_set_info       0, or a positive errno
 *        drv_i915_dma_map_sg         the mapped segment count, or 0
 *        drv_i915_dma_map_sgtable    0, or a positive errno
 *        drv_i915_dma_map_page       an address tested with
 *                                    drv_i915_dma_mapping_failed()
 *   3. A mapping is an owned resource with a pin count.  While it is pinned
 *      into a GPU page table it can be neither unmapped nor freed.
 *
 * The address production sits behind an operations table, so the same
 * bookkeeping runs against a mock in the host tests and against the kernel's
 * drv_dma_* device in the driver.  An operation the table leaves out is
 * recorded as unimplemented, never passed off as a refusal by the hardware.
 */

#ifndef DRIVERS_GPU_I915_DMA_H
#define DRIVERS_GPU_I915_DMA_H

#include <stdint.h>

struct i915_trace;

/*
 * The value a failed page mapping reports.
 *
 * It mirrors the dedicated mapping-error test the DMA API requires: a
 * mapping result is never compared with zero or with the CPU address.
 */
#define I915_DMA_MAPPING_ERROR	(~(uint64_t)0)

/*
 * How many device segments one mapping holds.
 *
 * It bounds both the entries a scatter map accepts and the segments the
 * operations may produce for it.
 */
#define I915_DMA_MAX_SEGMENTS	64U

/*
 * How many mappings one device can hold at once.
 *
 * The bound exists because the mapping table is embedded in the device.  Each slot carries I915_DMA_MAX_SEGMENTS
 * segments, about 1 KiB, so 256 slots cost 268 KiB of .bss while the device
 * start maps nothing through the table; 32 keeps room for real use without
 * pushing the kernel image past the physical end the UEFI memory map leaves
 * free.
 */
#define I915_DMA_MAX_MAPPINGS	32U

/* Which way the data of a mapping moves. */
enum i915_dma_direction {
	I915_DMA_TO_DEVICE = 0,
	I915_DMA_FROM_DEVICE,
	I915_DMA_BIDIRECTIONAL
};

/*
 * A CPU physical address: what the page manager hands out.
 *
 * Inside a VM it is guest-physical.  Only the DMA layer consumes it; it
 * never reaches a GPU page table.
 */
typedef struct {
	uint64_t value;
} i915_cpu_phys_t;

/*
 * A device-visible DMA address: what a DMA mapping produces.
 *
 * This is the only kind of address a GPU page table entry may hold.
 */
typedef struct {
	uint64_t value;
} i915_dma_addr_t;

/*
 * A GPU virtual address handed out by the GGTT or PPGTT allocator.
 *
 * State base addresses, kernel start pointers, batches and surfaces are
 * addressed this way.  A CPU virtual address stays an ordinary pointer.
 */
typedef struct {
	uint64_t value;
} i915_gpu_vaddr_t;

/*
 * One physical run the CPU owns, before it is mapped.
 *
 * A caller builds a list of these and hands it to a scatter map.
 */
struct i915_sg_entry {
	/* Where the run starts. */
	i915_cpu_phys_t phys;

	/* How many bytes the run covers. */
	uint32_t length;
};

/*
 * One device DMA run, after mapping.
 *
 * The hardware walk of a mapping reads these; it never sees the entries
 * they were produced from.
 */
struct i915_dma_segment {
	/* Where the device sees the run start. */
	i915_dma_addr_t address;

	/* How many bytes the run covers. */
	uint32_t length;
};

/*
 * One scatter list the device can reach.
 *
 * A slot of the device's mapping table.  A scatter map claims it, an unmap
 * gives it back, and it cannot be given back while a GPU page table holds a
 * pin on it.
 */
struct i915_dma_mapping {
	/* A number no other mapping of the device ever had, for the trace. */
	uint32_t resource_id;

	/* Nonzero while the slot holds a mapping. */
	int in_use;

	/*
	 * How many GPU page tables the mapping is bound into.  While it is
	 * nonzero the unmap is refused, so the device never reaches memory
	 * that has been given back.
	 */
	int pin_count;

	/* Which way the data moves. */
	enum i915_dma_direction direction;

	/*
	 * How many entries the caller mapped.  An unmap gives this count
	 * back, as dma_unmap_sg takes the original entry count rather than
	 * the coalesced one.
	 */
	unsigned orig_nents;

	/* How many segments the mapping produced; the hardware walk uses it. */
	unsigned nents;

	/* The produced segments. */
	struct i915_dma_segment segments[I915_DMA_MAX_SEGMENTS];
};

/*
 * The address production end of DMA.
 *
 * The real device reaches the kernel's drv_dma_* device; a host test
 * substitutes a mock.  An operation left NULL is unimplemented: the mapping
 * layer records that and fails the call instead of pretending the hardware
 * refused it.
 */
struct i915_dma_ops {
	/* The name of the implementation. */
	const char *name;

	/* The width of a device DMA address. */
	unsigned address_bits;

	/* The largest single DMA segment. */
	uint64_t max_segment;

	/* Nonzero when mappings are cache coherent. */
	int coherent;

	/* Sets the streaming and coherent mask and the largest segment; returns 0 or a positive errno. */
	int (*set_info)(void *context, unsigned mask_bits, uint64_t max_segment);

	/*
	 * Maps orig_nents entries into at most out_capacity segments.  Reports
	 * how many segments it produced, which may be fewer than the entries
	 * when runs coalesce, or 0 on failure.  Every produced segment carries
	 * its device address and length.
	 */
	int (*map_sg)(void *context, const struct i915_sg_entry *entries, unsigned orig_nents, struct i915_dma_segment *segments, unsigned out_capacity, enum i915_dma_direction direction);

	/* Gives back what map_sg produced. */
	void (*unmap_sg)(void *context, const struct i915_dma_segment *segments, unsigned nents, unsigned orig_nents, enum i915_dma_direction direction);

	/* Maps one physical run; reports its device address or I915_DMA_MAPPING_ERROR. */
	uint64_t (*map_page)(void *context, uint64_t phys, uint32_t size, enum i915_dma_direction direction);

	/* Gives back what map_page produced. */
	void (*unmap_page)(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);

	/* Hands a run the CPU wrote over to the device. */
	void (*sync_for_device)(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);

	/* Hands a run the device wrote back to the CPU. */
	void (*sync_for_cpu)(void *context, uint64_t address, uint32_t size, enum i915_dma_direction direction);
};

/*
 * The DMA side of one device and the mappings it holds.
 *
 * It lives inside the device from the device start to the device stop.
 * The mapping table is protected by the caller: the device start and the
 * request worker are the only users, and they never run at once.
 */
struct i915_dma {
	/* The address production and the context it is given. */
	const struct i915_dma_ops *ops;
	void *context;

	/* Where mappings and failures are recorded; may be NULL. */
	struct i915_trace *trace;

	/* The mask width and largest segment the last successful set_info accepted. */
	unsigned mask_bits;
	uint64_t max_segment;

	/* Nonzero once set_info has succeeded. */
	int info_set;

	/* The resource number the next mapping receives; it only ever increases. */
	uint32_t next_resource_id;

	/* The mapping slots. */
	struct i915_dma_mapping mappings[I915_DMA_MAX_MAPPINGS];
};

/*
 * Makes a CPU physical address from its value.
 */
static __inline i915_cpu_phys_t
drv_i915_cpu_phys(
	uint64_t value)
{
	i915_cpu_phys_t address;

	/* Wraps the value in its address kind. */
	address.value = value;

	/* Succeeded: the value is now a CPU physical address. */
	return address;
}

/*
 * Makes a device DMA address from its value.
 */
static __inline i915_dma_addr_t
drv_i915_dma_addr(
	uint64_t value)
{
	i915_dma_addr_t address;

	/* Wraps the value in its address kind. */
	address.value = value;

	/* Succeeded: the value is now a device DMA address. */
	return address;
}

/*
 * Makes a GPU virtual address from its value.
 */
static __inline i915_gpu_vaddr_t
drv_i915_gpu_vaddr(
	uint64_t value)
{
	i915_gpu_vaddr_t address;

	/* Wraps the value in its address kind. */
	address.value = value;

	/* Succeeded: the value is now a GPU virtual address. */
	return address;
}

/*
 * Reports the value of a CPU physical address.
 */
static __inline uint64_t
drv_i915_cpu_phys_raw(
	i915_cpu_phys_t address)
{
	/* Succeeded: the bare value, for arithmetic and logs. */
	return address.value;
}

/*
 * Reports the value of a device DMA address.
 */
static __inline uint64_t
drv_i915_dma_addr_raw(
	i915_dma_addr_t address)
{
	/* Succeeded: the bare value, for arithmetic and logs. */
	return address.value;
}

/*
 * Reports the value of a GPU virtual address.
 */
static __inline uint64_t
drv_i915_gpu_vaddr_raw(
	i915_gpu_vaddr_t address)
{
	/* Succeeded: the bare value, for arithmetic and logs. */
	return address.value;
}

/*
 * Reports nonzero when a page mapping failed.
 */
static __inline int
drv_i915_dma_mapping_failed(
	i915_dma_addr_t address)
{
	/* A failed mapping carries the dedicated error value. */
	if (address.value == I915_DMA_MAPPING_ERROR)
		return 1;

	/* The address is a real mapping. */
	return 0;
}

void drv_i915_dma_init(struct i915_dma *dma, const struct i915_dma_ops *ops, void *context, struct i915_trace *trace);
const struct i915_dma_ops *drv_i915_dma_device_ops(void);

int drv_i915_dma_set_info(struct i915_dma *dma, unsigned mask_bits, uint64_t max_segment);
unsigned drv_i915_dma_address_bits(const struct i915_dma *dma);
uint64_t drv_i915_dma_max_segment(const struct i915_dma *dma);
int drv_i915_dma_is_coherent(const struct i915_dma *dma);

int drv_i915_dma_map_sg(struct i915_dma *dma, const struct i915_sg_entry *entries, unsigned orig_nents, enum i915_dma_direction direction, struct i915_dma_mapping **mapping);
int drv_i915_dma_map_sgtable(struct i915_dma *dma, const struct i915_sg_entry *entries, unsigned orig_nents, enum i915_dma_direction direction, struct i915_dma_mapping **mapping);
int drv_i915_dma_unmap_sg(struct i915_dma *dma, struct i915_dma_mapping *mapping);

i915_dma_addr_t drv_i915_dma_map_page(struct i915_dma *dma, i915_cpu_phys_t phys, uint32_t size, enum i915_dma_direction direction);
void drv_i915_dma_unmap_page(struct i915_dma *dma, i915_dma_addr_t address, uint32_t size, enum i915_dma_direction direction);

i915_dma_addr_t drv_i915_dma_pin(struct i915_dma *dma, struct i915_dma_mapping *mapping);
void drv_i915_dma_unpin(struct i915_dma *dma, struct i915_dma_mapping *mapping);

void drv_i915_dma_sync_for_device(struct i915_dma *dma, i915_dma_addr_t address, uint32_t size, enum i915_dma_direction direction);
void drv_i915_dma_sync_for_cpu(struct i915_dma *dma, i915_dma_addr_t address, uint32_t size, enum i915_dma_direction direction);

unsigned drv_i915_dma_live_mappings(const struct i915_dma *dma);

#endif
