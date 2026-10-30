/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/vf2-dmatest (see vf2-dmatest.h).
 *
 * The check takes one 2 MiB run, aligned to 2 MiB, from the controller's
 * DMA owner, assuming coherent DMA, so its cached view is the one a
 * driver uses.  The alias is the same run 0x4_0000_0000 higher, the
 * JH7110's uncached system port, mapped through the device window: only
 * the run's own 2 MiB, never the firmware's or a reserved range.  The
 * window keeps that mapping for good, harmless as it caches nothing.
 * The run is flushed and freed after every check.
 */

#include <drivers/dma/dw-axi-dmac.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <drivers/platform/visionfive2/vf2-dmac.h>
#include <drivers/platform/visionfive2/vf2-dmatest-core.h>
#include <drivers/platform/visionfive2/vf2-dmatest.h>
#include <kern/cdev.h>
#include <kern/dcache.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The device's number (a major of its own) and name. */
#define DMATEST_DEVICE_NUMBER	0x00170000U
#define DMATEST_DEVICE_NAME	"vf2-dmatest"

/* How far above a RAM address the uncached system port sees it. */
#define DMATEST_ALIAS_OFFSET	0x400000000ULL

/* The bus master the answer names. */
#define DMATEST_MASTER		"dmac"

/* Room for the answer's lines. */
#define DMATEST_TEXT		256U

/* One check's memory: the cached view, the alias and the device's address. */
struct dmatest_memory {
	volatile uint64_t *cached;
	volatile uint64_t *alias;
	uint64_t device_address;
	struct drv_dw_axi_dmac_channel *channel;
};

/*
 * The controller, set once by drv_vf2_dmatest_publish() before the device
 * is published and never changed.
 */
static struct drv_dw_axi_dmac *dmatest_controller;

/*
 * Whether a check runs: a reader sets it with a compare-and-swap and
 * clears it when done, so two readers never share the run or the text.
 */
static unsigned dmatest_busy;

/* The last answer; written only by the reader holding dmatest_busy. */
static char dmatest_text[DMATEST_TEXT];

/* The bytes of dmatest_text the last answer used; written with it. */
static size_t dmatest_length;

static ssize_t dmatest_read(struct file *file, void *buffer, size_t size);
static size_t run_check(void);
static uint64_t memory_load(void *context, int view, size_t offset);
static void memory_store(void *context, int view, size_t offset, uint64_t value);
static void memory_flush(void *context, size_t offset, size_t size);
static int memory_copy(void *context, size_t destination, size_t source, size_t size);
static size_t put_failure(const char *reason, int error);

/* The device: a read from the start runs the check. */
static const struct cdev_ops dmatest_ops = {
	.read = dmatest_read,
};

/*
 * Brings up the AXI DMA controller and publishes the device.
 */
int
drv_vf2_dmatest_publish(
	const struct drv_fdt *fdt)
{
	uint32_t node;
	int error;

	/* Finds the controller. */
	error = drv_fdt_find_compatible(fdt, "starfive,jh7110-axi-dma", DRV_FDT_NO_NODE, &node);
	if (error != 0) {
		kern_logf("vf2-dmatest: no AXI DMA controller (%d)\n", error);
		return ENODEV;
	}

	/* Brings it up, assuming coherent DMA so the cached view is a driver's. */
	error = drv_vf2_dmac_get(fdt, node, 1, &dmatest_controller);
	if (error != 0) {
		kern_logf("vf2-dmatest: AXI DMA controller unavailable (%d)\n", error);
		return error;
	}

	/* Publishes the device. */
	error = cdev_register(DMATEST_DEVICE_NAME, (dev_t)DMATEST_DEVICE_NUMBER, &dmatest_ops, NULL);
	if (error != 0) {
		kern_logf("vf2-dmatest: /dev/%s not published (%d)\n", DMATEST_DEVICE_NAME, error);
		return error;
	}

	/* Succeeded: reading the device runs the check. */
	return 0;
}

/*
 * Runs the check on a read from the start, then reads the answer from the
 * file's offset on.  Zero at the end.
 */
static ssize_t
dmatest_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	unsigned expected;
	uint64_t offset;
	size_t length;
	size_t chunk;
	int taken;

	/* One reader at a time. */
	expected = 0;
	taken = __atomic_compare_exchange_n(&dmatest_busy, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (!taken)
		return -EBUSY;

	/* Refuses a negative offset, letting the next reader in. */
	if (file->f_offset < 0) {
		__atomic_store_n(&dmatest_busy, 0U, __ATOMIC_RELEASE);
		return -EINVAL;
	}

	/* A read from the start runs a new check. */
	if (file->f_offset == 0)
		dmatest_length = run_check();

	/* The part of the answer at the offset. */
	offset = (uint64_t)file->f_offset;
	length = dmatest_length;
	chunk = 0;
	if (offset < length) {
		chunk = length - (size_t)offset;
		if (chunk > size)
			chunk = size;
		kern_memcpy(buffer, dmatest_text + offset, chunk);
	}

	/* Moves the file's offset past the bytes read. */
	file->f_offset = (off_t)(offset + chunk);

	/* Lets the next reader in. */
	__atomic_store_n(&dmatest_busy, 0U, __ATOMIC_RELEASE);

	/* Succeeded: the bytes read. */
	return (ssize_t)chunk;
}

/*
 * Takes the run, a channel and the alias, runs the check and writes the
 * answer.  Reports the answer's length.
 */
static size_t
run_check(
	void)
{
	struct drv_vf2_dmatest_result result;
	struct drv_vf2_dmatest_ops ops;
	struct dmatest_memory memory;
	struct drv_dma_buffer run;
	struct drv_dma_device *device;
	void *direct;
	void *alias;
	size_t length;
	int error;

	/* Takes a channel. */
	error = drv_dw_axi_dmac_channel_get(dmatest_controller, &memory.channel);
	if (error != 0) {
		length = put_failure("channel", error);
		return length;
	}

	/* Takes the run from the controller's DMA owner. */
	device = drv_dw_axi_dmac_dma_device(dmatest_controller);
	error = drv_dma_alloc_coherent(device, DRV_VF2_DMATEST_REGION, DRV_VF2_DMATEST_REGION, &run);
	if (error != 0) {
		drv_dw_axi_dmac_channel_put(memory.channel);
		length = put_failure("alloc", error);
		return length;
	}

	/* The alias needs the run's physical address: the device's must be it. */
	direct = kern_pmem_to_kernel((kern_physaddr_t)run.device_address);
	if (direct != run.address) {
		drv_dma_free_coherent(device, &run);
		drv_dw_axi_dmac_channel_put(memory.channel);
		length = put_failure("physical", EINVAL);
		return length;
	}

	/* Maps the run's 2 MiB of the uncached system port. */
	error = kern_device_map(run.device_address + DMATEST_ALIAS_OFFSET,
				DRV_VF2_DMATEST_REGION,
				KERN_DEVICE_UNCACHED,
				&alias);
	if (error != 0) {
		drv_dma_free_coherent(device, &run);
		drv_dw_axi_dmac_channel_put(memory.channel);
		length = put_failure("alias", error);
		return length;
	}

	/* The memory the procedure sees. */
	memory.cached = run.address;
	memory.alias = alias;
	memory.device_address = run.device_address;
	ops.context = &memory;
	ops.load = memory_load;
	ops.store = memory_store;
	ops.flush = memory_flush;
	ops.copy = memory_copy;

	/* Runs the check; it flushes the whole run before it returns. */
	drv_vf2_dmatest_run(&ops, &result);

	/* Gives the run and the channel back; the window keeps the alias. */
	(void)kern_device_unmap(alias, DRV_VF2_DMATEST_REGION);
	drv_dma_free_coherent(device, &run);
	drv_dw_axi_dmac_channel_put(memory.channel);

	/* Writes the answer. */
	length = drv_vf2_dmatest_format(&result, DMATEST_MASTER, dmatest_text, sizeof(dmatest_text));
	if (length >= sizeof(dmatest_text))
		length = sizeof(dmatest_text) - 1U;
	kern_logf("%s", dmatest_text);

	/* Succeeded: the answer's length. */
	return length;
}

/* Loads one word through a view. */
static uint64_t
memory_load(
	void *context,
	int view,
	size_t offset)
{
	struct dmatest_memory *memory;

	/* Reads the cached view or the alias. */
	memory = context;
	if (view == DRV_VF2_DMATEST_ALIAS)
		return memory->alias[offset / 8U];

	/* The cached view. */
	return memory->cached[offset / 8U];
}

/* Stores one word through a view. */
static void
memory_store(
	void *context,
	int view,
	size_t offset,
	uint64_t value)
{
	struct dmatest_memory *memory;

	/* Writes the cached view or the alias. */
	memory = context;
	if (view == DRV_VF2_DMATEST_ALIAS)
		memory->alias[offset / 8U] = value;
	else
		memory->cached[offset / 8U] = value;
}

/* Writes back and discards the cached lines of a range (the L2's FLUSH64). */
static void
memory_flush(
	void *context,
	size_t offset,
	size_t size)
{
	struct dmatest_memory *memory;
	const uint8_t *start;

	/* The range in the cached view. */
	memory = context;
	start = (const uint8_t *)memory->cached + offset;

	/* Writes the dirty lines back, then discards every line. */
	kern_dcache_clean_range(start, size);
	kern_dcache_invalidate_range(start, size);
}

/* Has the controller copy a range of the run and waits for the end. */
static int
memory_copy(
	void *context,
	size_t destination,
	size_t source,
	size_t size)
{
	struct dmatest_memory *memory;
	int error;

	/* Copies with the device's addresses. */
	memory = context;
	error = drv_dw_axi_dmac_copy(memory->channel,
				     memory->device_address + destination,
				     memory->device_address + source,
				     size);
	if (error != 0)
		return error;

	/* Succeeded: the copy ended. */
	return 0;
}

/*
 * Writes the answer of a check that could not start.  Reports its length.
 */
static size_t
put_failure(
	const char *reason,
	int error)
{
	int length;

	/* One line naming what failed. */
	length = kern_snprintf(dmatest_text,
			       sizeof(dmatest_text),
			       "DMATEST " DMATEST_MASTER " FAIL %s error=%d\n",
			       reason,
			       error);
	if (length < 0)
		length = 0;
	if ((size_t)length >= sizeof(dmatest_text))
		length = (int)sizeof(dmatest_text) - 1;
	kern_logf("%s", dmatest_text);

	/* Succeeded: the answer's length. */
	return (size_t)length;
}
