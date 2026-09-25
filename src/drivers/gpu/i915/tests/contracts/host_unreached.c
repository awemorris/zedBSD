/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel and device calls the contract tests link against but never reach.
 *
 * The production files under test also hold the paths that reach the real
 * device: the mapped register window, the kernel DMA and PCI devices, the
 * HAL interrupt allocator, the page manager, the GT objects and the
 * monotonic counter.  The contract tests drive the mock operations instead,
 * so every one of these calls is a test bug: it names itself and stops the
 * program rather than inventing a hardware answer.
 */

#include "contract.h"

#include "../../memory.h"

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/pmem.h>

#include <stdio.h>
#include <stdlib.h>

static void host_unreached(const char *name) __attribute__((noreturn));

/*
 * Stands in for an uncached 32-bit register read.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	UNUSED_PARAMETER(address);

	/* The mock register file replaces the window. */
	host_unreached("kern_mmio_read32");
}

/*
 * Stands in for an uncached 64-bit register read.
 */
uint64_t
kern_mmio_read64(
	const volatile void *address)
{
	UNUSED_PARAMETER(address);

	/* No GGTT table is mapped on the host. */
	host_unreached("kern_mmio_read64");
}

/*
 * Stands in for an uncached 32-bit register write.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(value);

	/* The mock register file replaces the window. */
	host_unreached("kern_mmio_write32");
}

/*
 * Stands in for an uncached 64-bit register write.
 */
void
kern_mmio_write64(
	volatile void *address,
	uint64_t value)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(value);

	/* No GGTT table is mapped on the host. */
	host_unreached("kern_mmio_write64");
}

/*
 * Stands in for the device write barrier.
 */
void
kern_io_write_barrier(void)
{
	/* Only a GGTT or PPGTT table write needs it. */
	host_unreached("kern_io_write_barrier");
}

/*
 * Stands in for the compiler barrier of the delay loops.
 */
void
kern_compiler_barrier(void)
{
	/* The delays and register waits are not part of the contract tests. */
	host_unreached("kern_compiler_barrier");
}

/*
 * Stands in for the monotonic counter.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(frequency_hz);

	/* The delays and register waits are not part of the contract tests. */
	host_unreached("kern_rtc_read_counter");
}

/*
 * Stands in for the tick deadline arithmetic.
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	UNUSED_PARAMETER(now);
	UNUSED_PARAMETER(delta);
	UNUSED_PARAMETER(deadline);

	/* Only the delayed works and the slow register wait compute deadlines. */
	host_unreached("kern_deadline_after");
}

/*
 * Stands in for the kernel DMA device's address width.
 */
unsigned
drv_dma_device_address_bits(
	const struct drv_dma_device *device)
{
	UNUSED_PARAMETER(device);

	/* The mock address producer replaces the kernel DMA device. */
	host_unreached("drv_dma_device_address_bits");
}

/*
 * Stands in for the PCI address of the kernel PCI device.
 */
void
drv_pci_device_address(
	const struct drv_pci_device *d,
	struct drv_pci_address *a)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(a);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_address");
}

/*
 * Stands in for an 8-bit configuration read of the kernel PCI device.
 */
int
drv_pci_device_config_read8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t *v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_read8");
}

/*
 * Stands in for a 16-bit configuration read of the kernel PCI device.
 */
int
drv_pci_device_config_read16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t *v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_read16");
}

/*
 * Stands in for a 32-bit configuration read of the kernel PCI device.
 */
int
drv_pci_device_config_read32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t *v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_read32");
}

/*
 * Stands in for an 8-bit configuration write of the kernel PCI device.
 */
int
drv_pci_device_config_write8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_write8");
}

/*
 * Stands in for a 16-bit configuration write of the kernel PCI device.
 */
int
drv_pci_device_config_write16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_write16");
}

/*
 * Stands in for a 32-bit configuration write of the kernel PCI device.
 */
int
drv_pci_device_config_write32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t v)
{
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(v);

	/* The mock PCI function replaces the kernel PCI device. */
	host_unreached("drv_pci_device_config_write32");
}

/*
 * Stands in for the HAL MSI vector allocation.
 */
int
hal_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);

	/* The mock PCI function hands out its own vectors. */
	host_unreached("hal_irq_alloc_msi");
}

/*
 * Stands in for the HAL MSI vector release.
 */
int
hal_irq_free_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* The mock PCI function takes its own vectors back. */
	host_unreached("hal_irq_free_msi");
}

/*
 * Stands in for the kernel heap's zeroed allocation.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(size);

	/* Only the PPGTT construction allocates. */
	host_unreached("kern_calloc");
}

/*
 * Stands in for the kernel heap's release.
 */
void
kern_free(
	void *pointer)
{
	UNUSED_PARAMETER(pointer);

	/* Only the PPGTT teardown frees. */
	host_unreached("kern_free");
}

/*
 * Stands in for the page manager's bounded allocation.
 */
int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(alignment);
	UNUSED_PARAMETER(max_address);
	UNUSED_PARAMETER(boundary);
	UNUSED_PARAMETER(run);

	/* Only the PPGTT tables take pages. */
	host_unreached("kern_pmem_alloc_limited");
}

/*
 * Stands in for the page manager's release.
 */
int
kern_pmem_free(
	struct kern_pmem *run)
{
	UNUSED_PARAMETER(run);

	/* Only the PPGTT tables give pages back. */
	host_unreached("kern_pmem_free");
}

/*
 * Stands in for the direct map of physical RAM.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	UNUSED_PARAMETER(address);

	/* Only the PPGTT tables are written through the direct map. */
	host_unreached("kern_pmem_to_kernel");
}

/*
 * Stands in for the GT object allocation.
 */
struct i915_gt_object *
drv_i915_gt_object_create(
	struct i915_gt_mem *gm,
	uint32_t bytes)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(bytes);

	/* The encoders under test allocate no object. */
	host_unreached("drv_i915_gt_object_create");
}

/*
 * Stands in for the GT object release.
 */
void
drv_i915_gt_object_destroy(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(o);

	/* The encoders under test allocate no object. */
	host_unreached("drv_i915_gt_object_destroy");
}

/*
 * Stands in for the DMA address of a GT object page.
 */
int
drv_i915_gt_object_page_dma(
	const struct i915_gt_object *o,
	unsigned page,
	uint64_t *dma_out)
{
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(page);
	UNUSED_PARAMETER(dma_out);

	/* The encoders under test are given their DMA addresses directly. */
	host_unreached("drv_i915_gt_object_page_dma");
}

/*
 * Stands in for the cache-line flush of a table.
 */
void
drv_i915_gt_clflush(
	const volatile void *address,
	size_t bytes)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(bytes);

	/* Only a PPGTT table write flushes. */
	host_unreached("drv_i915_gt_clflush");
}

/* Names a call the contract tests must never make and stops the program. */
static void
host_unreached(
	const char *name)
{
	/* A reached stub means a test drove a real-device path. */
	printf("host: unreached kernel call %s\n", name);
	fflush(stdout);
	abort();
}
