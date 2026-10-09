/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual hardware owner runs against bounded IO, cache and allocation models. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kmem.h>
#include <uapi/gpu-allocation.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/render-device.h"

/* Persistent single-thread fixture; no allocation or IRQ owner can disappear. */
static struct bcm2711_v3d test_engine;
static uint32_t test_hub[0x4000 / 4];
static uint32_t test_core[0x4000 / 4];
static struct bcm2711_buffer test_buffers[32];
static unsigned test_allocated;
static unsigned test_released;
static unsigned test_fail_allocation;
static uint32_t test_stuck_hub;
static uint32_t test_stuck_core;
static bool test_accessible;
static bool test_reset_failed;
static unsigned test_waits;
static unsigned test_unmasks;
static const char *test_stop;

/* Deferred native queue execution lets actual IRQ service run outside publication spin. */
static unsigned test_pending;
static bool test_bin_done;
static bool test_oom;
static bool test_hang;
static bool test_job_fault;
static bool test_drop_store;
static unsigned test_invalidations;
static bool test_forced_loop;

/* Literal write trace distinguishes cache/barrier/IO publication and source bank. */
static uint32_t test_offsets[256];
static uint32_t test_values[256];
static unsigned test_writes;

/* Captures the actual native renderer node and common error publications. */
static const struct drv_gpu_ops *test_operations;
static struct bcm2711_render_device *test_controller;
static unsigned test_reported;

static void fixture(void);
static void registers_reset(void);
static void trace(uint32_t offset, uint32_t data);
static unsigned find_write(uint32_t offset, unsigned from);
static void check_jobs(void);
static void check_resources(void);
static void model_clear(void);
static void model_tfu(void);
static void *gpu_pointer(uint32_t address);
static uint32_t packet_word(const uint8_t *bytes);

/*
 * Checks real MMU/cache/IRQ/reset control flow and failed-operation retention.
 * The model supplies no proof of physical timing, SMP or GPU DMA execution.
 */
int
main(
	void)
{
	struct bcm2711_v3d_events events;
	struct bcm2711_buffer *pages;
	struct bcm2711_buffer *scratch;
	uint32_t *table;
	unsigned first;
	unsigned next;
	unsigned allocations;
	unsigned releases;
	unsigned writes;
	bool handled;
	int error;

	/* Native power admission prevents even identification reads of a stopped domain. */
	fixture();
	test_engine.power.ready = false;
	test_accessible = false;
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ENODEV && !test_engine.hardware.initialized && test_writes == 0);

	/* Identification accepts actual layout fields, not an exact synthetic whole ID. */
	fixture();
	test_hub[0x0c / 4] = 0x000e1134;
	allocations = test_allocated;
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ENOTSUP && test_allocated == allocations && test_unmasks == 0);

	/* Failure of scratch allocation safely releases the still-unpublished table owner. */
	fixture();
	test_fail_allocation = test_allocated + 2;
	releases = test_released;
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ENOMEM && test_released == releases + 1);
	assert(test_engine.hardware.pages == NULL && !test_engine.hardware.mmu_published);

	/* Full startup uses zeroed 4 MiB storage and native PFNs, never bus aliases. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0 && test_engine.hardware.ready && test_unmasks == 1);
	pages = test_engine.hardware.pages;
	scratch = test_engine.hardware.scratch;
	table = pages->address;
	assert(pages->bytes == 0x400000 && scratch->bytes == 4096);
	assert(table[0] == 0 && table[0xfffff] == 0);
	assert(test_hub[0x1204 / 4] == pages->memory.paddr >> 12);
	assert(test_hub[0x1230 / 4] == ((scratch->memory.paddr >> 12) | 0x80000000));
	first = find_write(0x11204, 0);
	assert(test_offsets[first + 1] == 0x11200 && test_values[first + 1] == 0x060d0c01);
	assert(test_offsets[first + 2] == 0x11230);
	assert(test_offsets[first + 3] == 0x11000 && test_values[first + 3] == 1);
	assert(test_offsets[first + 4] == 0x11000 && test_values[first + 4] == 3);
	assert(test_offsets[first + 5] == 0x11200 && test_values[first + 5] == 0x060d0c05);
	assert(test_core[0x5c / 4] == ~0xa7U && test_hub[0x5c / 4] == ~0x3aU);
	assert(test_core[0x34 / 4] == 0 && test_core[0x38 / 4] == UINT32_MAX);

	/* Every software PTE edit reaches RAM before MMUC flush and then TLB clear. */
	test_writes = 0;
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x800000, 4096);
	assert(error == 0 && table[1] == 0x30000800 && table[0] == 0);
	error = bcm2711_v3d_hardware_pages_sync(&test_engine);
	assert(error == 0 && test_offsets[0] == 0x20000 && test_offsets[1] == 0x20001);
	assert(test_offsets[2] == 0x11000 && test_values[2] == 3);
	assert(test_offsets[3] == 0x11200 && (test_values[3] & 4) != 0);

	/* 4.2 invalidates only L2T then slices; clean drains the TMU combiner first. */
	test_writes = 0;
	error = bcm2711_v3d_hardware_invalidate(&test_engine);
	assert(error == 0 && test_offsets[0] == 0x30 && test_values[0] == 1);
	assert(test_offsets[1] == 0x24 && test_values[1] == 0x0f0f0f0f);
	test_writes = 0;
	error = bcm2711_v3d_hardware_clean(&test_engine);
	assert(error == 0 && test_offsets[0] == 0x30 && test_values[0] == 0x100);
	assert(test_offsets[1] == 0x30 && test_values[1] == 5);

	/* Shared IRQ delivery consumes core and hub sources together before generic EOI. */
	test_core[0x50 / 4] = 2;
	test_hub[0x50 / 4] = 2;
	handled = test_engine.irq.service(test_engine.irq.owner);
	assert(handled && test_core[0x50 / 4] == 0 && test_hub[0x50 / 4] == 0);
	bcm2711_v3d_hardware_events(&test_engine, &events);
	assert(events.core == 2 && events.hub == 2 && events.error == 0);
	bcm2711_v3d_hardware_events(&test_engine, &events);
	assert(events.core == 0 && events.hub == 0);

	/* A simultaneous MMU fault remains sticky even when a completion arrived too. */
	test_core[0x50 / 4] = 1;
	test_hub[0x50 / 4] = 0x12;
	test_hub[0x122c / 4] = 0x81;
	test_hub[0x1234 / 4] = 0x12345000;
	handled = test_engine.irq.service(test_engine.irq.owner);
	assert(handled && !test_engine.hardware.irq_live && test_engine.hardware.faulted);
	bcm2711_v3d_hardware_events(&test_engine, &events);
	assert(events.core == 1 && events.hub == 0x12 && events.error == EIO);
	assert(events.fault_client == 0x81 && events.fault_address == 0x12345000);
	error = bcm2711_v3d_hardware_invalidate(&test_engine);
	assert(error == EIO);

	/* Successful reset joins IRQ service and restores the same retained allocations. */
	allocations = test_allocated;
	releases = test_released;
	error = bcm2711_v3d_hardware_reset(&test_engine);
	assert(error == 0 && test_engine.hardware.ready && test_engine.hardware.resets == 1);
	assert(!test_engine.hardware.faulted && test_allocated == allocations);
	assert(test_released == releases && pages == test_engine.hardware.pages);
	assert(scratch == test_engine.hardware.scratch && table[1] == 0x30000800);
	bcm2711_v3d_hardware_events(&test_engine, &events);
	assert(events.core == 0 && events.hub == 0 && events.error == 0);

	/* Failed ASB reset closes admission and cannot free or read the uncertain domain. */
	test_reset_failed = true;
	error = bcm2711_v3d_hardware_reset(&test_engine);
	assert(error == ETIMEDOUT && !test_engine.hardware.ready);
	assert(test_released == releases && test_engine.hardware.resets == 1);
	writes = test_writes;
	handled = test_engine.irq.service(test_engine.irq.owner);
	assert(!handled && writes == test_writes);
	bcm2711_v3d_hardware_mask(&test_engine);
	assert(writes == test_writes);

	/* A MMUC timeout retains both published owners and never opens GIC delivery. */
	fixture();
	test_stuck_hub = 0x1000;
	releases = test_released;
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ETIMEDOUT && test_engine.hardware.mmu_published);
	assert(test_engine.hardware.faulted && !test_engine.hardware.ready);
	assert(test_unmasks == 0 && test_released == releases && test_waits == 10000);

	/* TLB failure has the same ownership rule after the separate MMUC succeeded. */
	fixture();
	test_stuck_hub = 0x1200;
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ETIMEDOUT && test_engine.hardware.mmu_published && test_unmasks == 0);
	first = find_write(0x11000, 0);
	next = find_write(0x11200, first);
	assert(next > first && test_waits == 10000);

	/* Cache-clean timeout closes submission without releasing hardware storage. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	test_stuck_core = 0x30;
	error = bcm2711_v3d_hardware_clean(&test_engine);
	assert(error == ETIMEDOUT && test_engine.hardware.faulted && !test_engine.hardware.ready);
	assert(test_engine.hardware.pages->references == 1 && test_waits == 10000);

	/* The native V6 stop keeps source masks closed while retaining installed MMU owners. */
	fixture();
	test_stop = "V6";
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == ECANCELED && test_engine.hardware.mmu_published && test_unmasks == 0);
	assert(test_core[0x5c / 4] == UINT32_MAX && test_hub[0x5c / 4] == UINT32_MAX);

	/* The same actual MMU/cache/IRQ owner now executes native job submission paths. */
	check_jobs();

	/* Exercises real renderer callbacks and quarantined native VA ownership after boot work. */
	check_resources();

	/* Succeeded: ordinary and failed paths obey literal native ownership boundaries. */
	puts("v3d-hardware-host-test PASS");
	return 0;
}

/*
 * Allocates private model RAM using the same low, contiguous placement contract.
 */
int
bcm2711_buffer_create(
	uint64_t bytes,
	uint64_t limit,
	size_t alignment,
	struct bcm2711_buffer **result)
{
	struct bcm2711_buffer *buffer;

	/* Allocation failure occurs before any model owner is published. */
	assert(limit == 0x3fffffff && alignment == 4096 && test_allocated < 32);
	*result = NULL;
	if (test_allocated + 1 == test_fail_allocation)
		return ENOMEM;

	/* Storage is page-rounded, private and retained for the fixture process lifetime. */
	buffer = &test_buffers[test_allocated++];
	memset(buffer, 0, sizeof(*buffer));
	buffer->bytes = bytes;
	buffer->memory.size = (size_t)bytes;
	buffer->memory.paddr = 0x1000000 + test_allocated * 0x800000;
	buffer->references = 1;
	buffer->address = calloc(1, (size_t)bytes);
	assert(buffer->address != NULL);
	*result = buffer;

	/* Succeeded: one model owner retains the independent zeroed storage. */
	return 0;
}

/*
 * Records safe, unpublished allocation unwind; no fixture memory is recycled.
 */
void
bcm2711_buffer_release(
	struct bcm2711_buffer *buffer)
{
	/* Published PT storage must never be released by a failure or reset path. */
	assert(buffer->references != 0);
	buffer->references--;
	if (buffer->references != 0)
		return;
	assert(buffer != test_engine.hardware.scratch);
	if (buffer == test_engine.hardware.pages)
		assert(!test_engine.hardware.mmu_published);
	test_released++;
}

/*
 * Supplies native identification and immediate self-clearing command behavior.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	/* Reads after a failed reset would access an unproved power/clock domain. */
	assert(test_accessible);
	return *(const volatile uint32_t *)address;
}

/*
 * Models W1C sources, IRQ masks and command completion independently of the driver.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t data)
{
	volatile uint32_t *registers;
	uintptr_t pointer;
	uint32_t offset;
	bool hub;
	const uint8_t *bin;
	uint32_t branch;

	/* Every write belongs to one bounded, powered source bank. */
	assert(test_accessible);
	pointer = (uintptr_t)address;
	hub = false;
	registers = test_core;
	if (pointer >= (uintptr_t)test_hub && pointer < (uintptr_t)test_hub + sizeof(test_hub)) {
		hub = true;
		registers = test_hub;
	}

	/* Literal traces include the bank, so core and hub acknowledgements cannot alias. */
	assert(pointer >= (uintptr_t)registers && pointer < (uintptr_t)registers + sizeof(test_core));
	offset = (uint32_t)(pointer - (uintptr_t)registers);
	if (hub)
		trace(0x10000 + offset, data);
	else
		trace(offset, data);

	/* Ownership forbids old-generation L2C/GCA commands on the 4.2 core. */
	assert(hub || offset != 0x20);
	if (offset == 0x58) {
		registers[0x50 / 4] &= ~data;
	} else if (offset == 0x60) {
		registers[0x5c / 4] |= data;
	} else if (offset == 0x64) {
		registers[0x5c / 4] &= ~data;
	} else if (hub && offset == 0x1000) {
		registers[offset / 4] = data & ~2U;
		if (test_stuck_hub == offset)
			registers[offset / 4] |= 4;
	} else if (hub && offset == 0x1200) {
		registers[offset / 4] = data & ~4U;
		if (test_stuck_hub == offset)
			registers[offset / 4] |= 0x80;
	} else if (!hub && offset == 0x30) {
		registers[offset / 4] = 0;
		if (test_stuck_core == offset)
			registers[offset / 4] = data;
	} else {
		registers[offset / 4] = data;
	}

	/* Queue launch is deferred until a worker wait, never delivered recursively under spin. */
	if (!hub && offset == 0x168) {
		test_pending = 1;
		if (data - registers[0x160 / 4] == 5) {
			bin = gpu_pointer(registers[0x160 / 4]);
			branch = packet_word(bin + 1);
			assert(bin[0] == 16 && branch == registers[0x160 / 4]);
			test_forced_loop = true;
		}
	}

	/* Render launch is legal only after the modeled binner delivered completion. */
	if (!hub && offset == 0x16c) {
		assert(test_bin_done);
		test_pending = 2;
	}

	/* TFU and CSD each launch only on their final configuration word. */
	if (hub && offset == 0x408)
		test_pending = 3;
	if (!hub && offset == 0x904)
		test_pending = 4;
}

/*
 * Detects waits under spin and records the model's finite command poll bound.
 */
void
kern_usleep_range(
	unsigned minimum,
	unsigned maximum)
{
	bool handled;
	unsigned pending;

	/* IRQ publication never borrows sleeping cache/MMU operations. */
	assert(test_engine.hardware.guard.held.value == 0);
	assert(minimum == 10 && maximum == 10);
	test_waits++;

	/* Native queue events occur only after actual launch and outside the worker spin. */
	if (test_pending == 0 || test_hang || test_forced_loop)
		return;
	pending = test_pending;
	if (pending == 1 && test_oom) {
		/* Early OOM keeps the binner active until the worker supplies mapped storage. */
		test_core[0x50 / 4] |= 4;
		test_oom = false;
	} else {
		/* Actual completion updates the hardware counter before delivering its event. */
		if (pending == 1) {
			test_bin_done = true;
			test_core[0x134 / 4]++;
			test_core[0x50 / 4] |= 2;
		} else if (pending == 2) {
			/* The model writes bytes before render IRQ; the real output still needs hardware proof. */
			if (!test_drop_store)
				model_clear();
			test_core[0x138 / 4]++;
			test_core[0x50 / 4] |= 1;
		} else if (pending == 3) {
			model_tfu();
			test_hub[0x50 / 4] |= 2;
		} else {
			test_core[0x50 / 4] |= 0x80;
		}

		/* The completed queue no longer borrows input or output mappings in this model. */
		test_pending = 0;
	}

	/* A simultaneous fault must outrank any queue-completion bit in the same IRQ. */
	if (test_job_fault)
		test_hub[0x50 / 4] |= 0x10;
	handled = test_engine.irq.service(test_engine.irq.owner);
	assert(handled);
}

/*
 * Checks CPU cache publication before any native MMU/cache wait can start.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t bytes)
{
	/* Clean reaches the entire actual table or scratch allocation, outside spin. */
	assert(test_engine.hardware.guard.held.value == 0 && address != NULL);
	assert(bytes == 0x400000 || bytes == 0x200000 || bytes == 4096 || bytes == 256);
	trace(0x20000, (uint32_t)bytes);
}

/*
 * Checks output invalidation occurs after actual modeled render completion.
 */
void
kern_dcache_invalidate_range(
	const void *address,
	size_t bytes)
{
	/* No output cache maintenance is permitted while DMA or an IRQ spin guard is active. */
	assert(test_engine.hardware.guard.held.value == 0 && test_pending == 0);
	assert(address != NULL && (bytes == 16384 || bytes == 256));
	test_invalidations++;
}

/*
 * Records ordering after cache clean and before device publication.
 */
void
kern_io_write_barrier(
	void)
{
	/* A barrier cannot replace the earlier cache publication event. */
	trace(0x20001, 0);
}

/*
 * Records ordering between a completed cache clean and CPU observation.
 */
void
kern_io_read_barrier(
	void)
{
	/* The host trace supplies ordering checks, not an architecture cache proof. */
	trace(0x20002, 0);
}

/*
 * Checks that source masks and MMU state precede first GIC admission.
 */
void
kern_irq_unmask(
	int irq)
{
	/* The real driver must have installed a persistent callback before delivery. */
	assert(irq == 106 && test_engine.irq.owner == &test_engine);
	assert(test_engine.hardware.irq_live && test_engine.irq.service != NULL);
	assert((test_hub[0x1200 / 4] & 1) != 0);
	test_unmasks++;
}

/*
 * Models GIC closure without manipulating a native device register.
 */
void
kern_irq_mask(
	int irq)
{
	/* Even a failed provider reset may safely mask the independent GIC. */
	assert(irq == 106);
}

/*
 * Models successful reset or failed ASB stop after IRQ access has been joined.
 */
int
bcm2711_v3d_power_reset(
	struct bcm2711_v3d *engine)
{
	/* The power provider is called without spin and with every IRQ admission closed. */
	assert(engine == &test_engine && !engine->hardware.irq_live);
	assert(engine->hardware.guard.held.value == 0);
	if (test_reset_failed) {
		test_accessible = false;
		engine->power.ready = false;
		return ETIMEDOUT;
	}

	/* A real successful reset loses volatile engine state, but not the retained RAM. */
	registers_reset();
	test_pending = 0;
	test_forced_loop = false;
	engine->power.ready = true;
	test_accessible = true;

	/* Succeeded: the hardware owner must reestablish all MMU/cache/IRQ state. */
	return 0;
}

/*
 * Honors the real staged startup gate without injecting a test switch in production.
 */
bool
bcm2711_stage_allowed(
	const char *family,
	const char *stage)
{
	int same;

	/* The fixture supplies the ordinary boot parameter decision. */
	assert(strcmp(family, "v3d") == 0);
	if (test_stop == NULL)
		return true;
	same = strcmp(stage, test_stop);
	if (same >= 0)
		return false;

	/* Earlier stages remain authorized before the requested native stop. */
	return true;
}

/*
 * Consumes diagnostics without representing model logs as hardware evidence.
 */
void
bcm2711_stage_mark(
	const char *family,
	const char *format,
	...)
{
	/* Model diagnostics do not contribute to any physical acceptance claim. */
	assert(family != NULL && format != NULL);
}

/*
 * Accepts the production diagnostic pause point without running a real boot delay.
 */
void
bcm2711_stage_pause(
	const char *family,
	const char *stage)
{
	int family_same;

	/* The normal staged path owns this pause; no test-only production switch exists. */
	family_same = strcmp(family, "v3d");
	assert(family_same == 0);
	assert(stage[0] == 'V');
}

/*
 * Retains model allocations for resource, export and native VA ownership.
 */
void
bcm2711_buffer_retain(
	struct bcm2711_buffer *buffer)
{
	/* Every new reference starts from already retained model storage. */
	assert(buffer->references != 0);
	buffer->references++;
}

/*
 * Allocates ordinary host descriptors used by the actual renderer and sharing source.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *storage;

	/* Descriptor allocation is independent from the model's retained physical storage. */
	storage = calloc(count, bytes);
	assert(storage != NULL);

	/* Succeeded: the caller owns zeroed ordinary descriptor memory. */
	return storage;
}

/*
 * Frees a safely retired host descriptor without recycling uncertain native RAM.
 */
void
kern_free(
	void *storage)
{
	/* Native view and shared allocation ownership are asserted separately by the fixture. */
	free(storage);
}

/*
 * Supplies observable controller mutex ownership without modeling SMP scheduling.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Initializes the same controller lock before any native renderer node publishes. */
	spin_init(&mutex->guard, rank, name);
	mutex->locked = 0;

	/* Succeeded: the model can detect nested or unbalanced mutex ownership. */
	return 0;
}

/*
 * Acquires a host controller owner and refuses reentrant renderer callbacks.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* Common error publication must occur after controller ownership ends. */
	assert(mutex->locked == 0);
	mutex->locked = 1;
}

/*
 * Releases the exact controller owner acquired by this model callback.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* Every native mapping transaction balances one controller ownership interval. */
	assert(mutex->locked == 1);
	mutex->locked = 0;
}

/*
 * Captures complete real operation tables without simulating common ioctl success.
 */
int
drv_gpu_register(
	const struct drv_gpu_ops *operations,
	void *controller,
	struct drv_gpu_device **result)
{
	/* Storage-only registration must not advertise Vulkan before an executor exists. */
	assert((operations->capabilities & (GPU_CAP_COMMAND | GPU_CAP_CAPSET)) == 0);
	assert(operations->share != NULL && operations->recovery != NULL);
	assert(operations->blob_create_placed != NULL && operations->resource_destroy != NULL);
	test_operations = operations;
	test_controller = controller;
	*result = controller;

	/* Succeeded: the fixture can exercise the actual published renderer callbacks. */
	return 0;
}

/*
 * Records common device loss only after native fault teardown has been armed.
 */
void
drv_gpu_report_error(
	struct drv_gpu_device *device,
	int error)
{
	/* No callback may report an error while uncertain resource destruction can free its storage. */
	assert(device == (struct drv_gpu_device *)test_controller);
	assert(error != 0 && test_controller->mutex.locked == 0);
	assert(test_engine.hardware.faulted);
	test_reported++;
}

/* Resets volatile registers independently of any MMU allocation or IRQ owner. */
static void
registers_reset(
	void)
{
	/* Synthetic identifiers encode the published single-core 4.2 layout only. */
	memset(test_hub, 0, sizeof(test_hub));
	memset(test_core, 0, sizeof(test_core));
	test_hub[0x0c / 4] = 0x000e1124;
	test_hub[0x10 / 4] = 0x100;
	test_hub[0x14 / 4] = 0xe00;
	test_hub[0x1238 / 4] = 0x620;
	test_core[0] = 0x04443356;
	test_core[1] = 0x81001422;
	test_core[2] = 0x40078121;
}

/* Creates one powered fixture while retaining earlier failed owners for process lifetime. */
static void
fixture(
	void)
{
	/* Model reset supplies no inference that prior failed real DMA could be freed. */
	memset(&test_engine, 0, sizeof(test_engine));
	registers_reset();
	test_engine.present = true;
	test_engine.power.ready = true;
	test_engine.hub.mapped = (volatile uint8_t *)test_hub;
	test_engine.core.mapped = (volatile uint8_t *)test_core;
	test_engine.hub.size = sizeof(test_hub);
	test_engine.core.size = sizeof(test_core);
	test_engine.irq.registered = true;
	test_engine.irq.irq = 106;
	test_accessible = true;
	test_reset_failed = false;
	test_fail_allocation = 0;
	test_stuck_hub = 0;
	test_stuck_core = 0;
	test_waits = 0;
	test_unmasks = 0;
	test_writes = 0;
	test_stop = NULL;
	test_pending = 0;
	test_bin_done = false;
	test_oom = false;
	test_hang = false;
	test_job_fault = false;
	test_drop_store = false;
	test_invalidations = 0;
	test_forced_loop = false;
}

/* Checks actual bin/render, overflow, TFU, CSD and timeout retirement semantics. */
static void
check_jobs(
	void)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint32_t *table;
	unsigned first;
	unsigned second;
	unsigned writes;
	int error;

	/* Maps private caller-owned spans before testing any native launch. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	table = test_engine.hardware.pages->address;
	error = bcm2711_v3d_pages_map(table, 0x1000, 0x20000000, 0x100000);
	assert(error == 0);
	error = bcm2711_v3d_hardware_pages_sync(&test_engine);
	assert(error == 0);
	memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl.bin_start = 0x1000;
	job.command.cl.bin_end = 0x100e;
	job.command.cl.render_start = 0x2000;
	job.command.cl.render_end = 0x2038;
	job.command.cl.pool_address = 0x4000;
	job.command.cl.pool_bytes = 0x83000;
	job.command.cl.state_address = 0x3000;
	job.command.cl.state_bytes = 256;
	job.command.cl.overflow_count = 1;
	job.command.cl.overflow[0].address = 0x90000;
	job.command.cl.overflow[0].bytes = 0x40000;

	/* Binner OOM is serviced by the worker from preallocated storage before render. */
	test_writes = 0;
	test_oom = true;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == 0 && result.retired && result.core_events == 7);
	assert(result.overflow_used == 1 && result.bin_counter == 1 && result.render_counter == 1);
	first = find_write(0x168, 0);
	assert(test_offsets[first - 1] == 0x160 && test_values[first] == 0x100e);
	second = find_write(0x308, first);
	assert(test_values[second] == 0x90000 && test_values[second + 1] == 0x40000);
	second = find_write(0x16c, second);
	assert(second > first && test_values[second] == 0x2038);
	assert(!test_engine.hardware.job_busy);

	/* A missing final PTE is refused before launch without borrowing DMA lifetime. */
	table[0x100000 >> 12] = 0;
	job.command.cl.render_end = 0x100001;
	test_writes = 0;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == EFAULT && result.retired && !test_engine.hardware.job_busy);
	assert(test_pending == 0 && test_writes == 2);

	/* TFU programs inputs/outputs before ICFG and omits unused extra coefficients. */
	memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = 0x10000;
	job.command.tfu.input_bytes = 4096;
	job.command.tfu.output = 0x20000;
	job.command.tfu.output_bytes = 4096;
	job.command.tfu.output_format = 3;
	job.command.tfu.stride = 64;
	job.command.tfu.size = 0x00100010;
	job.command.tfu.configuration = 0x20400;
	test_writes = 0;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == 0 && result.retired && result.hub_events == 2);
	first = find_write(0x10408, 0);
	assert(test_values[first] == 0x20401 && test_offsets[first - 1] == 0x10424);
	assert(first == 10);
	assert(test_hub[0x41c / 4] == 0x20018);

	/* CSD places CFG1..6 before CFG0, then cleans TMU outputs after its real IRQ. */
	memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_COMPUTE;
	job.command.compute.configuration[0] = 0x10000;
	job.command.compute.configuration[1] = 0x10000;
	job.command.compute.configuration[2] = 0x10000;
	job.command.compute.configuration[3] = 0x1101;
	job.command.compute.configuration[5] = 0x30007;
	job.command.compute.configuration[6] = 0x31000;
	job.command.compute.shader_bytes = 64;
	job.command.compute.uniform_bytes = 64;
	test_writes = 0;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == 0 && result.retired && result.core_events == 0x80);
	first = find_write(0x904, 0);
	assert(test_offsets[first - 6] == 0x908 && test_offsets[first - 1] == 0x91c);
	second = find_write(0x30, first);
	assert(test_values[second] == 0x100 && test_values[second + 1] == 5);

	/* Fault and completion together leave the launched compute's DMA unretired. */
	test_job_fault = true;
	test_writes = 0;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == EIO && !result.retired && !test_engine.hardware.ready);
	assert(!test_engine.hardware.job_busy);
	test_job_fault = false;
	error = bcm2711_v3d_hardware_reset(&test_engine);
	assert(error == 0);

	/* Stalled CSD times out at 500 ms and never reports a mere launch as completion. */
	test_hang = true;
	test_waits = 0;
	test_writes = 0;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == ETIMEDOUT && !result.retired && test_waits == 50000);
	assert(!test_engine.hardware.ready && !test_engine.hardware.irq_live);
	writes = test_writes;
	error = bcm2711_v3d_job_run(&test_engine, &job, &result);
	assert(error == ENODEV && result.retired && test_writes == writes);

	/* The actual boot diagnostic builds noop packets, executes IRQ waits and removes PTEs. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	error = bcm2711_v3d_diagnostic(&test_engine);
	assert(error == 0 && test_engine.hardware.diagnostic == NULL);
	assert(test_invalidations == 2 && test_engine.hardware.resets == 1);
	table = test_engine.hardware.pages->address;
	assert(table[0x100] == 0 && table[0x2ff] == 0);

	/* A failed native reset retains the diagnostic's actual allocation and VA mapping. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	test_hang = true;
	test_reset_failed = true;
	error = bcm2711_v3d_diagnostic(&test_engine);
	assert(error == ETIMEDOUT && test_engine.hardware.diagnostic != NULL);
	assert(test_engine.hardware.diagnostic->references == 1);
	table = test_engine.hardware.pages->address;
	assert(table[0x100] != 0 && table[0x2ff] != 0);

	/* Missing GPU stores cannot pass the diagnostic merely because render IRQ arrived. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	test_drop_store = true;
	error = bcm2711_v3d_diagnostic(&test_engine);
	assert(error == EIO && test_engine.hardware.diagnostic == NULL);
	assert(test_engine.hardware.resets == 1 && test_invalidations == 1);
}

/* Models one packed 8x8 UIF block independently of the diagnostic's pixel generator. */
static void
model_tfu(
	void)
{
	const uint32_t *input;
	uint32_t *output;
	uint32_t x;
	uint32_t y;
	uint32_t utile;

	/* Register-only job tests intentionally supply no image contents to this output model. */
	if (test_hub[0x420 / 4] != 0x00080008)
		return;
	assert(test_hub[0x414 / 4] == 8);
	assert(test_hub[0x408 / 4] == 0x801);
	assert((test_hub[0x41c / 4] & 63U) == 32);
	input = gpu_pointer(test_hub[0x40c / 4]);
	output = gpu_pointer(test_hub[0x41c / 4] & ~63U);

	/* A UIF block has four 64-byte utiles, each containing a raster 4x4 group. */
	for (y = 0; y < 8; y++) {
		for (x = 0; x < 8; x++) {
			utile = (x / 4U) * 16U + (y / 4U) * 32U;
			output[utile + (y % 4U) * 4U + x % 4U] = input[y * 8U + x];
		}
	}
}

/* Resolves an actual PTE to its independently retained host allocation. */
static void *
gpu_pointer(
	uint32_t address)
{
	const uint32_t *table;
	uint64_t physical;
	struct bcm2711_buffer *buffer;
	unsigned index;

	/* An absent page or freed physical owner cannot silently supply model GPU storage. */
	table = test_engine.hardware.pages->address;
	assert((table[address >> 12] & 0x10000000) != 0);
	physical = ((uint64_t)(table[address >> 12] & 0xffffff) << 12) | (address & 4095U);
	for (index = 0; index < test_allocated; index++) {
		buffer = &test_buffers[index];
		if (physical < buffer->memory.paddr)
			continue;
		if (physical - buffer->memory.paddr >= buffer->bytes)
			continue;
		assert(buffer->references != 0);
		return (uint8_t *)buffer->address + (size_t)(physical - buffer->memory.paddr);
	}

	/* Native model access must name live allocated RAM, never a guessed framebuffer alias. */
	assert(0);
	return NULL;
}

/* Decodes a packet word directly from generated little-endian bytes. */
static uint32_t
packet_word(
	const uint8_t *bytes)
{
	uint32_t word;

	/* Byte decoding remains independent of the C serializer's helper and host endianness. */
	word = bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;

	/* Succeeded: the model consumes the complete packet field. */
	return word;
}

/* Models the audited single-tile clear output, not a physical V3D or shader simulator. */
static void
model_clear(
	void)
{
	const uint8_t *render;
	const uint8_t *tile;
	uint32_t *target;
	uint32_t color;
	uint32_t tile_address;
	uint32_t target_address;
	uint32_t index;

	/* Ordinary host job tests have no real CL bytes and make no output-content claim. */
	if (test_core[0x16c / 4] - test_core[0x164 / 4] != 106)
		return;
	render = gpu_pointer(test_core[0x164 / 4]);
	assert(render[9] == 121 && render[10] == 3 && render[93] == 20);
	color = packet_word(render + 11);
	tile_address = packet_word(render + 94);
	tile = gpu_pointer(tile_address);
	assert(tile[4] == 29);
	target_address = packet_word(tile + 13);
	target = gpu_pointer(target_address);

	/* Output writes precede the IRQ, allowing the actual diagnostic to detect a omitted store. */
	for (index = 0; index < 4096; index++)
		target[index] = color;
}

/* Records bounded literal observations rather than computing expected driver commands. */
static void
trace(
	uint32_t offset,
	uint32_t data)
{
	/* Cache/MMU polling does not append unbounded reads to the publication trace. */
	assert(test_writes < 256);
	test_offsets[test_writes] = offset;
	test_values[test_writes++] = data;
}

/* Locates a literal bank/offset in actual writes, preserving their observed order. */
static unsigned
find_write(
	uint32_t offset,
	unsigned from)
{
	unsigned index;

	/* Missing required publication is a fixture failure, never an inferred success. */
	for (index = from; index < test_writes; index++) {
		if (test_offsets[index] == offset)
			return index;
	}

	/* No matching write may be silently substituted by a neighboring command. */
	assert(0);
	return test_writes;
}

/* Checks real resource sharing and VA quarantine against the actual native MMU owner. */
static void
check_resources(
	void)
{
	struct gpu_blob_create blob;
	struct gpu_info info;
	struct gpu_device_info device;
	struct drv_gpu_mapping mapping;
	struct bcm2711_render_resource *resource;
	struct bcm2711_render_resource *imported;
	struct bcm2711_render_session *first;
	struct bcm2711_render_session *second;
	struct bcm2711_buffer *buffer;
	void *object;
	void *alias;
	void *shared;
	uint32_t identifier;
	uint32_t address;
	unsigned releases;
	int error;

	/* The final fixture starts with a complete native MMU and no boot diagnostic owner. */
	fixture();
	error = bcm2711_v3d_hardware_start(&test_engine);
	assert(error == 0);
	error = bcm2711_render_register(&test_engine);
	assert(error == 0 && test_operations != NULL);
	error = test_operations->open(test_controller, &object);
	assert(error == 0);
	first = object;
	error = test_operations->open(test_controller, &object);
	assert(error == 0);
	second = object;
	error = test_operations->get_info(test_controller, first, &info);
	assert(error == 0 && info.max_resources == 128 && (info.capabilities & GPU_CAP_ALLOCATION_SHARE) != 0);
	error = test_operations->scanout->query_device(test_controller, first, &device);
	assert(error == 0 && device.roles == GPU_DEVICE_RENDER && device.companion_id == 0);

	/* Native blobs publish checked PTEs and independent per-open protocol identities. */
	memset(&blob, 0, sizeof(blob));
	blob.bytes = 8192;
	blob.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE;
	error = test_operations->blob_create(test_controller, first, &blob, &object, &identifier);
	assert(error == 0 && identifier == 1);
	resource = object;
	buffer = resource->view->buffer;
	address = resource->view->address;
	assert(address == 4096 && buffer->references == 1);
	error = test_operations->resource_map(test_controller, first, resource, &mapping);
	assert(error == 0 && mapping.physical == buffer->memory.paddr && mapping.bytes == 8192);
	error = test_operations->share->export_resource(test_controller, first, resource, NULL, &shared);
	assert(error == 0 && buffer->references == 2);
	error = test_operations->share->import_resource(test_controller, second, shared, &alias, &identifier);
	assert(error == 0 && identifier == 1 && buffer->references == 3);
	imported = alias;
	assert(imported->view->address == 12288 && imported->view != resource->view);

	/* Source retirement frees its VA while the export and destination retain the same RAM. */
	test_operations->resource_destroy(test_controller, first, resource);
	assert(buffer->references == 2 && ((uint32_t *)test_engine.hardware.pages->address)[1] == 0);
	test_operations->close(test_controller, first);
	test_operations->share->release(test_controller, shared);
	assert(buffer->references == 1);
	test_operations->resource_destroy(test_controller, second, imported);
	assert(buffer->references == 0 && test_controller->space.views == NULL);

	/* Failed TLB retirement cannot release a descriptor's independent native allocation hold. */
	error = test_operations->blob_create(test_controller, second, &blob, &object, &identifier);
	assert(error == 0 && identifier == 2);
	resource = object;
	buffer = resource->view->buffer;
	releases = test_released;
	test_stuck_hub = 0x1200;
	test_operations->resource_destroy(test_controller, second, resource);
	assert(test_reported == 1 && test_released == releases && buffer->references == 1);
	assert(test_controller->space.views->address == 4096 && test_controller->space.views->quarantined);
	assert(test_controller->space.views->references == 0);
	assert(((uint32_t *)test_engine.hardware.pages->address)[1] == 0);
	error = test_operations->blob_create(test_controller, second, &blob, &object, &identifier);
	assert(error == EIO && object == NULL);
	error = test_operations->recovery->reset(test_controller);
	assert(error == EBUSY && buffer->references == 1);
	test_operations->close(test_controller, second);

	/* Failed native reset retains inaccessible storage and admits no fresh namespace. */
	test_reset_failed = true;
	error = test_operations->recovery->reset(test_controller);
	assert(error == ETIMEDOUT && buffer->references == 1);
	error = test_operations->open(test_controller, &object);
	assert(error == EIO && object == NULL);
	test_reset_failed = false;
	test_stuck_hub = 0;
	error = test_operations->recovery->reset(test_controller);
	assert(error == 0 && buffer->references == 0 && test_controller->space.views == NULL);

	/* Failed initial publication retains its unreturned VA even before a resource became visible. */
	error = test_operations->open(test_controller, &object);
	assert(error == 0);
	first = object;
	test_stuck_hub = 0x1000;
	releases = test_released;
	error = test_operations->blob_create(test_controller, first, &blob, &object, &identifier);
	assert(error == ETIMEDOUT && object == NULL && identifier == 0 && test_reported == 2);
	assert(test_released == releases && test_controller->space.views->address == 4096);
	test_operations->close(test_controller, first);
	test_stuck_hub = 0;
	error = test_operations->recovery->reset(test_controller);
	assert(error == 0 && test_released == releases + 1 && test_controller->space.views == NULL);
}
