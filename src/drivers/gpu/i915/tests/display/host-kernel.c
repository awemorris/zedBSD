/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host stand-ins of the kernel services and of the i915 files outside
 * display/ that the display code links against, for the host display
 * tests.
 *
 * The tested paths reach the display code through its hooks (struct
 * i915_dp_env, struct i915_lcd_emit), which the register models of
 * dp-fake-hw.c and lcd-fake-hw.c serve; what they reach of the kernel is
 * the allocator, the locks, the log and the clock, which are served here
 * single-threaded.  Everything else (the MMIO BAR, the PCI bus, the GT
 * memory, the work queues, the worker) stands in only so the production
 * files link: a call ends the test with the name of the service, because
 * the tested path must not reach real hardware.
 *
 * Host only: built by the plan/ws031/tests/run-*-host-test.sh scripts.
 */

#include "host-test.h"

#include "../../display/internal.h"

#include "../../firmware.h"
#include "../../ggtt.h"
#include "../../irq.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../pci.h"
#include "../../power.h"
#include "../../ppgtt.h"
#include "../../runtime-pm.h"
#include "../../sync.h"
#include "../../worker.h"
#include "../../workqueue.h"
#include "../../render/blit.h"

#include "drivers/platform/pcat/graphics/backend.h"

#include <drivers/pci/pci.h>
#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Whether interrupt delivery is enabled on the one host "processor".
 *
 * Starts enabled; kern_irq_disable() and spin_lock_irqsave() clear it and
 * their counterparts restore it.
 */
static bool i915_host_irq_enabled = true;

/*
 * The nesting depth of kern_preempt_disable().
 *
 * Never negative; a release without an acquisition ends the test.
 */
static unsigned i915_host_preempt_depth;

/*
 * The scheduler tick the host reports.
 *
 * Advances by one on every reading, so a loop bounded by a tick deadline
 * always ends.
 */
static uint64_t i915_host_ticks;

static void i915_host_unreachable(const char *service) __attribute__((noreturn));
static void i915_host_lock_error(const char *what, const char *name) __attribute__((noreturn));

/*
 * Prints a kernel log line when the run is verbose.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* A quiet run prints nothing of the driver's log. */
	if (!i915_host_verbose)
		return;

	/* Prints the line as the kernel log would. */
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

/*
 * Allocates zeroed memory from the host heap.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *memory;

	/* Allocates from the host heap, which ASan watches. */
	memory = calloc(count, size);
	if (memory == NULL)
		return NULL;

	/* Succeeded: the memory is zeroed. */
	return memory;
}

/*
 * Frees memory from kern_calloc().
 */
void
kern_free(
	void *pointer)
{
	/* Returns the memory to the host heap. */
	free(pointer);
}

/*
 * Initializes a spin lock as free.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Starts the lock free, named for the error messages. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

/*
 * Takes a spin lock; a second acquisition would never return on a kernel.
 */
void
spin_lock(
	struct spinlock *lock)
{
	/* owner_valid marks the lock taken on the one host processor. */
	if (lock->owner_valid != 0U)
		i915_host_lock_error("spin lock taken twice", lock->name);

	lock->owner_valid = 1U;
}

/*
 * Releases a spin lock.
 */
void
spin_unlock(
	struct spinlock *lock)
{
	/* Releasing a free lock is a driver bug. */
	if (lock->owner_valid == 0U)
		i915_host_lock_error("free spin lock released", lock->name);

	lock->owner_valid = 0U;
}

/*
 * Disables interrupts and takes a spin lock.
 *
 * Returns whether interrupts were enabled before.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	unsigned long enabled;

	/* Remembers and clears the interrupt state. */
	enabled = 0UL;
	if (i915_host_irq_enabled)
		enabled = 1UL;

	i915_host_irq_enabled = false;

	/* Takes the lock. */
	spin_lock(lock);

	/* Succeeded: reports the state to restore. */
	return enabled;
}

/*
 * Releases a spin lock and restores the interrupt state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Releases the lock. */
	spin_unlock(lock);

	/* Restores the state spin_lock_irqsave() saved. */
	if (enabled != 0UL)
		i915_host_irq_enabled = true;
}

/*
 * Initializes a mutex as free.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Starts the mutex free. */
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);
	waitq_init(&mutex->waiters, name);

	/* Succeeded: the mutex may be taken. */
	return 0;
}

/*
 * Takes a mutex; the single-threaded host has nobody to wait for, so a
 * held mutex is a self-deadlock.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* A held mutex would never be released. */
	if (mutex->locked != 0U)
		i915_host_lock_error("mutex taken twice", mutex->guard.name);

	mutex->locked = 1U;
}

/*
 * Releases a mutex.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* Releasing a free mutex is a driver bug. */
	if (mutex->locked == 0U)
		i915_host_lock_error("free mutex released", mutex->guard.name);

	mutex->locked = 0U;
}

/*
 * Tells whether the caller holds a mutex.
 *
 * On the single-threaded host the caller is whoever took it.
 */
int
mutex_owned(
	struct mutex *mutex)
{
	/* A taken mutex is the caller's. */
	if (mutex->locked != 0U)
		return 1;

	/* Succeeded: nobody holds it. */
	return 0;
}

/*
 * Initializes an empty wait queue.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Starts the queue without waiters. */
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

/*
 * Returns the scheduler tick, advancing it by one.
 */
uint64_t
sched_ticks(void)
{
	/* Every reading is one tick later than the last. */
	i915_host_ticks++;

	/* Succeeded: reports the tick. */
	return i915_host_ticks;
}

/*
 * Disables interrupt delivery; returns whether it was enabled.
 */
bool
kern_irq_disable(void)
{
	bool enabled;

	/* Remembers and clears the state. */
	enabled = i915_host_irq_enabled;
	i915_host_irq_enabled = false;

	/* Succeeded: reports the state to restore. */
	return enabled;
}

/*
 * Enables interrupt delivery.
 */
void
kern_irq_enable(void)
{
	/* Delivery is on again. */
	i915_host_irq_enabled = true;
}

/*
 * Disables preemption, nesting.
 */
void
kern_preempt_disable(void)
{
	/* One more level of nesting. */
	i915_host_preempt_depth++;
}

/*
 * Enables preemption at the outermost level.
 */
void
kern_preempt_enable(void)
{
	/* An enable without a disable is a driver bug. */
	if (i915_host_preempt_depth == 0U)
		i915_host_lock_error("preemption enabled without a disable", "preempt");

	i915_host_preempt_depth--;
}

/*
 * Computes a deadline delta after now.
 *
 * Returns 0, or ERANGE when the sum does not fit.
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	/* Refuses a deadline past the end of time. */
	if (delta > UINT64_MAX - now)
		return ERANGE;

	*deadline = now + delta;

	/* Succeeded: the deadline is set. */
	return 0;
}

/*
 * Sleeps between min_us and max_us microseconds; the host does not wait,
 * because the tested paths keep their time in the register models.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	UNUSED_PARAMETER(min_us);
	UNUSED_PARAMETER(max_us);
}

/*
 * Reads the real-time counter; the host has none to offer.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(frequency_hz);

	/* Reports that no counter is available. */
	return false;
}

/*
 * Busy-waits microseconds; the host does not wait.
 */
int
drv_i915_udelay(
	unsigned microseconds)
{
	UNUSED_PARAMETER(microseconds);

	/* Succeeded: the time base is sound. */
	return 0;
}

/*
 * Reports a latched time-base fault; the host clock never faults.
 */
int
drv_i915_time_base_faulted(void)
{
	/* Succeeded: no fault. */
	return 0;
}

/*
 * Stands in for drv_i915_cancel_work_sync(): the work queue is not modelled on the host.
 */
int
drv_i915_cancel_work_sync(
	struct i915_workqueue *queue,
	struct i915_work *work,
	uint64_t deadline)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(deadline);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_cancel_work_sync");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_complete(): the completion is not modelled on the host.
 */
void
drv_i915_complete(
	struct i915_completion *completion)
{
	UNUSED_PARAMETER(completion);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_complete");
}

/*
 * Stands in for drv_i915_completion_init(): the completion is not modelled on the host.
 */
void
drv_i915_completion_init(
	struct i915_completion *completion,
	const char *name)
{
	UNUSED_PARAMETER(completion);
	UNUSED_PARAMETER(name);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_completion_init");
}

/*
 * Stands in for drv_i915_delayed_cancel(): the timer queue is not modelled on the host.
 */
int
drv_i915_delayed_cancel(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	UNUSED_PARAMETER(timers);
	UNUSED_PARAMETER(delayed);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_delayed_cancel");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_delayed_cancel_sync(): the timer queue is not modelled on the host.
 */
int
drv_i915_delayed_cancel_sync(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed,
	uint64_t deadline)
{
	UNUSED_PARAMETER(timers);
	UNUSED_PARAMETER(delayed);
	UNUSED_PARAMETER(deadline);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_delayed_cancel_sync");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_delayed_pending(): the timer queue is not modelled on the host.
 */
int
drv_i915_delayed_pending(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	UNUSED_PARAMETER(timers);
	UNUSED_PARAMETER(delayed);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_delayed_pending");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_delayed_queue(): the timer queue is not modelled on the host.
 */
int
drv_i915_delayed_queue(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed,
	unsigned delay_ms)
{
	UNUSED_PARAMETER(timers);
	UNUSED_PARAMETER(delayed);
	UNUSED_PARAMETER(delay_ms);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_delayed_queue");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_delayed_work_init(): the timer queue is not modelled on the host.
 */
void
drv_i915_delayed_work_init(
	struct i915_delayed_work *delayed,
	void (*function)(void *),
	void *context)
{
	UNUSED_PARAMETER(delayed);
	UNUSED_PARAMETER(function);
	UNUSED_PARAMETER(context);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_delayed_work_init");
}

/*
 * Stands in for drv_i915_firmware_release(): the firmware loader is not modelled on the host.
 */
void
drv_i915_firmware_release(
	struct i915_firmware *firmware)
{
	UNUSED_PARAMETER(firmware);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_firmware_release");
}

/*
 * Stands in for drv_i915_firmware_request(): the firmware loader is not modelled on the host.
 */
int
drv_i915_firmware_request(
	struct i915_firmware *firmware,
	const char *name)
{
	UNUSED_PARAMETER(firmware);
	UNUSED_PARAMETER(name);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_firmware_request");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_flush_work(): the work queue is not modelled on the host.
 */
int
drv_i915_flush_work(
	struct i915_workqueue *queue,
	struct i915_work *work,
	uint64_t deadline)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(deadline);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_flush_work");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gen3_assert_iir_is_zero(): the interrupt registers is not modelled on the host.
 */
void
drv_i915_gen3_assert_iir_is_zero(
	struct i915_irq_dev *irq,
	uint32_t iir)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(iir);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gen3_assert_iir_is_zero");
}

/*
 * Stands in for drv_i915_gen3_irq_init(): the interrupt registers is not modelled on the host.
 */
void
drv_i915_gen3_irq_init(
	struct i915_irq_dev *irq,
	uint32_t imr,
	uint32_t imr_value,
	uint32_t ier,
	uint32_t ier_value,
	uint32_t iir)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(imr);
	UNUSED_PARAMETER(imr_value);
	UNUSED_PARAMETER(ier);
	UNUSED_PARAMETER(ier_value);
	UNUSED_PARAMETER(iir);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gen3_irq_init");
}

/*
 * Stands in for drv_i915_gen3_irq_reset(): the interrupt registers is not modelled on the host.
 */
void
drv_i915_gen3_irq_reset(
	struct i915_irq_dev *irq,
	uint32_t imr,
	uint32_t iir,
	uint32_t ier)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(imr);
	UNUSED_PARAMETER(iir);
	UNUSED_PARAMETER(ier);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gen3_irq_reset");
}

/*
 * Stands in for drv_i915_gfx_rect_build(): the render engine is not modelled on the host.
 */
int
drv_i915_gfx_rect_build(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear,
	uint64_t *batch_va)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(dst);
	UNUSED_PARAMETER(dst_rect);
	UNUSED_PARAMETER(src);
	UNUSED_PARAMETER(src_rect);
	UNUSED_PARAMETER(clear);
	UNUSED_PARAMETER(linear);
	UNUSED_PARAMETER(batch_va);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gfx_rect_build");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gfx_rect_prepare(): the render engine is not modelled on the host.
 */
int
drv_i915_gfx_rect_prepare(
	struct i915_render_session *session)
{
	UNUSED_PARAMETER(session);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gfx_rect_prepare");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gt_clflush(): the GT memory is not modelled on the host.
 */
void
drv_i915_gt_clflush(
	const volatile void *address,
	size_t bytes)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(bytes);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_clflush");
}

/*
 * Stands in for drv_i915_gt_display_bind(): the GGTT is not modelled on the host.
 */
int
drv_i915_gt_display_bind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o,
	unsigned align_pages,
	unsigned guard_pages)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(o);
	UNUSED_PARAMETER(align_pages);
	UNUSED_PARAMETER(guard_pages);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_display_bind");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gt_display_unbind(): the GGTT is not modelled on the host.
 */
void
drv_i915_gt_display_unbind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(o);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_display_unbind");
}

/*
 * Stands in for drv_i915_gt_display_window_init(): the GGTT is not modelled on the host.
 */
int
drv_i915_gt_display_window_init(
	struct i915_gt_mem *gm,
	unsigned pages)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(pages);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_display_window_init");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gt_ggtt_read_pte(): the GGTT is not modelled on the host.
 */
uint64_t
drv_i915_gt_ggtt_read_pte(
	const struct i915_gt_mem *gm,
	unsigned index)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(index);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_ggtt_read_pte");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_gt_object_create(): the GT memory is not modelled on the host.
 */
struct i915_gt_object *
drv_i915_gt_object_create(
	struct i915_gt_mem *gm,
	uint32_t bytes)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(bytes);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_object_create");

	/* Not reached: i915_host_unreachable() does not return. */
	return NULL;
}

/*
 * Stands in for drv_i915_gt_object_destroy(): the GT memory is not modelled on the host.
 */
void
drv_i915_gt_object_destroy(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	UNUSED_PARAMETER(gm);
	UNUSED_PARAMETER(o);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_object_destroy");
}

/*
 * Stands in for drv_i915_gt_object_page_dma(): the GT memory is not modelled on the host.
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

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_gt_object_page_dma");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_pci_read16(): the PCI configuration space is not modelled on the host.
 */
uint16_t
drv_i915_pci_read16(
	struct i915_pci *pci,
	unsigned offset)
{
	UNUSED_PARAMETER(pci);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_pci_read16");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_pci_read32(): the PCI configuration space is not modelled on the host.
 */
uint32_t
drv_i915_pci_read32(
	struct i915_pci *pci,
	unsigned offset)
{
	UNUSED_PARAMETER(pci);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_pci_read32");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_pci_read8(): the PCI configuration space is not modelled on the host.
 */
uint8_t
drv_i915_pci_read8(
	struct i915_pci *pci,
	unsigned offset)
{
	UNUSED_PARAMETER(pci);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_pci_read8");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_pci_write32(): the PCI configuration space is not modelled on the host.
 */
void
drv_i915_pci_write32(
	struct i915_pci *pci,
	unsigned offset,
	uint32_t value)
{
	UNUSED_PARAMETER(pci);
	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_pci_write32");
}

/*
 * Stands in for drv_i915_pcode_read(): the PCODE mailbox is not modelled on the host.
 */
int
drv_i915_pcode_read(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t *val,
	uint32_t *val1)
{
	UNUSED_PARAMETER(sb_lock);
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(mbox);
	UNUSED_PARAMETER(val);
	UNUSED_PARAMETER(val1);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_pcode_read");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_posting_read32(): the MMIO BAR is not modelled on the host.
 */
void
drv_i915_posting_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_posting_read32");
}

/*
 * Stands in for drv_i915_ppgtt_clear(): the PPGTT is not modelled on the host.
 */
void
drv_i915_ppgtt_clear(
	struct i915_ppgtt *vm,
	uint64_t va,
	unsigned pages)
{
	UNUSED_PARAMETER(vm);
	UNUSED_PARAMETER(va);
	UNUSED_PARAMETER(pages);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_ppgtt_clear");
}

/*
 * Stands in for drv_i915_ppgtt_insert_uncached(): the PPGTT is not modelled on the host.
 */
int
drv_i915_ppgtt_insert_uncached(
	struct i915_ppgtt *vm,
	uint64_t va,
	uint64_t physical,
	unsigned pages)
{
	UNUSED_PARAMETER(vm);
	UNUSED_PARAMETER(va);
	UNUSED_PARAMETER(physical);
	UNUSED_PARAMETER(pages);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_ppgtt_insert_uncached");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_ppgtt_va_alloc(): the PPGTT is not modelled on the host.
 */
int
drv_i915_ppgtt_va_alloc(
	struct i915_ppgtt *vm,
	uint64_t bytes,
	uint64_t *va)
{
	UNUSED_PARAMETER(vm);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(va);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_ppgtt_va_alloc");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_queue_work(): the work queue is not modelled on the host.
 */
int
drv_i915_queue_work(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(work);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_queue_work");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_raw_read32(): the MMIO BAR is not modelled on the host.
 */
uint32_t
drv_i915_raw_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_raw_read32");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_raw_write32(): the MMIO BAR is not modelled on the host.
 */
void
drv_i915_raw_write32(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_raw_write32");
}

/*
 * Stands in for drv_i915_read32(): the MMIO BAR is not modelled on the host.
 */
uint32_t
drv_i915_read32(
	struct i915_mmio *mmio,
	uint32_t offset)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(offset);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_read32");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_reinit_completion(): the completion is not modelled on the host.
 */
void
drv_i915_reinit_completion(
	struct i915_completion *completion)
{
	UNUSED_PARAMETER(completion);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_reinit_completion");
}

/*
 * Stands in for drv_i915_rpm_active(): the runtime power management is not modelled on the host.
 */
int
drv_i915_rpm_active(
	const struct i915_rpm *rpm)
{
	UNUSED_PARAMETER(rpm);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_rpm_active");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_rpm_get_sync(): the runtime power management is not modelled on the host.
 */
int
drv_i915_rpm_get_sync(
	struct i915_rpm *rpm)
{
	UNUSED_PARAMETER(rpm);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_rpm_get_sync");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_rpm_put(): the runtime power management is not modelled on the host.
 */
void
drv_i915_rpm_put(
	struct i915_rpm *rpm)
{
	UNUSED_PARAMETER(rpm);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_rpm_put");
}

/*
 * Stands in for drv_i915_rpm_usage(): the runtime power management is not modelled on the host.
 */
int
drv_i915_rpm_usage(
	const struct i915_rpm *rpm)
{
	UNUSED_PARAMETER(rpm);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_rpm_usage");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_skl_pcode_request(): the PCODE mailbox is not modelled on the host.
 */
int
drv_i915_skl_pcode_request(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t request,
	uint32_t reply_mask,
	uint32_t reply,
	int timeout_base_ms)
{
	UNUSED_PARAMETER(sb_lock);
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(mbox);
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(reply_mask);
	UNUSED_PARAMETER(reply);
	UNUSED_PARAMETER(timeout_base_ms);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_skl_pcode_request");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_snb_pcode_write(): the PCODE mailbox is not modelled on the host.
 */
int
drv_i915_snb_pcode_write(
	struct mutex *sb_lock,
	struct i915_mmio *mmio,
	uint32_t mbox,
	uint32_t val)
{
	UNUSED_PARAMETER(sb_lock);
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(mbox);
	UNUSED_PARAMETER(val);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_snb_pcode_write");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_timer_queue_create(): the timer queue is not modelled on the host.
 */
int
drv_i915_timer_queue_create(
	struct i915_timer_queue *timers,
	struct i915_workqueue *workqueue,
	const char *name)
{
	UNUSED_PARAMETER(timers);
	UNUSED_PARAMETER(workqueue);
	UNUSED_PARAMETER(name);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_timer_queue_create");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_timer_queue_destroy(): the timer queue is not modelled on the host.
 */
void
drv_i915_timer_queue_destroy(
	struct i915_timer_queue *timers)
{
	UNUSED_PARAMETER(timers);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_timer_queue_destroy");
}

/*
 * Stands in for drv_i915_wait_for_completion(): the completion is not modelled on the host.
 */
int
drv_i915_wait_for_completion(
	struct i915_completion *completion,
	uint64_t deadline)
{
	UNUSED_PARAMETER(completion);
	UNUSED_PARAMETER(deadline);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_wait_for_completion");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_wait_reg(): the MMIO BAR is not modelled on the host.
 */
int
drv_i915_wait_reg(
	struct i915_mmio *mmio,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *last)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(reg);
	UNUSED_PARAMETER(mask);
	UNUSED_PARAMETER(value);
	UNUSED_PARAMETER(fast_us);
	UNUSED_PARAMETER(slow_ms);
	UNUSED_PARAMETER(last);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_wait_reg");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_worker_run_batch(): the device worker is not modelled on the host.
 */
int
drv_i915_worker_run_batch(
	struct i915_device *device,
	struct i915_context *context,
	uint64_t batch_va)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(batch_va);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_worker_run_batch");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_worker_serve_window(): the device worker is not modelled on the host.
 */
void
drv_i915_worker_serve_window(
	struct i915_device *device)
{
	UNUSED_PARAMETER(device);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_worker_serve_window");
}

/*
 * Stands in for drv_i915_worker_sync_display(): the device worker is not modelled on the host.
 */
int
drv_i915_worker_sync_display(
	struct i915_device *device,
	enum i915_worker_sync_kind kind,
	const struct i915_worker_present *present)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(kind);
	UNUSED_PARAMETER(present);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_worker_sync_display");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_work_init(): the work queue is not modelled on the host.
 */
void
drv_i915_work_init(
	struct i915_work *work,
	void (*function)(void *),
	void *context)
{
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(function);
	UNUSED_PARAMETER(context);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_work_init");
}

/*
 * Stands in for drv_i915_work_pending(): the work queue is not modelled on the host.
 */
int
drv_i915_work_pending(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(work);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_work_pending");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_workqueue_create(): the work queue is not modelled on the host.
 */
int
drv_i915_workqueue_create(
	struct i915_workqueue *queue,
	const char *name)
{
	UNUSED_PARAMETER(queue);
	UNUSED_PARAMETER(name);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_workqueue_create");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_i915_workqueue_destroy(): the work queue is not modelled on the host.
 */
void
drv_i915_workqueue_destroy(
	struct i915_workqueue *queue)
{
	UNUSED_PARAMETER(queue);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_workqueue_destroy");
}

/*
 * Stands in for drv_i915_write32(): the MMIO BAR is not modelled on the host.
 */
void
drv_i915_write32(
	struct i915_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	UNUSED_PARAMETER(mmio);
	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_i915_write32");
}

/*
 * Stands in for drv_pcat_graphics_backend_get_framebuffer(): the boot framebuffer is not modelled on the host.
 */
int
drv_pcat_graphics_backend_get_framebuffer(
	volatile uint32_t **pixels,
	unsigned *width,
	unsigned *height,
	unsigned *stride,
	int *rgbx)
{
	UNUSED_PARAMETER(pixels);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(rgbx);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pcat_graphics_backend_get_framebuffer");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_class(): the PCI bus is not modelled on the host.
 */
uint32_t
drv_pci_device_class(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_class");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_config_read16(): the PCI bus is not modelled on the host.
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

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_config_read16");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_config_write16(): the PCI bus is not modelled on the host.
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

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_config_write16");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_product(): the PCI bus is not modelled on the host.
 */
uint16_t
drv_pci_device_product(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_product");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_revision(): the PCI bus is not modelled on the host.
 */
uint8_t
drv_pci_device_revision(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_revision");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_subproduct(): the PCI bus is not modelled on the host.
 */
uint16_t
drv_pci_device_subproduct(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_subproduct");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_subvendor(): the PCI bus is not modelled on the host.
 */
uint16_t
drv_pci_device_subvendor(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_subvendor");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_device_vendor(): the PCI bus is not modelled on the host.
 */
uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *d)
{
	UNUSED_PARAMETER(d);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_device_vendor");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for drv_pci_find_class(): the PCI bus is not modelled on the host.
 */
struct drv_pci_device *
drv_pci_find_class(
	uint32_t c,
	uint32_t m,
	struct drv_pci_device *after)
{
	UNUSED_PARAMETER(c);
	UNUSED_PARAMETER(m);
	UNUSED_PARAMETER(after);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_find_class");

	/* Not reached: i915_host_unreachable() does not return. */
	return NULL;
}

/*
 * Stands in for drv_pci_find_device(): the PCI bus is not modelled on the host.
 */
struct drv_pci_device *
drv_pci_find_device(
	const struct drv_pci_address *a)
{
	UNUSED_PARAMETER(a);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("drv_pci_find_device");

	/* Not reached: i915_host_unreachable() does not return. */
	return NULL;
}

/*
 * Stands in for hal_space_map_device(): the device mappings is not modelled on the host.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	UNUSED_PARAMETER(paddr);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(attr);
	UNUSED_PARAMETER(vaddr);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("hal_space_map_device");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for hal_space_unmap_device(): the device mappings is not modelled on the host.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);
	UNUSED_PARAMETER(size);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("hal_space_unmap_device");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for kern_boot_handoff(): the boot handoff is not modelled on the host.
 */
void *
kern_boot_handoff(
	const char *name)
{
	UNUSED_PARAMETER(name);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("kern_boot_handoff");

	/* Not reached: i915_host_unreachable() does not return. */
	return NULL;
}

/*
 * Stands in for kern_io_in8(): the I/O ports is not modelled on the host.
 */
uint8_t
kern_io_in8(
	uint16_t port)
{
	UNUSED_PARAMETER(port);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("kern_io_in8");

	/* Not reached: i915_host_unreachable() does not return. */
	return 0;
}

/*
 * Stands in for kern_io_out8(): the I/O ports is not modelled on the host.
 */
void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(value);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("kern_io_out8");
}

/*
 * Stands in for kern_pmem_to_kernel(): the physical memory is not modelled on the host.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	UNUSED_PARAMETER(address);

	/* Ends the test: the tested path reached a service the host does not have. */
	i915_host_unreachable("kern_pmem_to_kernel");

	/* Not reached: i915_host_unreachable() does not return. */
	return NULL;
}
/* Ends the test because the tested path reached a service the host does not model. */
static void
i915_host_unreachable(
	const char *service)
{
	/* Names the service, then stops with a distinct status. */
	fprintf(stderr, "host-kernel: %s is not modelled on the host (the tested path must not reach it)\n", service);
	fflush(stdout);
	abort();
}

/* Ends the test on a lock misuse a kernel would deadlock or panic on. */
static void
i915_host_lock_error(
	const char *what,
	const char *name)
{
	/* Names the misuse and the lock, then stops. */
	if (name == NULL)
		name = "(unnamed)";

	fprintf(stderr, "host-kernel: %s: %s\n", what, name);
	fflush(stdout);
	abort();
}
