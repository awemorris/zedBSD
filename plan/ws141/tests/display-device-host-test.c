/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual registered callbacks use the native flip/IRQ path and owned copy buffers. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <drivers/gpu/gpu.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* One allocated run lives until its actual last buffer reference is released. */
struct test_allocation {
	uint64_t physical;
	size_t bytes;
	void *address;
};

/* The model retains both private scanout targets and ordinary session resources. */
static struct test_allocation test_allocations[16];
static uint32_t test_hvs[8192];
static uint32_t test_pv[2][32];
static struct bcm2711_display test_display;
static const struct drv_gpu_ops *test_operations;
static void *test_device;
static uint8_t *test_console;
static unsigned test_run_count;
static unsigned test_freed;
static bool test_stall;

/* The clock provider can refuse a transition without permitting any list publication. */
static bool test_clock_refused;
static uint32_t test_core_hz;

/*
 * Verifies the real node callbacks, byte conversion, lease isolation and DMA holds.
 * This host model cannot prove physical output or actual ARM cache ordering.
 */
int
main(
	void)
{
	struct gpu_info info;
	struct gpu_display_info output;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_display_present present;
	struct gpu_display_wait wait;
	struct gpu_resource_create create;
	struct drv_gpu_mapping mapping;
	struct bcm2711_flip_status status;
	struct drv_bcm2711_boot_screen overlay;
	uint32_t *words;
	const uint8_t pixels[4] = {0x11, 0x22, 0x33, 0xff};
	uint8_t *native;
	void *first;
	void *second;
	void *resource;
	uint64_t lease;
	uint64_t sequence;
	unsigned freed_before;
	int error;

	/* Prepares a running native-mode fixture and real IRQ service owners. */
	test_console = calloc(1, 8294400);
	assert(test_console != NULL);
	test_display.compositor.mapped = (volatile uint8_t *)test_hvs;
	test_display.compositor.size = sizeof(test_hvs);
	test_display.compositor_irq.registered = true;
	test_display.timing[0].mapped = (volatile uint8_t *)test_pv[0];
	test_display.timing[1].mapped = (volatile uint8_t *)test_pv[1];
	test_display.timing[0].size = sizeof(test_pv[0]);
	test_display.timing[1].size = sizeof(test_pv[1]);
	test_display.timing_irq[0].registered = true;
	test_display.timing_irq[1].registered = true;
	test_display.screen.physical = 0x01000000;
	test_display.screen.size = 8294400;
	test_display.screen.width = 1920;
	test_display.screen.height = 1080;
	test_display.screen.pitch = 7680;
	test_display.scanout_started = true;
	test_display.refresh_millihz = 60000;
	test_display.max_core_hz = 500000000;
	test_display.console_core_hz = 137600000;
	test_hvs[0x40 / 4] = 0x87800438;
	test_hvs[0x14 / 4] = 0xc0000000;
	test_hvs[0x20 / 4] = 43;
	test_hvs[0x30 / 4] = 43;
	test_pv[0][0] = 1;
	test_pv[0][1] = 1;
	error = bcm2711_display_irq_prepare(&test_display);
	assert(error == 0);
	error = bcm2711_display_register(&test_display);
	assert(error == 0 && test_operations != NULL);
	assert(test_run_count == 2 && test_freed == 0);

	/* Opens two independent sessions through the actual published operation table. */
	error = test_operations->open(test_device, &first);
	assert(error == 0);
	error = test_operations->open(test_device, &second);
	assert(error == 0);
	memset(&info, 0, sizeof(info));
	error = test_operations->get_info(test_device, first, &info);
	assert(error == 0 && (info.capabilities & GPU_CAP_COMMAND) == 0);
	memset(&output, 0, sizeof(output));
	error = test_operations->display->query(test_device, first, &output);
	assert(error == 0 && output.display_id == 1 && output.current_width == 1920);

	/* Only the first open can claim the one actual boot output. */
	memset(&claim, 0, sizeof(claim));
	claim.display_id = 1;
	claim.generation = 1;
	error = test_operations->display->claim(test_device, first, &claim);
	assert(error == 0 && claim.lease != 0);
	lease = claim.lease;
	error = test_operations->display->claim(test_device, second, &claim);
	assert(error == EBUSY);

	/* Allocates source storage and transfers RGBA pixels through the real CPU callbacks. */
	memset(&create, 0, sizeof(create));
	create.bytes = 8294400;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	error = test_operations->resource_create(test_device, first, &create, &resource);
	assert(error == 0);
	error = test_operations->resource_write(test_device, first, resource, 0, pixels, 4);
	assert(error == 0);
	error = test_operations->resource_map(test_device, first, resource, &mapping);
	assert(error == 0 && mapping.bytes == 8294400 && mapping.attributes == 0);

	/* Presentation converts into private native BGRA storage and confirms actual adoption. */
	memset(&present, 0, sizeof(present));
	present.lease = lease;
	present.width = 1920;
	present.height = 1080;
	present.stride = 7680;
	present.format = GPU_PIXEL_RGBA8888;
	present.flags = GPU_DISPLAY_PRESENT_FIFO;
	present.generation = 1;
	test_clock_refused = true;
	error = test_operations->display->present(test_device, first, resource, &present);
	assert(error == EIO && test_hvs[0x20 / 4] == 43 && !test_display.flip.busy);
	test_clock_refused = false;
	error = test_operations->display->present(test_device, first, resource, &present);
	assert(error == 0 && present.sequence == 1);
	sequence = present.sequence;
	bcm2711_display_flip_snapshot(&test_display, &status);
	native = kern_pmem_to_kernel(status.frames[0].physical);
	assert(native[0] == 0x33 && native[1] == 0x22 && native[2] == 0x11 && native[3] == 0xff);
	assert(status.retained_mask == 1 && status.active_list == 128);
	words = &test_hvs[0x4000 / 4 + 128];
	assert(words[0] == 0x4800d807 && words[5] == 0xc1000000);
	assert(words[8] == 0x4800d807 && words[9] == 0 && words[10] == 0x4000fff0);
	assert(words[11] == 0x04380780 && words[13] == (0xc0000000U | (uint32_t)status.frames[0].physical));
	assert(words[12] == 0xc0c0c0c0 && words[14] == 0xc0c0c0c0);
	assert(words[15] == 7680 && words[16] == 0x80000000);
	assert(test_core_hz == 137600000 && status.clock_error == 0);

	/* The source may be destroyed immediately; HVS still references its private copy. */
	freed_before = test_freed;
	test_operations->resource_destroy(test_device, first, resource);
	assert(test_freed == freed_before + 1 && native[0] == 0x33);
	memset(&wait, 0, sizeof(wait));
	wait.lease = lease;
	wait.sequence = sequence;
	error = test_operations->display->wait(test_device, first, &wait);
	assert(error == 0 && wait.completed_sequence == sequence);

	/* A failed restoration retains both the private storage and the retryable lease. */
	memset(&release, 0, sizeof(release));
	release.lease = lease;
	test_stall = true;
	error = test_operations->display->release(test_device, first, &release);
	assert(error == ETIMEDOUT && test_freed == freed_before + 1);
	error = test_operations->display->claim(test_device, second, &claim);
	assert(error == EBUSY);
	test_stall = false;
	error = test_operations->display->release(test_device, first, &release);
	assert(error == 0);
	bcm2711_display_flip_snapshot(&test_display, &status);
	assert(status.retained_mask == 0 && status.active_list == 43);

	/* A fresh lease cannot observe an earlier owner's completed presentation. */
	error = test_operations->display->claim(test_device, second, &claim);
	assert(error == 0 && claim.lease != lease);
	wait.lease = claim.lease;
	wait.sequence = sequence;
	error = test_operations->display->wait(test_device, second, &wait);
	assert(error == ETIMEDOUT);
	test_operations->close(test_device, second);
	test_operations->close(test_device, first);
	assert(test_freed == 1);

	/* Exercises positioned premultiplied composition independently of the full copy API. */
	overlay = test_display.screen;
	overlay.physical = test_allocations[0].physical;
	overlay.width = 32;
	overlay.height = 16;
	overlay.pitch = 128;
	overlay.size = 2048;
	error = bcm2711_display_flip_compose(&test_display, &overlay, 123, 456, true);
	assert(error == 0);
	words = &test_hvs[0x4000 / 4 + 128];
	assert(words[8] == 0x4800d807 && words[9] == 0x01c8007b);
	assert(words[10] == 0x2000fff0 && words[11] == 0x00100020);
	assert(words[13] == 0xc2000000 && words[15] == 128 && words[16] == 0x80000000);
	error = bcm2711_display_flip_restore(&test_display);
	assert(error == 0 && test_core_hz == 137600000);

	/* Succeeded: private scanout storage survives every ordinary open and resource. */
	puts("display-device-host-test PASS");
	return 0;
}

/*
 * Keeps the visible diagnostic pauses bounded without physical wall-clock delay.
 */
void
bcm2711_stage_pause(
	const char *family,
	const char *stage)
{
	/* The real default path remains selected; only the platform time source is modeled. */
	(void)family;
	(void)stage;
}

/*
 * Models core-clock changes and verifies the provider is called outside the IRQ guard.
 */
int
drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered)
{
	/* Firmware operations are serialized with publication, while actual IRQs can run. */
	assert(test_display.flip.busy && test_display.flip.guard.held.value == 0);
	assert(tag == 0x00038002U && request_count == 3 && capacity == 3);
	assert(values[0] == 4 && values[2] == 0);
	if (test_clock_refused)
		return EIO;
	test_core_hz = values[1];
	*answered = 8;

	/* Succeeded: subsequent register writes observe the accepted floor. */
	return 0;
}

/*
 * Captures the common node publication after validating complete callback pairs.
 */
int
drv_gpu_register(
	const struct drv_gpu_ops *operations,
	void *private_data,
	struct drv_gpu_device **result)
{
	/* Registration can borrow only initialized tables and an attached native pipeline. */
	assert(operations->version == DRV_GPU_INTERFACE_VERSION);
	assert(operations->size == sizeof(*operations));
	assert(operations->display != NULL && operations->scanout != NULL);
	assert(test_display.flip.attached);
	test_operations = operations;
	test_device = private_data;
	*result = (struct drv_gpu_device *)private_data;

	/* Succeeded: the model can open the real published callbacks. */
	return 0;
}

/*
 * Allocates host RAM with independently checked physical placement descriptors.
 */
int
kern_pmem_alloc_limited(
	size_t bytes,
	size_t alignment,
	uint64_t limit,
	size_t boundary,
	struct kern_pmem *memory)
{
	struct test_allocation *allocation;

	/* Sequential physical slots are disjoint and satisfy the actual one-GiB request. */
	assert(test_run_count < 16 && alignment == 4096 && boundary == 0);
	allocation = &test_allocations[test_run_count];
	allocation->physical = 0x02000000ULL + test_run_count * 0x02000000ULL;
	assert(allocation->physical + bytes - 1 <= limit);
	allocation->address = calloc(1, bytes);
	assert(allocation->address != NULL);
	allocation->bytes = bytes;
	memory->paddr = allocation->physical;
	memory->size = bytes;
	test_run_count++;

	/* Succeeded: the caller owns one stable mapped run. */
	return 0;
}

/*
 * Frees one exact allocation and rejects stale or mismatched run descriptions.
 */
int
kern_pmem_free(
	struct kern_pmem *memory)
{
	unsigned index;

	/* Resolves the actual allocation by immutable physical identity. */
	for (index = 0; index < test_run_count; index++) {
		/* Only a matching live allocation may be returned to this allocator. */
		if (test_allocations[index].physical != memory->paddr)
			continue;
		assert(test_allocations[index].address != NULL && test_allocations[index].bytes == memory->size);
		free(test_allocations[index].address);
		test_allocations[index].address = NULL;
		memory->size = 0;
		test_freed++;
		return 0;
	}

	/* An unknown allocation is not silently accepted as a physical release. */
	return EINVAL;
}

/*
 * Translates only the persistent console and still-live modeled allocations.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t physical)
{
	unsigned index;

	/* The firmware console remains separately owned throughout the model. */
	if (physical == 0x01000000)
		return test_console;

	/* Every resource mapping is stable until its real final reference disappears. */
	for (index = 0; index < test_run_count; index++) {
		/* Exact base translations suffice for production buffer and flip preparation. */
		if (test_allocations[index].physical == physical)
			return test_allocations[index].address;
	}

	/* Unknown storage cannot grant a usable CPU mapping. */
	return NULL;
}

/*
 * Supplies observable W1C semantics for actual PV/HVS source services.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t word)
{
	unsigned port;

	/* Quieting a source differs from an ordinary register assignment. */
	if (address == &test_hvs[1]) {
		test_hvs[1] &= ~word;
		return;
	}

	/* Clears only the addressed pixelvalve interrupt source. */
	for (port = 0; port < 2; port++) {
		/* The selected output never acknowledges the other port's frame. */
		if (address == &test_pv[port][0x28 / 4]) {
			test_pv[port][0x28 / 4] &= ~word;
			return;
		}
	}

	/* Actual current-list adoption is deferred until the next modeled vblank. */
	if (address == &test_hvs[0x20 / 4])
		assert(test_core_hz == 500000000);

	/* Models ordered register storage after the publication check. */
	*(volatile uint32_t *)address = word;
}

/*
 * Reads the actual persistent model register word.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	/* Registers share storage with independently driven hardware completion. */
	return *(const volatile uint32_t *)address;
}

/*
 * Delivers one native selected-PV interrupt after deferred list adoption.
 */
void
kern_usleep_range(
	unsigned minimum,
	unsigned maximum)
{
	bool handled;

	/* A stalled model makes the production flip or restore reach its bounded timeout. */
	(void)minimum;
	(void)maximum;
	if (test_stall)
		return;
	test_hvs[0x30 / 4] = test_hvs[0x20 / 4];
	test_pv[0][0x28 / 4] = 0x80;
	handled = test_display.timing_irq[0].service(test_display.timing_irq[0].owner);
	assert(handled);
}

/*
 * Verifies that cache publication uses a valid owned CPU allocation.
 */
void
kern_dcache_clean_range(
	const void *address,
	size_t bytes)
{
	/* Production copy and native flip always clean a nonempty persistent run. */
	assert(address != NULL && bytes != 0);
}

/*
 * Keeps architecture ordering outside this CPU-only register model.
 */
void
kern_io_write_barrier(
	void)
{
	/* Source ordering is exercised through actual MMIO calls and deferred IRQ adoption. */
}

/*
 * Supplies heap objects whose ownership is exercised by the actual callbacks.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *address;

	/* Host allocation preserves sanitizer visibility of resource and session lifetimes. */
	address = calloc(count, bytes);

	/* Succeeded: returns the host allocation or its ordinary NULL failure. */
	return address;
}

/*
 * Releases one actual session or descriptor allocation.
 */
void
kern_free(
	void *address)
{
	/* Hardware-facing private copy targets never reach this through ordinary close. */
	free(address);
}

/*
 * Supplies an initialized single-thread controller mutex for callback serialization.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* The separate IRQ guard continues using the strict spin stubs. */
	(void)rank;
	(void)name;
	mutex->locked = 0;

	/* Succeeded: no callback owns the new controller mutex. */
	return 0;
}

/*
 * Detects reentrant controller access in this single-thread callback fixture.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* IRQ service takes only the independent display spin guard. */
	assert(mutex->locked == 0);
	mutex->locked = 1;
}

/*
 * Balances one real controller callback's ordinary mutex ownership.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* An unbalanced release is a fixture or production ownership error. */
	assert(mutex->locked == 1);
	mutex->locked = 0;
}

/*
 * Supplies a nonzero completion timestamp for successful native presentation.
 */
uint64_t
sched_ticks(
	void)
{
	/* One stable tick suffices to distinguish timing from a submission counter. */
	return 1;
}

/*
 * Allows each explicitly implemented diagnostic stage in this host fixture.
 */
bool
bcm2711_stage_allowed(
	const char *family,
	const char *stage)
{
	/* No hidden switch changes the production operation being exercised. */
	(void)family;
	(void)stage;

	/* Succeeded: the stage can proceed. */
	return true;
}

/*
 * Discards stage prose without changing any hardware or ownership observation.
 */
void
bcm2711_stage_mark(
	const char *family,
	const char *format,
	...)
{
	/* The model checks callback outcomes rather than parsing debug text. */
	(void)family;
	(void)format;
}

/*
 * Discards allocation diagnostics; unexpected release errors are asserted separately.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* Log output is never used to manufacture a successful functional test. */
	(void)format;
}

/*
 * Supplies inert interrupt-controller line unmasking for direct source delivery.
 */
void
kern_irq_unmask(
	int irq)
{
	/* Direct service invocation remains independent of GIC routing. */
	(void)irq;
}

/*
 * Supplies inert interrupt-controller line masking for shared helper references.
 */
void
kern_irq_mask(
	int irq)
{
	/* Owned source W1C is modeled by register writes above. */
	(void)irq;
}
