/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel and hardware stand-ins under the PC/AT console's end: locks
 * that work across host threads, device mappings over host memory that
 * model the hardware layer (a fresh view per mapping, the early console
 * window handed over once the kernel prints, the legacy aperture that
 * stays mapped), the boot framebuffer handoff, threads that run when a
 * scene says, and the services the console calls.
 *
 * A removed mapping is made read-only, so a write through it after the
 * removal ends the process.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/hal.h"
#include "kern/cdev.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/platform.h"
#include "kern/pmem.h"
#include "kern/sched.h"
#include "kern/text-display.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include "uapi/errno.h"
#include "drivers/pci/pci.h"
#include "drivers/platform/pcat/serial-mirror.h"

#include "text-end-stubs.h"

/* How many mappings the stand-in keeps track of, and how many injections a scene may set. */
#define STUB_MAPPINGS_MAX	32U
#define STUB_INJECTIONS_MAX	8U

/* One mapping the stand-in made, live until a removal that succeeded. */
struct stub_mapping {
	void *mapped;
	uint64_t physical;
	size_t bytes;
	size_t length;
	enum stub_kind kind;
	int live;
};

/* One error a scene asks the nth map or unmap call to return. */
struct stub_injection {
	int unmap;
	unsigned nth;
	int error;
};

/* What a scene sets. */
int stub_handoff;
int stub_quiet;
int stub_window_fixed;

/* What the stand-ins saw. */
unsigned stub_failures;
struct stub_call stub_calls[STUB_CALLS_MAX];
unsigned stub_call_count;
unsigned stub_bad_unmaps;
unsigned stub_mirrored;
unsigned stub_io_writes;
unsigned stub_resumes;
unsigned stub_mutex_locks;
enum stub_window_state stub_window_state;
uint32_t *stub_window;
uint8_t *stub_legacy;
const struct cdev_ops *stub_graphics_ops;
uint64_t stub_last_sleep;
unsigned stub_mutex_waits;
unsigned long stub_unlock_sequence[2];
unsigned long stub_acquire_sequence[2];
void (*stub_unmap_hook)(void);
void (*stub_sleep_hook)(unsigned sleeps);
void (*stub_thread_entry[4])(void *);
void *stub_thread_argument[4];
unsigned stub_thread_count;
__thread unsigned stub_cpu;

/* The kernel's character path, which the HAL reads. */
void (*kernel_putc)(int c);

/* The mappings made, the injections set, and the counters behind them. */
static struct stub_mapping mappings[STUB_MAPPINGS_MAX];
static unsigned mapping_count;
static struct stub_injection injections[STUB_INJECTIONS_MAX];
static unsigned injection_count;
static unsigned maps_seen;
static unsigned unmaps_seen;
static unsigned long sequence;
static uint64_t ticks;
static uint64_t counter;
static unsigned sleeps;
static int thread_storage;

void __real_kern_text_resume(void);
void __wrap_kern_text_resume(void);

static int injected(int unmap, unsigned nth);
static void call_record(int unmap, uint64_t physical, size_t bytes, void *mapped, enum stub_kind kind, int result);
static struct stub_mapping *mapping_add(void *mapped, uint64_t physical, size_t bytes, size_t length, enum stub_kind kind);

/*
 * Sets the hardware up for a scene: the early console window over the
 * framebuffer, filled with a picture, and the legacy aperture.
 */
void
stub_setup(void)
{
	size_t length;
	size_t index;

	/* The early console window, held by the hardware layer, showing a picture. */
	stub_window = text_end_host_pages((size_t)STUB_FB_BYTES, &length);
	for (index = 0; index < (size_t)STUB_FB_BYTES / 4U; index++)
		stub_window[index] = (uint32_t)(index * 2654435761U);
	stub_window_state = STUB_WINDOW_HELD;

	/* The legacy aperture. */
	stub_legacy = text_end_host_pages((size_t)STUB_LEGACY_BYTES, &length);
}

/* Has the nth (from 1) kern_device_map() return error. */
void
stub_inject_map(
	unsigned nth,
	int error)
{
	/* One more injection. */
	if (injection_count == STUB_INJECTIONS_MAX)
		abort();
	injections[injection_count].unmap = 0;
	injections[injection_count].nth = nth;
	injections[injection_count].error = error;
	injection_count++;
}

/* Has the nth (from 1) kern_device_unmap() return error and leave the mapping. */
void
stub_inject_unmap(
	unsigned nth,
	int error)
{
	/* One more injection. */
	if (injection_count == STUB_INJECTIONS_MAX)
		abort();
	injections[injection_count].unmap = 1;
	injections[injection_count].nth = nth;
	injections[injection_count].error = error;
	injection_count++;
}

/* Takes the next number in the order of events. */
unsigned long
stub_sequence_next(void)
{
	/* One more event. */
	return __atomic_add_fetch(&sequence, 1UL, __ATOMIC_SEQ_CST);
}

/* Counts the map (unmap 0) or unmap (unmap 1) calls seen. */
unsigned
stub_count(
	int unmap)
{
	unsigned index;
	unsigned count;

	/* Every call of the kind. */
	count = 0U;
	for (index = 0U; index < stub_call_count; index++) {
		if (stub_calls[index].unmap == unmap)
			count++;
	}
	return count;
}

/* Finds the nth (from 1) map or unmap call, or NULL. */
const struct stub_call *
stub_find(
	int unmap,
	unsigned nth)
{
	unsigned index;
	unsigned seen;

	/* The calls of the kind, in order. */
	seen = 0U;
	for (index = 0U; index < stub_call_count; index++) {
		if (stub_calls[index].unmap != unmap)
			continue;
		seen++;
		if (seen == nth)
			return &stub_calls[index];
	}
	return NULL;
}

/* Sums memory with the places of its bytes, so that any change shows. */
uint64_t
stub_sum(
	const volatile void *pixels,
	size_t bytes)
{
	const volatile uint8_t *byte;
	uint64_t sum;
	size_t index;

	/* Every byte, weighted by its index. */
	byte = pixels;
	sum = 0U;
	for (index = 0U; index < bytes; index++)
		sum += (uint64_t)byte[index] * (index + 1U);
	return sum;
}

/* Counts a failure the stand-ins found, and names it. */
void
stub_fail(
	const char *what)
{
	/* One more failure. */
	__atomic_add_fetch(&stub_failures, 1U, __ATOMIC_SEQ_CST);
	printf("FAIL %s\n", what);
}

/* Reports the error injected into the nth call of a kind, or 0. */
static int
injected(
	int unmap,
	unsigned nth)
{
	unsigned index;

	/* The injection for this call, if any. */
	for (index = 0U; index < injection_count; index++) {
		if (injections[index].unmap == unmap && injections[index].nth == nth)
			return injections[index].error;
	}
	return 0;
}

/* Logs one map or unmap call. */
static void
call_record(
	int unmap,
	uint64_t physical,
	size_t bytes,
	void *mapped,
	enum stub_kind kind,
	int result)
{
	struct stub_call *call;

	/* The next entry of the log. */
	if (stub_call_count == STUB_CALLS_MAX)
		abort();
	call = &stub_calls[stub_call_count++];
	call->unmap = unmap;
	call->physical = physical;
	call->bytes = bytes;
	call->mapped = mapped;
	call->kind = kind;
	call->result = result;
	call->cpu = stub_cpu;
}

/* Records a mapping the stand-in made. */
static struct stub_mapping *
mapping_add(
	void *mapped,
	uint64_t physical,
	size_t bytes,
	size_t length,
	enum stub_kind kind)
{
	struct stub_mapping *mapping;

	/* The next entry of the table. */
	if (mapping_count == STUB_MAPPINGS_MAX)
		abort();
	mapping = &mappings[mapping_count++];
	mapping->mapped = mapped;
	mapping->physical = physical;
	mapping->bytes = bytes;
	mapping->length = length;
	mapping->kind = kind;
	mapping->live = 1;
	return mapping;
}

/*
 * The kernel's device mapping, as the amd64 hardware layer does it: the
 * early console window for exactly the boot framebuffer, uncached, once
 * the kernel prints; the legacy aperture's fixed window; otherwise a fresh
 * view.
 */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	struct stub_mapping *mapping;
	void *pages;
	size_t length;
	int error;

	/* An injected failure maps nothing. */
	maps_seen++;
	error = injected(0, maps_seen);
	if (error != 0) {
		call_record(0, address, size, NULL, STUB_KIND_VIEW, error);
		return error;
	}

	/* The early console window, handed over once. */
	if (stub_window_state == STUB_WINDOW_HELD &&
	    __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE) != NULL &&
	    stub_handoff &&
	    address == STUB_FB_PHYSICAL &&
	    size == (size_t)STUB_FB_BYTES &&
	    attributes == KERN_DEVICE_UNCACHED) {
		stub_window_state = STUB_WINDOW_GIVEN;
		(void)mapping_add(stub_window, address, size, (size_t)STUB_FB_BYTES, STUB_KIND_WINDOW);
		call_record(0, address, size, stub_window, STUB_KIND_WINDOW, 0);
		*mapped = stub_window;
		return 0;
	}

	/* The legacy aperture's window, the same for every mapping inside it. */
	if (address >= STUB_LEGACY_PHYSICAL && address - STUB_LEGACY_PHYSICAL < STUB_LEGACY_BYTES) {
		if (size > STUB_LEGACY_BYTES - (address - STUB_LEGACY_PHYSICAL))
			abort();
		pages = stub_legacy + (address - STUB_LEGACY_PHYSICAL);
		(void)mapping_add(pages, address, size, 0U, STUB_KIND_FIXED);
		call_record(0, address, size, pages, STUB_KIND_FIXED, 0);
		*mapped = pages;
		return 0;
	}

	/* A fresh view of its own. */
	pages = text_end_host_pages(size, &length);
	mapping = mapping_add(pages, address, size, length, STUB_KIND_VIEW);
	(void)mapping;
	call_record(0, address, size, pages, STUB_KIND_VIEW, 0);
	*mapped = pages;
	return 0;
}

/*
 * The kernel's removal of a device mapping: a fixed window stays, the
 * early console window over a fixed window is refused, anything else goes
 * (read-only from now on).
 */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	struct stub_mapping *mapping;
	void (*hook)(void);
	unsigned index;
	int error;

	/* A scene's hook, once. */
	hook = stub_unmap_hook;
	stub_unmap_hook = NULL;
	if (hook != NULL)
		hook();

	/* The live mapping at the address. */
	unmaps_seen++;
	mapping = NULL;
	for (index = 0U; index < mapping_count; index++) {
		if (mappings[index].live && mappings[index].mapped == mapped) {
			mapping = &mappings[index];
			break;
		}
	}
	if (mapping == NULL || mapping->bytes != size) {
		stub_bad_unmaps++;
		stub_fail("an unmap of a mapping that is not there, or of another size");
		call_record(1, 0U, size, mapped, STUB_KIND_VIEW, EINVAL);
		return EINVAL;
	}

	/* An injected failure leaves the mapping. */
	error = injected(1, unmaps_seen);
	if (error != 0) {
		call_record(1, mapping->physical, size, mapped, mapping->kind, error);
		return error;
	}

	/* A fixed window stays mapped; the hardware layer reports success. */
	if (mapping->kind == STUB_KIND_FIXED) {
		call_record(1, mapping->physical, size, mapped, mapping->kind, 0);
		return 0;
	}

	/* The early console window over a fixed window cannot go. */
	if (mapping->kind == STUB_KIND_WINDOW && stub_window_fixed) {
		call_record(1, mapping->physical, size, mapped, mapping->kind, EBUSY);
		return EBUSY;
	}

	/* The mapping goes: no write reaches through it again. */
	mapping->live = 0;
	text_end_host_read_only(mapping->mapped, mapping->length);
	if (mapping->kind == STUB_KIND_WINDOW)
		stub_window_state = STUB_WINDOW_REMOVED;
	call_record(1, mapping->physical, size, mapped, mapping->kind, 0);
	return 0;
}

/* The boot framebuffer the platform handed over, when the scene has one. */
int
kern_boot_framebuffer(
	const char *prefix,
	struct kern_boot_framebuffer *framebuffer)
{
	/* Only the PC/AT name. */
	memset(framebuffer, 0, sizeof(*framebuffer));
	if (strcmp(prefix, "pcat.framebuffer") != 0)
		return ENOENT;
	if (!stub_handoff)
		return ENOENT;

	/* The framebuffer. */
	framebuffer->physical_base = STUB_FB_PHYSICAL;
	framebuffer->size = STUB_FB_BYTES;
	framebuffer->width = STUB_FB_WIDTH;
	framebuffer->height = STUB_FB_HEIGHT;
	framebuffer->pitch = STUB_FB_PITCH;
	framebuffer->format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	return 0;
}

/* No boot font: the built-in one is used. */
void *
kern_boot_handoff(
	const char *name)
{
	(void)name;
	return NULL;
}

/* Counts what /dev/graphics resumes, and resumes. */
void
__wrap_kern_text_resume(void)
{
	/* One more resume asked for. */
	stub_resumes++;
	__real_kern_text_resume();
}

/* A spinlock that holds across host threads, refusing a second take on one thread. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Free, named. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(
	struct spinlock *lock)
{
	/* A second take on the one thread would never come back. */
	if (__atomic_load_n(&lock->held.value, __ATOMIC_ACQUIRE) != 0U && lock->owner_valid && lock->owner_cpu == stub_cpu) {
		stub_fail("a spinlock taken twice on one thread");
		abort();
	}

	/* Waits for the lock. */
	while (__atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE) != 0U)
		text_end_host_pause(100000L);
	lock->owner_cpu = stub_cpu;
	lock->owner_valid = 1U;
}

int
spin_trylock(
	struct spinlock *lock)
{
	/* Taken when free. */
	if (__atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE) != 0U)
		return 0;
	lock->owner_cpu = stub_cpu;
	lock->owner_valid = 1U;
	return 1;
}

void
spin_unlock(
	struct spinlock *lock)
{
	/* Free again. */
	lock->owner_valid = 0U;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* The same lock; interrupts are not modelled. */
	spin_lock(lock);
	return 0UL;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)enabled;
	spin_unlock(lock);
}

/* A sleeping lock that holds across host threads, counting waits and the order of takes and releases. */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Free, named. */
	memset(mutex, 0, sizeof(*mutex));
	mutex->guard.rank = rank;
	mutex->guard.name = name;
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	/* A second take on the one thread would never come back. */
	if (__atomic_load_n(&mutex->locked, __ATOMIC_ACQUIRE) != 0U && mutex->guard.owner_valid && mutex->guard.owner_cpu == stub_cpu) {
		stub_fail("a mutex taken twice on one thread");
		abort();
	}

	/* Waits for the lock, counting a wait. */
	__atomic_add_fetch(&stub_mutex_locks, 1U, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&mutex->locked, __ATOMIC_ACQUIRE) != 0U)
		__atomic_add_fetch(&stub_mutex_waits, 1U, __ATOMIC_SEQ_CST);
	while (__atomic_exchange_n(&mutex->locked, 1U, __ATOMIC_ACQUIRE) != 0U)
		text_end_host_pause(100000L);
	mutex->guard.owner_cpu = stub_cpu;
	mutex->guard.owner_valid = 1U;
	stub_acquire_sequence[stub_cpu & 1U] = stub_sequence_next();
}

void
mutex_unlock(
	struct mutex *mutex)
{
	/* Free again, in order. */
	stub_unlock_sequence[stub_cpu & 1U] = stub_sequence_next();
	mutex->guard.owner_valid = 0U;
	__atomic_store_n(&mutex->locked, 0U, __ATOMIC_RELEASE);
}

/* Threads run when the scene says. */
int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	(void)priority;

	/* Recorded, to be run by the scene. */
	if (stub_thread_count == 4U)
		return ENOMEM;
	stub_thread_entry[stub_thread_count] = entry;
	stub_thread_argument[stub_thread_count] = arg;
	stub_thread_count++;
	*result = (struct thread *)(void *)&thread_storage;
	return 0;
}

void
thread_start(
	struct thread *thread)
{
	(void)thread;
}

/* A sleep lets the time pass to the tick asked for, and calls the scene's hook. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	/* The time asked for passes. */
	stub_last_sleep = timeout_tick;
	if (ticks < timeout_tick)
		ticks = timeout_tick;
	sleeps++;
	if (stub_sleep_hook != NULL)
		stub_sleep_hook(sleeps);
}

uint64_t
sched_ticks(void)
{
	return ticks;
}

/* A counter at 1 kHz that moves on 125 ms (one step of the splash's spinner) at each reading. */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *frequency)
{
	/* The next reading. */
	counter += 125U;
	*value = counter;
	*frequency = 1000U;
	return true;
}

/* The kernel's name for the same counter. */
bool
kern_rtc_read_counter(
	uint64_t *value,
	uint64_t *frequency)
{
	return hal_rtc_read_counter(value, frequency);
}

hal_cpu_id_t
hal_cpu_current(void)
{
	return (hal_cpu_id_t)stub_cpu;
}

/* The kernel's name for the same answer. */
unsigned
kern_cpu_current(void)
{
	return (unsigned)stub_cpu;
}

/* The device registry keeps the operations, for the scene to call. */
int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data)
{
	(void)rdev;
	(void)data;

	/* Only /dev/graphics registers here. */
	if (strcmp(name, "graphics") != 0)
		return EINVAL;
	stub_graphics_ops = ops;
	return 0;
}

/* User memory is host memory. */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	/* A plain copy. */
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	/* A plain copy. */
	memcpy((void *)destination, source, size);
	return 0;
}

/* The serial mirror and the VGA ports: counted. */
void
drv_pcat_serial_mirror(
	int character)
{
	(void)character;
	__atomic_add_fetch(&stub_mirrored, 1U, __ATOMIC_SEQ_CST);
}

void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	(void)port;
	(void)value;
	__atomic_add_fetch(&stub_io_writes, 1U, __ATOMIC_SEQ_CST);
}

/* The log: quiet as the scene says, written nowhere. */
int
kern_log_quiet(void)
{
	return stub_quiet;
}

void
kern_log_set_quiet(
	int quiet)
{
	(void)quiet;
}

void
kern_log_write(
	const char *bytes,
	size_t length)
{
	(void)bytes;
	(void)length;
}

void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

void
waitq_wake_all(
	struct wait_queue *queue)
{
	(void)queue;
}

/* No PCI device is found: the Cirrus driver never attaches. */
int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	(void)driver;
	return 0;
}

void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	(void)device;
	memset(address, 0, sizeof(*address));
	stub_fail("a PCI device was asked for");
}

uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *device)
{
	(void)device;
	stub_fail("a PCI device was asked for");
	return 0U;
}

uint16_t
drv_pci_device_product(
	const struct drv_pci_device *device)
{
	(void)device;
	stub_fail("a PCI device was asked for");
	return 0U;
}

int
drv_pci_device_enable(
	struct drv_pci_device *device)
{
	(void)device;
	stub_fail("a PCI device was asked for");
	return ENODEV;
}

int
drv_pci_device_bar(
	const struct drv_pci_device *device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	(void)device;
	(void)index;
	(void)bar;
	stub_fail("a PCI device was asked for");
	return ENODEV;
}

int
drv_pci_device_claim_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	(void)device;
	(void)index;
	stub_fail("a PCI device was asked for");
	return ENODEV;
}

void
drv_pci_device_release_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	(void)device;
	(void)index;
	stub_fail("a PCI device was asked for");
}

int
drv_pci_device_map_bar_region(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t offset,
	size_t size,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	(void)device;
	(void)index;
	(void)offset;
	(void)size;
	(void)flags;
	(void)mapping;
	stub_fail("a PCI device was asked for");
	return ENODEV;
}

void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	(void)device;
	(void)mapping;
	stub_fail("a PCI device was asked for");
}
