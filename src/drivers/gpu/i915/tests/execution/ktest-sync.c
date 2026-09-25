/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel tests of the kernel services the driver waits and queues
 * work on.
 *
 * The completions and the work queue are the ones the driver itself uses;
 * the tests drive them with real threads on the running kernel.  The reset,
 * the PCODE mailbox, the DRAM and bandwidth probe and the power-well fuse
 * wait run against a register model in this file and never reach the
 * device's registers.  The interrupt vector and write-combining checks call
 * the real HAL: they allocate and free MSI vectors for a made-up source and
 * map and unmap a small device range, and touch nothing the started device
 * owns.
 */

#include "ktest.h"
#include <kern/kcrt.h>

#include "../../mmio.h"
#include "../../power.h"
#include "../../reset.h"
#include "../../runtime-pm.h"
#include "../../sync.h"
#include "../../trace.h"
#include "../../workqueue.h"
#include "../../display/power.h"
#include "../../display/watermark.h"

#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* How many works the cross-CPU test queues, one generation each. */
#define I915_KTEST_XCPU_GENS		64U

/* The pattern mixed into each generation's payload. */
#define I915_KTEST_XCPU_MAGIC		0xA5A5A5A5U

/* The registers the model answers: GEN6_GDRST, the PCODE mailbox, SKL_FUSE_STATUS, HSW_PWR_WELL_CTL2. */
#define I915_KTEST_REG_GDRST		0x941cU
#define I915_KTEST_REG_PCODE_MAILBOX	0x138124U
#define I915_KTEST_REG_PCODE_DATA	0x138128U
#define I915_KTEST_REG_PCODE_DATA1	0x13812cU
#define I915_KTEST_REG_FUSE_STATUS	0x42000U
#define I915_KTEST_REG_PWR_WELL_CTL2	0x45404U

/* The PCODE mailbox bit the driver sets to post a command and the firmware clears. */
#define I915_KTEST_PCODE_READY		0x80000000U

/* The request bits of a power-well control register; each state bit sits one below its request bit. */
#define I915_KTEST_PW_REQUEST_BITS	0xAAAAAAAAU

/* How many other registers the model remembers. */
#define I915_KTEST_FAKE_REGS		96U

/* The fuse distribution bits of power gates 0 and 1 (SKL_FUSE_PG_DIST). */
#define I915_KTEST_FUSE_PG0		(1U << 27)
#define I915_KTEST_FUSE_PG1		(1U << 26)

/* The made-up PCI source the MSI vectors are allocated for. */
#define I915_KTEST_MSI_SOURCE		"PCI ffff:ff:1f.7"

/* The device range the write-combining checks map: the HPET page. */
#define I915_KTEST_WC_ADDRESS		0xfed00000ULL

/*
 * A register model of the reset, PCODE, fuse and power-well registers.
 *
 * One instance is shared by the tests, which run one after another; each
 * test clears it before use.  The PCODE mailbox answers either from a
 * script of one response per transaction, with a status that sticks for
 * every transaction, or by approving a request only while preemption is
 * disabled.
 */
struct i915_fake_mmio {
	/* How many reset requests read back as not yet acknowledged. */
	unsigned gdrst_fail_writes;

	/* How many reset requests were written. */
	unsigned gdrst_writes;

	/* The PCODE mailbox and its two data words. */
	uint32_t mailbox;
	uint32_t data;
	uint32_t data1;

	/* The scripted PCODE responses: { data, data1, status } per transaction. */
	const uint32_t (*script)[3];
	unsigned script_len;

	/* How many scripted transactions were answered. */
	unsigned txn;

	/* Nonzero: every PCODE transaction ends with this status byte. */
	int pcode_sticky_status;

	/* Nonzero: a request is approved only while preemption is disabled. */
	int pcode_approve_when_preempt;

	/* How many PCODE transactions reached the script or the approval model. */
	unsigned pcode_txn_count;

	/* The request bits the driver set in the power-well control register. */
	uint32_t pw_hsw_req;

	/* The fuse distribution status, and bits that appear after a number of reads. */
	uint32_t fuse_status;
	uint32_t fuse_delay_bits;
	unsigned fuse_delay;

	/* Every other register written, by offset. */
	uint32_t gen_off[I915_KTEST_FAKE_REGS];
	uint32_t gen_val[I915_KTEST_FAKE_REGS];
	unsigned gen_n;
};

/*
 * What a completer thread signals.
 *
 * It is static because the thread may still read it after the test moved on.
 */
struct i915_ktest_completer {
	/* The completion the thread signals once. */
	struct i915_completion *completion;
};

/*
 * What a self-requeueing work needs to queue itself again.
 */
struct i915_ktest_requeue {
	/* The queue the work runs on. */
	struct i915_workqueue *queue;

	/* The work itself. */
	struct i915_work *self;
};

/*
 * The shared state of the three-party synchronous cancel.
 *
 * The worker callback, the cancelling thread and the test thread meet on
 * these completions; the flags record the order in which they finished.
 */
struct i915_ktest_cancel {
	/* Signalled when the callback has started. */
	struct i915_completion worker_started;

	/* Signalled by the test to let the callback return. */
	struct i915_completion release;

	/* Signalled by the canceller once its synchronous cancel returned. */
	struct i915_completion cancel_done;

	/* Set at the very end of the callback. */
	volatile int worker_returned;

	/* The callback's flag as the canceller saw it after the cancel: must be 1. */
	volatile int order_ok;
};

/*
 * What the cancelling thread cancels.
 */
struct i915_ktest_canceller {
	/* The queue the work sits on. */
	struct i915_workqueue *queue;

	/* The work to cancel synchronously. */
	struct i915_work *work;
};

/*
 * The payload one generation of the cross-CPU test publishes.
 *
 * The test thread stores the payload before it queues the work; the
 * callback reads it first thing and reports where it ran.
 */
struct i915_ktest_xcpu {
	/* The payload published before the queue. */
	volatile uint32_t payload;

	/* The CPUs the test queued on and the callback ran on. */
	volatile unsigned queuer_cpu;
	volatile unsigned worker_cpu;

	/* The payload the callback saw. */
	volatile uint32_t seen_payload;

	/* Signalled by the callback when it is done. */
	struct i915_completion done;
};

/*
 * The trace ring the runtime-PM and power-domain tests record into.
 *
 * A ring is far too big for the 16 KiB kernel stack, and the tests run one
 * at a time on the start worker, so one static ring serves all of them;
 * each test clears it first.
 */
static struct i915_trace i915_ktest_trace;

/*
 * The register block the model is reached through, and the model itself.
 *
 * They are shared by the tests, which run one after another; each test
 * rebinds the block to a cleared model with i915_fake_open().
 */
static struct i915_mmio i915_ktest_mmio;
static struct i915_fake_mmio i915_ktest_fake;

/*
 * The meeting point of the three-party synchronous cancel; see its type.
 */
static struct i915_ktest_cancel i915_ktest_cancel_state;

/*
 * The payload of the cross-CPU test; see its type.
 */
static struct i915_ktest_xcpu i915_ktest_xcpu_state;

/*
 * The work queue of the work-queue tests and the works queued on it.
 *
 * They are static because they are large and because the queue's worker
 * reads them until the queue is destroyed at the end of those tests.
 */
static struct i915_workqueue i915_ktest_queue;
static struct i915_work i915_ktest_xcpu_works[I915_KTEST_XCPU_GENS];
static struct i915_work i915_ktest_work;
static struct i915_ktest_requeue i915_ktest_requeue_context;
static struct i915_ktest_canceller i915_ktest_canceller_context;

/*
 * The completion of the completion tests and what their completer thread
 * signals; the detached thread may still hold the context after the wait.
 */
static struct i915_completion i915_ktest_completion;
static struct i915_ktest_completer i915_ktest_completer_context;

/*
 * The handler arguments of the MSI lifecycle test.
 *
 * Only their addresses matter: a detach must name the argument the attach
 * registered, and the other one stands for a mismatched registration.
 */
static int i915_ktest_msi_token;
static int i915_ktest_msi_other_token;

/*
 * The co-runner of the sleep-range test.
 *
 * The counter moves while the co-runner runs; the stop flag, set by the
 * test once it has measured, makes the co-runner leave.
 */
static volatile unsigned long i915_ktest_corun_counter;
static volatile int i915_ktest_corun_stop;

/*
 * Nonzero makes the runtime-PM test device fail its resume.
 */
static int i915_ktest_rpm_fail;

/*
 * The power-domain state the late-fuse test builds.
 *
 * It is too big for the stack and is not the started device's state.
 */
static struct i915_power_domains i915_ktest_power_domains;

static void i915_ktest_waitq_deadline(struct i915_ktest *ktest);
static void i915_ktest_completions(struct i915_ktest *ktest);
static void i915_ktest_workqueue(struct i915_ktest *ktest);
static void i915_ktest_workqueue_requeue(struct i915_ktest *ktest);
static void i915_ktest_workqueue_cancel(struct i915_ktest *ktest);
static void i915_ktest_workqueue_xcpu(struct i915_ktest *ktest);
static void i915_ktest_msi(struct i915_ktest *ktest);
static void i915_ktest_msi_attach(struct i915_ktest *ktest);
static void i915_ktest_msi_distinct(struct i915_ktest *ktest);
static void i915_ktest_wc_contract(struct i915_ktest *ktest);
static void i915_ktest_reset(struct i915_ktest *ktest);
static void i915_ktest_pcode(struct i915_ktest *ktest);
static void i915_ktest_dram_decode(struct i915_ktest *ktest);
static void i915_ktest_dram_bw(struct i915_ktest *ktest);
static void i915_ktest_dram_bw_psf_failure(struct i915_ktest *ktest, struct mutex *sb_lock);
static void i915_ktest_wc_full_range(struct i915_ktest *ktest);
static void i915_ktest_irq_oneshot(struct i915_ktest *ktest);
static void i915_ktest_rpm(struct i915_ktest *ktest);
static void i915_ktest_pcode_request(struct i915_ktest *ktest);
static void i915_ktest_usleep_range(struct i915_ktest *ktest);
static void i915_ktest_preempt(struct i915_ktest *ktest);
static void i915_ktest_pcode_preempt_region(struct i915_ktest *ktest);
static void i915_ktest_late_fuse(struct i915_ktest *ktest);

static void i915_fake_open(struct i915_mmio *mmio, struct i915_fake_mmio *fake);
static uint32_t i915_fake_gen_get(struct i915_fake_mmio *fake, uint32_t offset);
static void i915_fake_gen_set(struct i915_fake_mmio *fake, uint32_t offset, uint32_t value);
static uint32_t i915_fake_read32(void *context, uint32_t offset);
static void i915_fake_write32(void *context, uint32_t offset, uint32_t value);
static void i915_fake_pcode_post(struct i915_fake_mmio *fake);
static void i915_fake_forcewake_request(void *context, int domain, int wake);
static int i915_fake_forcewake_ack(void *context, int domain);

static int i915_ktest_spawn_detached(void (*function)(void *), void *argument);
static void i915_ktest_sleep_ms(unsigned milliseconds);
static void i915_ktest_completer_worker(void *argument);
static void i915_ktest_requeue_fn(void *argument);
static void i915_ktest_cancel_worker_fn(void *argument);
static void i915_ktest_canceller_thread(void *argument);
static void i915_ktest_xcpu_fn(void *argument);
static void i915_ktest_msi_handler(int irq, hal_irq_ack_t acknowledge, void *argument);
static void i915_ktest_sleep_corunner(void *argument);
static int i915_ktest_rpm_resume(void *context);
static void i915_ktest_rpm_suspend(void *context);

/*
 * The bus end of the register model.
 */
static const struct i915_mmio_ops i915_fake_mmio_ops = {
	i915_fake_read32,
	i915_fake_write32,
	i915_fake_forcewake_request,
	i915_fake_forcewake_ack
};

/*
 * The runtime-PM test device, whose resume fails on request.
 */
static const struct i915_rpm_ops i915_ktest_rpm_ops = {
	i915_ktest_rpm_resume,
	i915_ktest_rpm_suspend
};

/*
 * Runs the tests of the kernel services the driver waits and queues work on.
 *
 * Covers the completions, the work queue, the MSI vector lifecycle and the
 * write-combining attribute contract of the HAL, the reset and PCODE control
 * flow, the DRAM and bandwidth probe, the runtime-PM contract, the yielding
 * sleep-range, the PCODE preemption-off phase and the late fuse wait.
 */
void
drv_i915_ktest_sync(
	struct i915_ktest *ktest)
{
	unsigned cpu;

	/* Names the CPU the tests start on. */
	cpu = (unsigned)hal_cpu_current();
	kern_logf("i915: ktest sync begin (cpu=%u, shared backend)\n", cpu);

	/* Samples the time base once while it is fresh. */
	(void)drv_i915_time_base_ok();

	/* The native wait queue, then the completion built on it. */
	i915_ktest_waitq_deadline(ktest);
	i915_ktest_completions(ktest);

	/* The work queue. */
	i915_ktest_workqueue(ktest);

	/* The HAL interrupt vector lifecycle and the write-combining contract. */
	i915_ktest_msi(ktest);
	i915_ktest_wc_contract(ktest);

	/* The reset, PCODE and DRAM paths against the register model. */
	i915_ktest_reset(ktest);
	i915_ktest_pcode(ktest);
	i915_ktest_dram_decode(ktest);
	i915_ktest_dram_bw(ktest);

	/* A refused aperture map and the map that follows it. */
	i915_ktest_wc_full_range(ktest);

	/* A completion signalled from a timer interrupt. */
	i915_ktest_irq_oneshot(ktest);

	/* The runtime-PM contract of the PCI probe. */
	i915_ktest_rpm(ktest);

	/* The PCODE request with re-requests. */
	i915_ktest_pcode_request(ktest);

	/* The yielding sleep-range and the deferred reschedule. */
	i915_ktest_usleep_range(ktest);
	i915_ktest_preempt(ktest);

	/* The PCODE preemption-off phase and the late fuse wait. */
	i915_ktest_pcode_preempt_region(ktest);
	i915_ktest_late_fuse(ktest);
}

/* Checks that a native wait queue sleep ends at its timer deadline. */
static void
i915_ktest_waitq_deadline(
	struct i915_ktest *ktest)
{
	static struct spinlock lock;
	static struct wait_queue queue;
	uint64_t start_tick;
	uint64_t deadline;
	uint64_t end_tick;
	uint64_t observed;
	int slept;

	/* Sleeps on a queue nobody wakes, until the deadline 200 ms away. */
	spin_init(&lock, LOCK_RANK_DEVICE, "ktest-k0");
	waitq_init(&queue, "ktest-k0");
	start_tick = sched_ticks();
	deadline = drv_i915_ktest_deadline_ms(200U);
	spin_lock(&lock);

	observed = waitq_sequence(&queue);
	slept = waitq_sleep(&queue, &lock, observed, deadline, 0U);

	spin_unlock(&lock);

	/* Logs when the sleep ended against its deadline. */
	end_tick = sched_ticks();
	kern_logf("i915: ktest K0 native-waitq start=%llu deadline=%llu end=%llu rc=%d\n",
		  (unsigned long long)start_tick,
		  (unsigned long long)deadline,
		  (unsigned long long)end_tick,
		  slept);
	drv_i915_ktest_check(ktest, end_tick >= deadline, "K0: timer advanced past the deadline (native wait woke)");
}

/* Checks the counting completion: early signals, counting, cross-thread wake and timeout. */
static void
i915_ktest_completions(
	struct i915_ktest *ktest)
{
	int completed;
	int spawned;

	/* A signal before the wait lets the wait return at once. */
	drv_i915_completion_init(&i915_ktest_completion, "ktest-1");
	drv_i915_complete(&i915_ktest_completion);
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(1000U));
	drv_i915_ktest_check(ktest, completed == 1, "K1 complete-before-wait");

	/* Two signals allow exactly two waits; the third one times out. */
	drv_i915_completion_init(&i915_ktest_completion, "ktest-2");
	drv_i915_complete(&i915_ktest_completion);
	drv_i915_complete(&i915_ktest_completion);
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(1000U));
	drv_i915_ktest_check(ktest, completed == 1, "K2 first wait");
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(1000U));
	drv_i915_ktest_check(ktest, completed == 1, "K2 second wait");
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(200U));
	drv_i915_ktest_check(ktest, completed == 0, "K2 third wait times out");

	/* A blocked wait is woken by a signal from another thread. */
	drv_i915_completion_init(&i915_ktest_completion, "ktest-3");
	i915_ktest_completer_context.completion = &i915_ktest_completion;
	spawned = i915_ktest_spawn_detached(i915_ktest_completer_worker, &i915_ktest_completer_context);
	drv_i915_ktest_check(ktest, spawned == 0, "K3 spawn completer");
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(5000U));
	drv_i915_ktest_check(ktest, completed == 1, "K3 blocking wait woken by thread");

	/* A completion never signalled times out. */
	drv_i915_completion_init(&i915_ktest_completion, "ktest-4");
	completed = drv_i915_wait_for_completion(&i915_ktest_completion, drv_i915_ktest_deadline_ms(200U));
	drv_i915_ktest_check(ktest, completed == 0, "K4 timeout");
}

/* Checks the work queue: a self-requeue, a synchronous cancel and a cross-CPU publish. */
static void
i915_ktest_workqueue(
	struct i915_ktest *ktest)
{
	int created;

	/* Starts the queue and its worker thread. */
	created = drv_i915_workqueue_create(&i915_ktest_queue, "ktest-wq");
	drv_i915_ktest_check(ktest, created == 0, "WQ create worker thread");

	/* Exercises the queue. */
	i915_ktest_workqueue_requeue(ktest);
	i915_ktest_workqueue_cancel(ktest);
	i915_ktest_workqueue_xcpu(ktest);

	/* Stops the worker; the destroy returns only once it has left. */
	drv_i915_workqueue_destroy(&i915_ktest_queue);
	drv_i915_ktest_check(ktest, i915_ktest_queue.worker_alive == 0, "WQ worker reclaimed on destroy");
}

/* Checks that a running work that queues itself again runs once more. */
static void
i915_ktest_workqueue_requeue(
	struct i915_ktest *ktest)
{
	unsigned poll;

	/* Queues a work whose callback queues it again once. */
	drv_i915_work_init(&i915_ktest_work, i915_ktest_requeue_fn, &i915_ktest_requeue_context);
	i915_ktest_requeue_context.queue = &i915_ktest_queue;
	i915_ktest_requeue_context.self = &i915_ktest_work;
	(void)drv_i915_queue_work(&i915_ktest_queue, &i915_ktest_work);

	/* Waits, in bounded steps, for the second run. */
	for (poll = 0U; poll < 200U; poll++) {
		/* Stops polling once both runs happened. */
		if (i915_ktest_work.ran_count >= 2)
			break;

		i915_ktest_sleep_ms(10U);
	}

	drv_i915_ktest_check(ktest, i915_ktest_work.ran_count == 2, "WQ-requeue: self-requeue ran twice");
}

/* Checks that a synchronous cancel does not return before the running callback has. */
static void
i915_ktest_workqueue_cancel(
	struct i915_ktest *ktest)
{
	struct i915_ktest_cancel *shared;
	int completed;
	int spawned;

	/* Prepares the meeting point of the worker, the canceller and this thread. */
	shared = &i915_ktest_cancel_state;
	drv_i915_completion_init(&shared->worker_started, "cs-start");
	drv_i915_completion_init(&shared->release, "cs-release");
	drv_i915_completion_init(&shared->cancel_done, "cs-done");
	shared->worker_returned = 0;
	shared->order_ok = 0;

	/* Queues a work whose callback blocks until released. */
	drv_i915_work_init(&i915_ktest_work, i915_ktest_cancel_worker_fn, NULL);
	(void)drv_i915_queue_work(&i915_ktest_queue, &i915_ktest_work);
	completed = drv_i915_wait_for_completion(&shared->worker_started, drv_i915_ktest_deadline_ms(5000U));
	drv_i915_ktest_check(ktest, completed == 1, "cancel: worker started");

	/* Starts the thread that cancels the running work synchronously. */
	i915_ktest_canceller_context.queue = &i915_ktest_queue;
	i915_ktest_canceller_context.work = &i915_ktest_work;
	spawned = i915_ktest_spawn_detached(i915_ktest_canceller_thread, &i915_ktest_canceller_context);
	drv_i915_ktest_check(ktest, spawned == 0, "cancel: spawn canceller");

	/* Gives the canceller time to block in the cancel, then lets the callback return. */
	i915_ktest_sleep_ms(100U);
	drv_i915_complete(&shared->release);

	/* The cancel returned, and only after the callback had finished. */
	completed = drv_i915_wait_for_completion(&shared->cancel_done, drv_i915_ktest_deadline_ms(5000U));
	drv_i915_ktest_check(ktest, completed == 1, "cancel: canceller finished");
	drv_i915_ktest_check(ktest, shared->order_ok == 1, "cancel_work_sync did not return before the callback finished");
}

/* Checks that a worker on another CPU sees the payload published before the queue. */
static void
i915_ktest_workqueue_xcpu(
	struct i915_ktest *ktest)
{
	struct i915_ktest_xcpu *shared;
	unsigned generation;
	unsigned diff_cpu;
	unsigned mismatch;
	unsigned queue_fail;
	int queued;

	/* Runs one work per generation, each with its own payload. */
	shared = &i915_ktest_xcpu_state;
	diff_cpu = 0U;
	mismatch = 0U;
	queue_fail = 0U;
	drv_i915_completion_init(&shared->done, "xcpu");
	for (generation = 1U; generation <= I915_KTEST_XCPU_GENS; generation++) {
		/* Publishes the payload before the queue. */
		drv_i915_work_init(&i915_ktest_xcpu_works[generation - 1U], i915_ktest_xcpu_fn, NULL);
		drv_i915_reinit_completion(&shared->done);
		shared->payload = generation ^ I915_KTEST_XCPU_MAGIC;
		shared->queuer_cpu = (unsigned)hal_cpu_current();
		shared->worker_cpu = 0xffffU;
		shared->seen_payload = 0U;

		/* Queues the work; a refused queue is counted and the generation skipped. */
		queued = drv_i915_queue_work(&i915_ktest_queue, &i915_ktest_xcpu_works[generation - 1U]);
		if (queued != 1) {
			queue_fail++;
			continue;
		}

		/* Waits for the callback and compares what it saw. */
		(void)drv_i915_wait_for_completion(&shared->done, drv_i915_ktest_deadline_ms(5000U));
		if (shared->seen_payload != (generation ^ I915_KTEST_XCPU_MAGIC))
			mismatch++;

		/* Counts the generations whose callback ran on another CPU. */
		if (shared->worker_cpu != shared->queuer_cpu)
			diff_cpu++;
	}

	/* Logs and checks the outcome. */
	kern_logf("i915: ktest cross-CPU: gens=%u cross_cpu=%u mismatch=%u queue_fail=%u\n",
		  I915_KTEST_XCPU_GENS,
		  diff_cpu,
		  mismatch,
		  queue_fail);
	drv_i915_ktest_check(ktest, mismatch == 0U, "cross-CPU: worker saw the published payload every generation");
	drv_i915_ktest_check(ktest, queue_fail == 0U, "cross-CPU: every queue succeeded");
}

/* Checks the HAL MSI vector lifecycle on a made-up source. */
static void
i915_ktest_msi(
	struct i915_ktest *ktest)
{
	int irq;
	paddr_t address;
	uint32_t event;
	int result;

	/* A vector allocated with no handler is released again. */
	irq = -1;
	address = 0;
	event = 0U;
	result = hal_irq_alloc_msi(I915_KTEST_MSI_SOURCE, &irq, &address, &event);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M0: alloc_msi reserves a vector");
	drv_i915_ktest_check(ktest, address != 0, "MSI M0: alloc_msi returns a message address");
	result = hal_irq_free_msi(irq);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M0: free_msi releases an unattached vector");

	/* An attach validates its handler. */
	result = hal_irq_alloc_msi(I915_KTEST_MSI_SOURCE, &irq, &address, &event);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M1: alloc");
	result = hal_irq_attach_msi(irq, NULL, NULL);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "MSI M1: attach rejects a NULL handler");
	result = hal_irq_free_msi(irq);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M1: free after a failed attach");

	/* The full attach, detach and free lifecycle, and two live vectors. */
	i915_ktest_msi_attach(ktest);
	i915_ktest_msi_distinct(ktest);
}

/* Checks the attach, detach and free lifecycle of one vector and its state gates. */
static void
i915_ktest_msi_attach(
	struct i915_ktest *ktest)
{
	int irq;
	paddr_t address;
	uint32_t event;
	int result;

	/* Allocates a vector and attaches a handler that never runs. */
	irq = -1;
	address = 0;
	event = 0U;
	result = hal_irq_alloc_msi(I915_KTEST_MSI_SOURCE, &irq, &address, &event);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M2: alloc");
	result = hal_irq_attach_msi(irq, i915_ktest_msi_handler, &i915_ktest_msi_token);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M2: attach a handler");

	/* A free while attached, and a detach of another registration, are refused. */
	result = hal_irq_free_msi(irq);
	drv_i915_ktest_check(ktest, result == HAL_ERR_STATE, "MSI M2: free is refused while a handler is attached");
	result = hal_irq_detach_msi_sync(irq, i915_ktest_msi_handler, &i915_ktest_msi_other_token);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "MSI M2: detach rejects a mismatched registration");

	/* The exact registration detaches, and then the vector frees. */
	result = hal_irq_detach_msi_sync(irq, i915_ktest_msi_handler, &i915_ktest_msi_token);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M2: detach_sync drains the exact registration");
	result = hal_irq_free_msi(irq);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M2: free after detach");
}

/* Checks that two live vectors differ and that a released vector is refused. */
static void
i915_ktest_msi_distinct(
	struct i915_ktest *ktest)
{
	int irq_a;
	int irq_b;
	paddr_t address_a;
	paddr_t address_b;
	uint32_t event_a;
	uint32_t event_b;
	int result;

	/* Allocates two vectors at once. */
	irq_a = -1;
	irq_b = -1;
	address_a = 0;
	address_b = 0;
	event_a = 0U;
	event_b = 0U;
	result = hal_irq_alloc_msi(I915_KTEST_MSI_SOURCE, &irq_a, &address_a, &event_a);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M3: alloc A");
	result = hal_irq_alloc_msi(I915_KTEST_MSI_SOURCE, &irq_b, &address_b, &event_b);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M3: alloc B");
	drv_i915_ktest_check(ktest, irq_a != irq_b, "MSI M3: distinct allocations get distinct vectors");

	/* Releases both. */
	result = hal_irq_free_msi(irq_a);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M3: free A");
	result = hal_irq_free_msi(irq_b);
	drv_i915_ktest_check(ktest, result == HAL_OK, "MSI M3: free B");

	/* A released vector can be neither freed nor detached again. */
	result = hal_irq_free_msi(irq_a);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "MSI M4: free of an already-released vector is refused");
	result = hal_irq_detach_msi_sync(irq_a, i915_ktest_msi_handler, &i915_ktest_msi_token);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "MSI M4: detach of an unallocated vector is refused");
}

/* Checks that the HAL rejects a write-combining map combined with another cache policy. */
static void
i915_ktest_wc_contract(
	struct i915_ktest *ktest)
{
	void *mapped;
	int result;

	/* Write combining and uncached together are refused before anything is mapped. */
	mapped = NULL;
	result = hal_space_map_device(I915_KTEST_WC_ADDRESS,
				      0x1000U,
				      HAL_SPACE_READ | HAL_SPACE_WC | HAL_SPACE_NOCACHE,
				      &mapped);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "WC: a WC request combined with NOCACHE is rejected");
	drv_i915_ktest_check(ktest, mapped == NULL, "WC: a rejected request leaves the output pointer untouched");

	/* Write combining and device memory together are refused. */
	result = hal_space_map_device(I915_KTEST_WC_ADDRESS,
				      0x1000U,
				      HAL_SPACE_READ | HAL_SPACE_WC | HAL_SPACE_DEVICE,
				      &mapped);
	drv_i915_ktest_check(ktest, result == HAL_ERR_INVALID, "WC: a WC request combined with DEVICE is rejected");
}

/* Checks the reset control flow: acknowledged, never acknowledged, and a retry. */
static void
i915_ktest_reset(
	struct i915_ktest *ktest)
{
	struct spinlock uncore_lock;
	int error;

	/* The reset succeeds when the domain acknowledges at once. */
	spin_init(&uncore_lock, LOCK_RANK_DEVICE, "ktest-uncore");
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.gdrst_fail_writes = 0U;
	error = drv_i915_gt_reset_all(&uncore_lock, &i915_ktest_mmio, 2000U);
	drv_i915_ktest_check(ktest, error == 0, "reset: succeeds when the domain acks");

	/* The reset times out when no acknowledge ever comes. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.gdrst_fail_writes = 999U;
	error = drv_i915_gt_reset_all(&uncore_lock, &i915_ktest_mmio, 2000U);
	drv_i915_ktest_check(ktest, error == ETIMEDOUT, "reset: times out when the ack never arrives");

	/* The reset recovers on the retry after one attempt that timed out. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.gdrst_fail_writes = 1U;
	error = drv_i915_gt_reset_all(&uncore_lock, &i915_ktest_mmio, 2000U);
	drv_i915_ktest_check(ktest, error == 0, "reset: recovers on a retry after one failed attempt");
}

/* Checks one PCODE read transaction: a response, a busy mailbox and a timeout status. */
static void
i915_ktest_pcode(
	struct i915_ktest *ktest)
{
	static const uint32_t ok_script[1][3] = { { 0x0000abcdU, 0U, 0x0U } };
	static const uint32_t err_script[1][3] = { { 0U, 0U, 0x2U } };
	struct mutex sb_lock;
	uint32_t value;
	int error;

	/* A completed transaction hands back the response word. */
	(void)mutex_init(&sb_lock, LOCK_RANK_DEVICE, "ktest-sb");
	value = 0U;
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = ok_script;
	i915_ktest_fake.script_len = 1U;
	error = drv_i915_pcode_read(&sb_lock, &i915_ktest_mmio, 0x0000000dU, &value, NULL);
	drv_i915_ktest_check(ktest, error == 0 && value == 0x0000abcdU, "pcode: success returns the response word");

	/* A mailbox still busy with a previous command refuses the transaction. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.mailbox = I915_KTEST_PCODE_READY;
	error = drv_i915_pcode_read(&sb_lock, &i915_ktest_mmio, 0x0000000dU, &value, NULL);
	drv_i915_ktest_check(ktest, error == EAGAIN, "pcode: a busy mailbox yields EAGAIN");

	/* The firmware's timeout status becomes ETIMEDOUT. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = err_script;
	i915_ktest_fake.script_len = 1U;
	error = drv_i915_pcode_read(&sb_lock, &i915_ktest_mmio, 0x0000000dU, &value, NULL);
	drv_i915_ktest_check(ktest, error == ETIMEDOUT, "pcode: a mailbox timeout status maps to ETIMEDOUT");
}

/* Checks the decode of the PCODE global memory information word. */
static void
i915_ktest_dram_decode(
	struct i915_ktest *ktest)
{
	struct i915_dram_info info;
	int error;

	/* A DDR4 word with two channels, four QGV points and three PSF points decodes. */
	kern_memset(&info, 0, sizeof(info));
	error = drv_i915_dram_decode(0x00003420U, &info);
	drv_i915_ktest_check(ktest, error == 0, "dram_decode: valid word decodes");
	drv_i915_ktest_check(ktest,
			     info.type == (int)I915_DRAM_DDR4 &&
			     info.num_channels == 2U &&
			     info.num_qgv_points == 4U &&
			     info.num_psf_gv_points == 3U,
			     "dram_decode: DDR4/2ch/4qgv/3psf fields");

	/* An unknown type field is refused. */
	error = drv_i915_dram_decode(0x0000000fU, &info);
	drv_i915_ktest_check(ktest, error == EINVAL, "dram_decode: an unknown type field is rejected");
}

/* Checks the DRAM detect and the bandwidth table it feeds, as saved in the caller's state. */
static void
i915_ktest_dram_bw(
	struct i915_ktest *ktest)
{
	static const uint32_t ok_seq[6][3] = {
		{ 0x00003420U, 0U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x00302010U, 0U, 0U },
	};
	static struct i915_bw_state bw;
	struct mutex sb_lock;
	struct i915_dram_info info;
	int error;

	/* Detects the DRAM over the scripted PCODE. */
	(void)mutex_init(&sb_lock, LOCK_RANK_DEVICE, "ktest-sb2");
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = ok_seq;
	i915_ktest_fake.script_len = 6U;
	kern_memset(&info, 0, sizeof(info));
	kern_memset(&bw, 0, sizeof(bw));
	error = drv_i915_dram_detect(&sb_lock, &i915_ktest_mmio, &info);
	drv_i915_ktest_check(ktest, error == 0 && info.num_qgv_points == 4U, "dram_detect: decodes global info over PCODE");

	/* Computes the bandwidth table from the four QGV points and the PSF points. */
	error = drv_i915_bw_init_hw(&sb_lock, &i915_ktest_mmio, &info, &bw);
	drv_i915_ktest_check(ktest, error == 0, "bw_init: returns success");
	drv_i915_ktest_check(ktest, bw.valid == 1 && bw.sagv_status == (int)I915_SAGV_ENABLED, "bw_init: saved SAGV status is enabled");
	drv_i915_ktest_check(ktest,
			     bw.max[0].num_qgv_points == 4U && bw.max[0].num_psf_gv_points == 3U,
			     "bw_init: saved per-group point counts");
	drv_i915_ktest_check(ktest,
			     bw.max[0].deratedbw[0] != 0U && bw.max[0].peakbw[0] != 0U,
			     "bw_init: saved group-0 bandwidth values");
	drv_i915_ktest_check(ktest,
			     bw.max[0].num_planes == 0U && bw.max[1].num_planes != 0U,
			     "bw_init: num_planes stored into the NEXT group");

	/* A failed PSF read is tolerated. */
	i915_ktest_dram_bw_psf_failure(ktest, &sb_lock);
}

/* Checks that a failed PSF point read is tolerated and leaves no PSF points. */
static void
i915_ktest_dram_bw_psf_failure(
	struct i915_ktest *ktest,
	struct mutex *sb_lock)
{
	static const uint32_t psf_fail_seq[6][3] = {
		{ 0x00003420U, 0U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0x000f03e8U, 0x00000804U, 0U },
		{ 0U, 0U, 0x1U },
	};
	static struct i915_bw_state bw;
	struct i915_dram_info info;
	int error;

	/* Detects the DRAM, then computes the table while the PSF read fails. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = psf_fail_seq;
	i915_ktest_fake.script_len = 6U;
	kern_memset(&info, 0, sizeof(info));
	kern_memset(&bw, 0, sizeof(bw));
	(void)drv_i915_dram_detect(sb_lock, &i915_ktest_mmio, &info);
	error = drv_i915_bw_init_hw(sb_lock, &i915_ktest_mmio, &info, &bw);
	drv_i915_ktest_check(ktest, error == 0, "bw_init: a PSF read failure is tolerated");
	drv_i915_ktest_check(ktest, bw.max[0].num_psf_gv_points == 0U, "bw_init: PSF failure reflected in the saved num_psf_gv_points");
}

/* Checks that an oversized aperture map is refused and a valid map still works afterwards. */
static void
i915_ktest_wc_full_range(
	struct i915_ktest *ktest)
{
	void *mapped;
	void *recovered;
	int refused;
	int result;

	/* A one-gigabyte write-combining map is refused and leaves the output alone. */
	mapped = (void *)0x1;
	refused = hal_space_map_device(I915_KTEST_WC_ADDRESS,
				       0x40000000U,
				       HAL_SPACE_READ | HAL_SPACE_WC,
				       &mapped);
	drv_i915_ktest_check(ktest, refused != HAL_OK, "WC: an oversized aperture map is refused");
	drv_i915_ktest_check(ktest, mapped == (void *)0x1, "WC: a refused map leaves the output untouched");

	/* A one-page write-combining map succeeds afterwards. */
	recovered = NULL;
	result = hal_space_map_device(I915_KTEST_WC_ADDRESS,
				      0x1000U,
				      HAL_SPACE_READ | HAL_SPACE_WC,
				      &recovered);
	drv_i915_ktest_check(ktest, result == HAL_OK && recovered != NULL, "WC: a valid map succeeds after the failed one (recovery)");

	/* Removes the page mapped by the recovery check. */
	if (result == HAL_OK)
		(void)hal_space_unmap_device(recovered, 0x1000U);
}

/* Records the timer-interrupt completion checks as not run: the kernel hook is not built. */
static void
i915_ktest_irq_oneshot(
	struct i915_ktest *ktest)
{
	/*
	 * kern_diag_oneshot_arm(), which runs a callback from the next timer
	 * interrupt, is built only in the retired diagnostic kernel
	 * configuration.
	 */
	drv_i915_ktest_skip(ktest,
			    "IRQ oneshot: shared completion signalled from a timer IRQ (cross-CPU)",
			    "the kernel's timer one-shot hook is not built");
	drv_i915_ktest_skip(ktest,
			    "IRQ oneshot: fired once in IRQ context, cross-CPU, with the published tag",
			    "the kernel's timer one-shot hook is not built");
	drv_i915_ktest_skip(ktest,
			    "IRQ oneshot: completion signalled from the waiter's OWN CPU timer IRQ",
			    "the kernel's timer one-shot hook is not built");
	drv_i915_ktest_skip(ktest,
			    "IRQ oneshot: fired in IRQ context on the SAME CPU as the waiter",
			    "the kernel's timer one-shot hook is not built");
}

/* Checks the runtime-PM contract of the PCI probe against a test device. */
static void
i915_ktest_rpm(
	struct i915_ktest *ktest)
{
	struct i915_rpm rpm;
	int usage;
	int active;
	int error;

	/* A get_sync resumes the device and holds a reference; the put drops it. */
	drv_i915_trace_init(&i915_ktest_trace);
	i915_ktest_rpm_fail = 0;
	drv_i915_rpm_init_early(&rpm, &i915_ktest_rpm_ops, NULL, &i915_ktest_trace);
	error = drv_i915_rpm_get_sync(&rpm);
	usage = drv_i915_rpm_usage(&rpm);
	active = drv_i915_rpm_active(&rpm);
	drv_i915_ktest_check(ktest,
			     error == 0 &&
			     usage == 1 &&
			     active == 1,
			     "rpm: get_sync resumes and holds a usage reference");
	drv_i915_rpm_put(&rpm);
	usage = drv_i915_rpm_usage(&rpm);
	drv_i915_ktest_check(ktest, usage == 0, "rpm: put drops the usage reference");

	/* A get_sync whose resume fails keeps the reference, which the caller must put. */
	i915_ktest_rpm_fail = 1;
	drv_i915_rpm_init_early(&rpm, &i915_ktest_rpm_ops, NULL, &i915_ktest_trace);
	rpm.active = 0;
	error = drv_i915_rpm_get_sync(&rpm);
	usage = drv_i915_rpm_usage(&rpm);
	drv_i915_ktest_check(ktest, error != 0 && usage == 1, "rpm: get_sync leaves usage incremented on resume failure (caller must put)");
	drv_i915_rpm_put(&rpm);

	/* A resume_and_get whose resume fails gives the reference back. */
	i915_ktest_rpm_fail = 1;
	drv_i915_rpm_init_early(&rpm, &i915_ktest_rpm_ops, NULL, &i915_ktest_trace);
	rpm.active = 0;
	error = drv_i915_rpm_resume_and_get(&rpm);
	usage = drv_i915_rpm_usage(&rpm);
	drv_i915_ktest_check(ktest, error != 0 && usage == 0, "rpm: resume_and_get unwinds usage on failure (distinct from get_sync)");
	i915_ktest_rpm_fail = 0;
}

/* Checks skl_pcode_request(): a first reply that matches, re-requests, and a sticky error. */
static void
i915_ktest_pcode_request(
	struct i915_ktest *ktest)
{
	static const uint32_t sc1[1][3] = { { 0x1U, 0U, 0x0U } };
	static const uint32_t sc2[3][3] = { { 0x0U, 0U, 0x0U }, { 0x0U, 0U, 0x0U }, { 0x1U, 0U, 0x0U } };
	struct mutex sb_lock;
	int error;

	/* A first reply that already matches sends no second request. */
	(void)mutex_init(&sb_lock, LOCK_RANK_DEVICE, "ktest-sb");
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = sc1;
	i915_ktest_fake.script_len = 1U;
	error = drv_i915_skl_pcode_request(&sb_lock, &i915_ktest_mmio, 0x7U, 0U, 0x1U, 0x1U, 100);
	drv_i915_ktest_check(ktest,
			     error == 0 && i915_ktest_fake.txn == 1U,
			     "pcode: skl_pcode_request first request satisfies reply (no re-request)");

	/* Each unmatched reply is followed by a fresh request until one matches. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.script = sc2;
	i915_ktest_fake.script_len = 3U;
	error = drv_i915_skl_pcode_request(&sb_lock, &i915_ktest_mmio, 0x7U, 0U, 0x1U, 0x1U, 100);
	drv_i915_ktest_check(ktest,
			     error == 0 && i915_ktest_fake.txn == 3U,
			     "pcode: skl_pcode_request re-requests until the reply matches");

	/* A persistent illegal-command status is reported as such, not as a timeout. */
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.pcode_sticky_status = 0x1;
	error = drv_i915_skl_pcode_request(&sb_lock, &i915_ktest_mmio, 0x7U, 0U, 0x1U, 0x1U, 1);
	drv_i915_ktest_check(ktest,
			     error != 0 &&
			     error != EIO &&
			     error != ETIMEDOUT,
			     "pcode: a persistent PCODE error status is returned (not a timeout/anomaly)");
}

/* Checks that kern_usleep_range() yields the CPU for the whole range. */
static void
i915_ktest_usleep_range(
	struct i915_ktest *ktest)
{
	uint64_t counter_before;
	uint64_t counter_after;
	uint64_t frequency;
	uint64_t frequency_after;
	uint64_t tick_before;
	uint64_t tick_after;
	uint64_t elapsed_us;
	unsigned long corun_before;
	unsigned long corun_after;
	int spawned;

	/* Starts a co-runner that makes progress whenever it gets a CPU. */
	i915_ktest_corun_counter = 0U;
	i915_ktest_corun_stop = 0;
	spawned = i915_ktest_spawn_detached(i915_ktest_sleep_corunner, NULL);
	if (spawned != 0) {
		drv_i915_ktest_check(ktest, 0, "usleep_range: could not spawn the co-runner");
		return;
	}

	/* Lets the co-runner start. */
	kern_usleep_range(2000U, 3000U);

	/* Sleeps 20 to 25 ms and measures the time, the ticks and the co-runner's progress. */
	counter_before = 0U;
	counter_after = 0U;
	frequency = 0U;
	frequency_after = 0U;
	corun_before = i915_ktest_corun_counter;
	tick_before = sched_ticks();
	(void)kern_rtc_read_counter(&counter_before, &frequency);
	kern_usleep_range(20000U, 25000U);
	(void)kern_rtc_read_counter(&counter_after, &frequency_after);
	tick_after = sched_ticks();
	corun_after = i915_ktest_corun_counter;
	i915_ktest_corun_stop = 1;

	/* Converts the measured counter span into microseconds when the rate held. */
	elapsed_us = 0U;
	if (frequency != 0U && frequency_after == frequency)
		elapsed_us = (counter_after - counter_before) * 1000000U / frequency;

	/*
	 * The minimum held (a little slack allowed), the sleep crossed at least
	 * one scheduler tick rather than spinning, and the co-runner ran.
	 */
	drv_i915_ktest_check(ktest,
			     elapsed_us >= 19000U &&
			     (tick_after - tick_before) >= 1U &&
			     corun_after > corun_before,
			     "usleep_range: 20-25ms yields the CPU (>=19ms elapsed, crossed a tick, co-runner ran)");
}

/* Records the deferred-reschedule check as not run: the kernel hook is not built. */
static void
i915_ktest_preempt(
	struct i915_ktest *ktest)
{
	/* The check raises its reschedule from kern_diag_oneshot_arm(), which is not built. */
	drv_i915_ktest_skip(ktest,
			    "preempt: real IRQ reschedule is deferred while preempt-disabled, runs at outermost enable",
			    "the kernel's timer one-shot hook is not built");
}

/* Checks that the PCODE request's preemption-off phase runs and restores preemption and the lock. */
static void
i915_ktest_pcode_preempt_region(
	struct i915_ktest *ktest)
{
	struct mutex sb_lock;
	unsigned preempt_count;
	int locked;
	int error;

	/*
	 * The model approves a request only while preemption is off, so the
	 * ordinary phase never succeeds and the preemption-off phase does.
	 */
	(void)mutex_init(&sb_lock, LOCK_RANK_DEVICE, "pcode-add-a");
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	i915_ktest_fake.pcode_approve_when_preempt = 1;
	error = drv_i915_skl_pcode_request(&sb_lock, &i915_ktest_mmio, 0x7U, 0x3U, 0x1U, 0x1U, 1);

	/* Samples the preemption depth and whether the lock was given back. */
	preempt_count = sched_test_preempt_count();
	locked = mutex_trylock(&sb_lock);
	drv_i915_ktest_check(ktest,
			     error == 0 &&
			     i915_ktest_fake.pcode_txn_count >= 3U &&
			     preempt_count == 0U &&
			     locked != 0,
			     "pcode: normal region unapproved -> additional (preempt-off) region approves; preempt/mutex restored");

	/* Gives back the lock the check took. */
	if (locked != 0)
		mutex_unlock(&sb_lock);
}

/* Checks the power-well fuse wait: fuses that arrive late, and fuses that never arrive. */
static void
i915_ktest_late_fuse(
	struct i915_ktest *ktest)
{
	struct i915_pw_ctx pwc;
	struct i915_power_well *well;
	uint32_t fuse;
	int enabled;
	int index;
	int error;

	/* Builds a display-13 power-well map of its own, reached through the model. */
	drv_i915_trace_init(&i915_ktest_trace);
	i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
	(void)drv_i915_power_domains_init(&i915_ktest_power_domains, 13U, -1, 1, &i915_ktest_trace);
	kern_memset(&pwc, 0, sizeof(pwc));
	pwc.mmio = &i915_ktest_mmio;
	index = drv_i915_power_well_by_id(&i915_ktest_power_domains, I915_SKL_DISP_PW_1);

	/*
	 * PG0 is distributed now; PG1 only after eight fuse reads.  Without
	 * power well 1 in the map the check fails.
	 */
	error = EINVAL;
	enabled = 0;
	fuse = 0U;
	if (index >= 0) {
		well = &i915_ktest_power_domains.power_wells[index];
		i915_ktest_fake.fuse_status = I915_KTEST_FUSE_PG0;
		i915_ktest_fake.fuse_delay = 8U;
		i915_ktest_fake.fuse_delay_bits = I915_KTEST_FUSE_PG1;
		error = drv_i915_power_well_enable(well, &pwc);
		enabled = well->hw_enabled;
		fuse = drv_i915_raw_read32(&i915_ktest_mmio, I915_KTEST_REG_FUSE_STATUS);
	}

	drv_i915_ktest_check(ktest,
			     error == 0 &&
			     enabled == 1 &&
			     (fuse & I915_KTEST_FUSE_PG1) != 0U,
			     "fuse: PG1 arrives late -> the fuse wait (fast 2us then slow 1ms) observes it and continues");

	/* PG1 never distributes: the timeout is warned and the enable goes on. */
	error = EINVAL;
	enabled = 0;
	if (index >= 0) {
		well = &i915_ktest_power_domains.power_wells[index];
		i915_fake_open(&i915_ktest_mmio, &i915_ktest_fake);
		i915_ktest_fake.fuse_status = I915_KTEST_FUSE_PG0;
		well->hw_enabled = -1;
		error = drv_i915_power_well_enable(well, &pwc);
		enabled = well->hw_enabled;
	}

	drv_i915_ktest_check(ktest, error == 0 && enabled == 1, "fuse: PG1 never distributes -> HW timeout warned, enable continues (not EIO)");
}

/* Binds the register block to a cleared model. */
static void
i915_fake_open(
	struct i915_mmio *mmio,
	struct i915_fake_mmio *fake)
{
	/* Clears the model and binds the block to it with no domain map and no trace. */
	kern_memset(fake, 0, sizeof(*fake));
	drv_i915_mmio_init(mmio, &i915_fake_mmio_ops, fake, NULL, 0U, NULL);
}

/* Reads a register the model has no special meaning for; zero when never written. */
static uint32_t
i915_fake_gen_get(
	struct i915_fake_mmio *fake,
	uint32_t offset)
{
	unsigned index;

	/* Looks the offset up among the written registers. */
	for (index = 0U; index < fake->gen_n; index++) {
		if (fake->gen_off[index] == offset)
			return fake->gen_val[index];
	}

	/* A register never written reads zero. */
	return 0U;
}

/* Stores a register the model has no special meaning for. */
static void
i915_fake_gen_set(
	struct i915_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	unsigned index;

	/* Replaces the value of a register written before. */
	for (index = 0U; index < fake->gen_n; index++) {
		if (fake->gen_off[index] == offset) {
			fake->gen_val[index] = value;
			return;
		}
	}

	/* Remembers a new register while there is room; later ones are dropped. */
	if (fake->gen_n < I915_KTEST_FAKE_REGS) {
		fake->gen_off[fake->gen_n] = offset;
		fake->gen_val[fake->gen_n] = value;
		fake->gen_n++;
	}
}

/* Reads one register of the model. */
static uint32_t
i915_fake_read32(
	void *context,
	uint32_t offset)
{
	struct i915_fake_mmio *fake;
	uint32_t state;

	fake = context;

	/* Answers each register the model gives a meaning to. */
	switch (offset) {
	case I915_KTEST_REG_FUSE_STATUS:
		/* Bits that arrive late appear once their read count runs out. */
		if (fake->fuse_delay > 0U) {
			fake->fuse_delay--;
			if (fake->fuse_delay == 0U)
				fake->fuse_status |= fake->fuse_delay_bits;
		}

		return fake->fuse_status;
	case I915_KTEST_REG_GDRST:
		/* The first requests read back as still pending, the later ones as done. */
		if (fake->gdrst_writes <= fake->gdrst_fail_writes)
			return 0x1U;

		return 0x0U;
	case I915_KTEST_REG_PCODE_MAILBOX:
		return fake->mailbox;
	case I915_KTEST_REG_PCODE_DATA:
		return fake->data;
	case I915_KTEST_REG_PCODE_DATA1:
		return fake->data1;
	case I915_KTEST_REG_PWR_WELL_CTL2:
		/* Every requested well reports its state bit at once. */
		state = (fake->pw_hsw_req & I915_KTEST_PW_REQUEST_BITS) >> 1;
		return fake->pw_hsw_req | state;
	default:
		break;
	}

	/* Every other register reads what was last written to it. */
	return i915_fake_gen_get(fake, offset);
}

/* Writes one register of the model. */
static void
i915_fake_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_fake_mmio *fake;

	fake = context;

	/* Applies each register the model gives a meaning to. */
	switch (offset) {
	case I915_KTEST_REG_GDRST:
		fake->gdrst_writes++;
		return;
	case I915_KTEST_REG_PCODE_DATA:
		fake->data = value;
		return;
	case I915_KTEST_REG_PCODE_DATA1:
		fake->data1 = value;
		return;
	case I915_KTEST_REG_PCODE_MAILBOX:
		i915_fake_pcode_post(fake);
		return;
	case I915_KTEST_REG_PWR_WELL_CTL2:
		fake->pw_hsw_req = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	default:
		break;
	}

	/* Every other register keeps what was written. */
	i915_fake_gen_set(fake, offset, value);
}

/* Completes the PCODE command just posted, leaving READY clear and the status byte set. */
static void
i915_fake_pcode_post(
	struct i915_fake_mmio *fake)
{
	/* A sticky status ends every transaction with it. */
	if (fake->pcode_sticky_status != 0) {
		fake->mailbox = (uint32_t)fake->pcode_sticky_status & 0xffU;
		return;
	}

	/* Counts the transactions the remaining models answer. */
	fake->pcode_txn_count++;

	/* The approval model replies 1 only while preemption is disabled. */
	if (fake->pcode_approve_when_preempt != 0) {
		fake->data = 0x0U;
		if (sched_test_preempt_count() > 0U)
			fake->data = 0x1U;

		fake->mailbox = 0U;
		return;
	}

	/* The script answers one transaction per entry, and plain success after its end. */
	if (fake->txn < fake->script_len) {
		fake->data = fake->script[fake->txn][0];
		fake->data1 = fake->script[fake->txn][1];
		fake->mailbox = fake->script[fake->txn][2] & 0xffU;
		fake->txn++;
	} else {
		fake->mailbox = 0U;
	}
}

/* Accepts a forcewake request; the model has no sleeping domain. */
static void
i915_fake_forcewake_request(
	void *context,
	int domain,
	int wake)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);
	UNUSED_PARAMETER(wake);
}

/* Acknowledges every forcewake request at once. */
static int
i915_fake_forcewake_ack(
	void *context,
	int domain)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);

	/* Every domain is awake. */
	return 1;
}

/* Starts a kernel thread that reclaims itself when it returns. */
static int
i915_ktest_spawn_detached(
	void (*function)(void *),
	void *argument)
{
	struct thread *thread;
	int error;

	/* Creates the thread. */
	error = kthread_create(function, argument, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* Nobody joins it, so it reclaims itself; then it starts. */
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread runs. */
	return 0;
}

/* Sleeps for a number of milliseconds on a completion nobody signals. */
static void
i915_ktest_sleep_ms(
	unsigned milliseconds)
{
	static struct i915_completion idle;

	/* Waits out the deadline. */
	drv_i915_completion_init(&idle, "ktest-idle");
	(void)drv_i915_wait_for_completion(&idle, drv_i915_ktest_deadline_ms(milliseconds));
}

/* Signals the completion of the completer test once. */
static void
i915_ktest_completer_worker(
	void *argument)
{
	struct i915_ktest_completer *completer;

	completer = argument;

	/* Wakes the waiting test. */
	drv_i915_complete(completer->completion);
}

/* Queues the running work again until it has run twice. */
static void
i915_ktest_requeue_fn(
	void *argument)
{
	struct i915_ktest_requeue *requeue;

	requeue = argument;

	/* The first run queues the work once more. */
	if (requeue->self->ran_count < 2)
		(void)drv_i915_queue_work(requeue->queue, requeue->self);
}

/* Runs as the work being cancelled: blocks until released, then marks its return. */
static void
i915_ktest_cancel_worker_fn(
	void *argument)
{
	struct i915_ktest_cancel *shared;

	UNUSED_PARAMETER(argument);

	/* Tells the test the callback runs, and waits to be let go. */
	shared = &i915_ktest_cancel_state;
	drv_i915_complete(&shared->worker_started);
	(void)drv_i915_wait_for_completion(&shared->release, drv_i915_ktest_deadline_ms(5000U));

	/* The callback is about to return. */
	shared->worker_returned = 1;
}

/* Cancels the running work synchronously and records whether its callback had returned. */
static void
i915_ktest_canceller_thread(
	void *argument)
{
	struct i915_ktest_canceller *canceller;
	struct i915_ktest_cancel *shared;

	canceller = argument;
	shared = &i915_ktest_cancel_state;

	/* Waits until the callback runs, then blocks in the cancel until it returns. */
	(void)drv_i915_wait_for_completion(&shared->worker_started, drv_i915_ktest_deadline_ms(5000U));
	(void)drv_i915_cancel_work_sync(canceller->queue, canceller->work, drv_i915_ktest_deadline_ms(5000U));

	/* The callback must have set its flag by now. */
	shared->order_ok = shared->worker_returned;
	drv_i915_complete(&shared->cancel_done);
}

/* Reads the published payload first thing, then reports where the callback ran. */
static void
i915_ktest_xcpu_fn(
	void *argument)
{
	struct i915_ktest_xcpu *shared;
	uint32_t payload;

	UNUSED_PARAMETER(argument);

	/* Reads the payload before anything else can order it. */
	shared = &i915_ktest_xcpu_state;
	payload = shared->payload;
	shared->seen_payload = payload;
	shared->worker_cpu = (unsigned)hal_cpu_current();

	/* Wakes the test. */
	drv_i915_complete(&shared->done);
}

/* Serves as the handler of the MSI lifecycle test; the made-up source never raises it. */
static void
i915_ktest_msi_handler(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(acknowledge);
	UNUSED_PARAMETER(argument);
}

/* Makes progress while the sleep-range test sleeps, until told to stop. */
static void
i915_ktest_sleep_corunner(
	void *argument)
{
	UNUSED_PARAMETER(argument);

	/* Counts and sleeps briefly until the test has measured. */
	while (!i915_ktest_corun_stop) {
		i915_ktest_corun_counter++;
		kern_usleep_range(100U, 200U);
	}
}

/* Resumes the runtime-PM test device, failing when the test asks for it. */
static int
i915_ktest_rpm_resume(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* A requested failure is an I/O error. */
	if (i915_ktest_rpm_fail != 0)
		return EIO;

	/* Succeeded: the test device is in D0. */
	return 0;
}

/* Suspends the runtime-PM test device, which needs nothing. */
static void
i915_ktest_rpm_suspend(
	void *context)
{
	UNUSED_PARAMETER(context);
}
