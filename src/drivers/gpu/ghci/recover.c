/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device's service thread: the recovery from faults, watchdog
 * expiries and hangs, the hang check, the loss of a device, and the poll
 * thread of a device without an interrupt.
 *
 * Every fault is recovered by a reset of the device, whatever
 * STATUS.FAULTED says.  The recovery, which only the service thread
 * runs, goes in steps: it takes the fault records, halts the processor and reads what it latched, resets the
 * device and checks the registers the reset keeps, ends every ticket the
 * reset threw away with an error, runs the ring again and checks that a
 * fence of the driver's comes back, and opens the gate once a last
 * fence finds no new fault.  The checks that fail three times in a row,
 * a reset that does not end, or three faults no session caused within a
 * minute make the device lost.
 *
 * The GPU core has three recovery calls of its own: its fault makes the
 * device lost at once, and its reset (asked only once no older owner is
 * left) has the service thread reset the device, check that a fence of
 * the driver's comes back, and give back what the lost device kept and
 * what the isolated sessions held.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 5,
 * what a reset keeps and clears, and the order of the recovery; section
 * 10, the faults and the watchdog).
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "ghci.h"
#include "ghci-cmd.h"

/* How long the stop of a thread waits for it to leave. */
#define THREAD_STOP_MS			2000U

/* The owners of dead sessions one recovery reports at most. */
#define DEAD_REPORTS			GHCI_SESSIONS

/* What one pass of the recovery ended in. */
enum recovery_outcome {
	RECOVERY_NOTHING = 0,
	RECOVERY_DONE,
	RECOVERY_AGAIN,
	RECOVERY_LOST
};

/*
 * What the recovery read of a halted (or faulted) processor: FAULT_INFO,
 * SESSION_ACTIVE, the fence page, and whether the processor stopped.
 */
struct halt_reading {
	uint32_t fault_info;
	uint32_t active;
	uint64_t fence;
	int halted;
};

static enum recovery_outcome recover_once(struct ghci_device *device);
static unsigned take_records(struct ghci_device *device, struct ghci_fault_record *records);
static void halt_and_read(struct ghci_device *device, unsigned *fault_seen, struct halt_reading *reading);
static int count_storm(struct ghci_device *device, unsigned count, int culprits);
static int count_window(uint64_t *ticks, unsigned capacity, unsigned *count, uint64_t now);
static int reset_and_check(struct ghci_device *device);
static uint64_t end_tickets(struct ghci_device *device);
static uint64_t culprit_fences(struct ghci_device *device, uint64_t word, uint64_t *failed, unsigned *failed_count);
static int fence_back(struct ghci_device *device);
static int last_check(struct ghci_device *device, unsigned fault_seen);
static enum recovery_outcome check_failed(struct ghci_device *device, const char *what);
static void lose(struct ghci_device *device, const char *reason);
static void hang_check(struct ghci_device *device);
static void unlock_outer(struct ghci_device *device);
static void placing(struct ghci_device *device, int on, unsigned fault_seen);
static void report_dead(struct ghci_device *device, void **owners, unsigned count);
static void core_reset_step(struct ghci_device *device);
static int core_reset(struct ghci_device *device);

/*
 * Stops the service thread and waits a bounded time for it to leave.
 */
void
ghci_service_stop(
	struct ghci_device *device)
{
	unsigned long irq;
	unsigned waited;
	int running;

	/* Asks the thread to leave. */
	ghci_service_wake(device, GHCI_WORK_STOP);

	/*
	 * Waits until it says it has left: a recovery it is running ends
	 * first, as every wait of a recovery is bounded, so that the stop and
	 * the recovery never both drive the device.
	 */
	for (waited = 0U;; waited += 10U) {
		irq = spin_lock_irqsave(&device->irq_lock);

		running = device->service_running;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The thread has left. */
		if (!running)
			return;

		/* Says once that the thread is slow to leave, and sleeps before the next look. */
		if (waited == THREAD_STOP_MS)
			kern_logf("ghci: %s: waiting for the service thread to leave\n", device->name);
		ghci_sleep_ms(10U);
	}
}

/*
 * Wakes the service thread for work.
 */
void
ghci_service_wake(
	struct ghci_device *device,
	unsigned work)
{
	unsigned long irq;

	/* Records the work. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->service_work |= work;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Wakes the thread. */
	waitq_wake_all(&device->service_wake);
}

/*
 * Runs the service thread: sleeps until work comes or the period ends,
 * and does it, until asked to stop.
 */
void
ghci_service_main(
	void *argument)
{
	struct ghci_device *device;
	unsigned long irq;
	uint64_t sequence;
	uint64_t wake;
	unsigned work;

	/* The device the thread serves. */
	device = argument;

	/* Serves work until asked to stop. */
	for (;;) {
		/* Sleeps until work comes or the period ends, and takes the work. */
		irq = spin_lock_irqsave(&device->irq_lock);

		if (device->service_work == 0U) {
			wake = clock_ticks() + kern_ms_to_ticks(GHCI_SERVICE_PERIOD_MS);
			sequence = waitq_sequence(&device->service_wake);
			(void)waitq_sleep(&device->service_wake, &device->irq_lock, sequence, wake, 0U);
		}
		work = device->service_work;
		device->service_work = 0U;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* Leaves when the device stops. */
		if ((work & GHCI_WORK_STOP) != 0U)
			break;

		/* The work of this round. */
		ghci_service_step(device, work);
	}

	/* Tells the stop that the thread has left. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->service_running = 0;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Does one round of the service's work: the recovery when the gate is
 * closed, the hang check, the display, and the retirement of what the
 * GPU is done with.
 */
void
ghci_service_step(
	struct ghci_device *device,
	unsigned work)
{
	enum recovery_outcome outcome;
	unsigned long irq;
	int recovering;
	int locked;
	int freed;

	/* The GPU core's reset, when it asked for one. */
	if ((work & GHCI_WORK_RESET) != 0U)
		core_reset_step(device);

	/* Work that stopped moving closes the gate too. */
	hang_check(device);

	/* The recovery runs while the gate is closed, whatever woke the thread. */
	irq = spin_lock_irqsave(&device->irq_lock);

	recovering = device->recovering && device->state != GHCI_STATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Recovers until a pass ends the recovery or loses the device. */
	outcome = RECOVERY_NOTHING;
	while (recovering) {
		outcome = recover_once(device);
		if (outcome != RECOVERY_AGAIN)
			break;
	}

	/* The core hears of a lost device once every lock is let go. */
	if (outcome == RECOVERY_LOST && device->gpu != NULL)
		drv_gpu_report_error(device->gpu, EIO);

	/* The executor's completions whose work the GPU finished, or a reset ended. */
	if (device->executor_ops != NULL)
		device->executor_ops->service(device);

	/* The display's records, errors and connection, and what a reset made it place again. */
	ghci_display_service(device, work);

	/*
	 * What the GPU is done with goes back.  The device's lock is only
	 * tried: a thread that holds it may be waiting for the ring's room,
	 * which only this thread's hang check ends.
	 */
	locked = mutex_trylock(&device->lock);
	if (!locked)
		return;

	freed = ghci_vm_reclaim(device);

	mutex_unlock(&device->lock);

	/* The core hears that capacity came back. */
	if (freed && device->gpu != NULL)
		drv_gpu_capacity_changed(device->gpu);
}

/*
 * Waits until no recovery runs: it ended, or the device is lost.  The
 * caller holds none of the device's locks.
 */
void
ghci_recover_wait(
	struct ghci_device *device)
{
	unsigned long irq;
	uint64_t sequence;

	/* Sleeps on the gate while a recovery runs. */
	irq = spin_lock_irqsave(&device->irq_lock);

	while (device->recovering && device->state != GHCI_STATE_LOST) {
		sequence = waitq_sequence(&device->gate_wake);
		(void)waitq_sleep(&device->gate_wake, &device->irq_lock, sequence, 0U, 0U);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Takes the GPU core's fault: the device is lost from here, every ticket
 * still pending fails, and whoever waits at the gate hears it.  Calling
 * it again changes nothing.  The caller holds none of the device's locks.
 */
void
ghci_recover_fault(
	struct ghci_device *device)
{
	unsigned long irq;
	int lost;

	/* A recovery that runs ends its pass first; the loss is set under cp_lock as the recovery's is. */
	mutex_lock(&device->cp_lock);

	irq = spin_lock_irqsave(&device->irq_lock);

	lost = device->state == GHCI_STATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	if (!lost)
		lose(device, "the GPU core gave it up");

	mutex_unlock(&device->cp_lock);
}

/*
 * Runs the GPU core's reset on the service thread and waits a bounded
 * time for it.  The core calls it once no owner older than the caller is
 * left.  The caller holds none of the device's locks.
 *
 * Returns 0 once the device was reset, a fence of the driver's came back
 * and the device runs again, EIO when the reset failed (the device stays
 * lost), or ETIMEDOUT when the service thread did not answer.
 */
int
ghci_recover_reset(
	struct ghci_device *device)
{
	unsigned long irq;
	uint64_t round;
	uint64_t sequence;
	uint64_t deadline;
	uint64_t now;
	int answered;
	int answer;

	/* Asks the service thread, which alone drives a recovery. */
	irq = spin_lock_irqsave(&device->irq_lock);

	round = device->reset_rounds;
	device->reset_asked = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	ghci_service_wake(device, GHCI_WORK_RESET);

	/* Waits on the gate until that round is counted, or the time is up. */
	deadline = clock_ticks() + kern_ms_to_ticks(GHCI_CORE_RESET_WAIT_MS);
	irq = spin_lock_irqsave(&device->irq_lock);

	for (;;) {
		sequence = waitq_sequence(&device->gate_wake);
		answered = device->reset_rounds != round;
		if (answered)
			break;
		now = clock_ticks();
		if (now >= deadline)
			break;
		(void)waitq_sleep(&device->gate_wake, &device->irq_lock, sequence, deadline, 0U);
	}
	answer = device->reset_answer;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The service thread did not answer in time. */
	if (!answered)
		return ETIMEDOUT;

	/* The reset failed; the device stays lost. */
	if (answer != 0)
		return answer;

	/* Succeeded: the device runs again. */
	return 0;
}

/*
 * Runs the poll thread of a polled device: every GHCI_POLL_MS it does the
 * interrupt handler's work, until asked to stop.
 */
void
ghci_poll_main(
	void *argument)
{
	struct ghci_device *device;
	unsigned long irq;
	uint64_t sequence;
	uint64_t wake;
	int stop;

	/* The device the thread looks at. */
	device = argument;

	/* Looks at the device until asked to stop. */
	for (;;) {
		/* Sleeps for one period, or until the stop asks. */
		irq = spin_lock_irqsave(&device->irq_lock);

		if (!device->poll_stop) {
			wake = clock_ticks() + kern_ms_to_ticks(GHCI_POLL_MS);
			sequence = waitq_sequence(&device->poll_wake);
			(void)waitq_sleep(&device->poll_wake, &device->irq_lock, sequence, wake, 0U);
		}
		stop = device->poll_stop;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* Leaves when the device stops. */
		if (stop)
			break;

		/* Does what an interrupt would have started. */
		(void)ghci_cp_interrupt(device);
	}

	/* Tells the stop that the thread has left. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->poll_running = 0;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Stops the poll thread and waits a bounded time for it to leave; it
 * comes before the fence page goes back, which the poll reads.
 */
void
ghci_poll_stop(
	struct ghci_device *device)
{
	unsigned long irq;
	unsigned waited;
	int running;

	/* Asks the thread to leave. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->poll_stop = 1;
	running = device->poll_running;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A device that was never polled has no thread to wait for. */
	if (!running)
		return;
	waitq_wake_all(&device->poll_wake);

	/* Waits until it says it has left. */
	for (waited = 0U; waited < THREAD_STOP_MS; waited += 10U) {
		irq = spin_lock_irqsave(&device->irq_lock);

		running = device->poll_running;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The thread has left. */
		if (!running)
			return;

		/* Sleeps before the next look. */
		ghci_sleep_ms(10U);
	}

	/* A thread that does not leave is reported. */
	kern_logf("ghci: %s: poll thread did not stop\n", device->name);
}

/*
 * Runs one pass of the recovery: the records, the halt, the reset, the
 * tickets, the ring running again, and the last check.
 */
static enum recovery_outcome
recover_once(
	struct ghci_device *device)
{
	struct ghci_fault_record records[GHCI_FAULT_RECORDS];
	struct halt_reading reading;
	void *dead[DEAD_REPORTS];
	uint64_t fence;
	unsigned fault_seen;
	unsigned count;
	unsigned index;
	unsigned long irq;
	int culprits;
	int storm;
	int error;

	/* Step 1: the records of this recovery generation; older ones are of faults a reset already took. */
	count = take_records(device, records);

	/* Step 2: the locks of the whole device, in their order. */
	mutex_lock(&device->lock);
	mutex_lock(&device->display_lock);
	mutex_lock(&device->cp_lock);

	/* A device lost meanwhile has nothing to recover. */
	irq = spin_lock_irqsave(&device->irq_lock);

	error = 0;
	if (device->state == GHCI_STATE_LOST)
		error = EIO;
	else
		device->state = GHCI_STATE_RECOVERING;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	if (error != 0) {
		mutex_unlock(&device->cp_lock);
		unlock_outer(device);
		return RECOVERY_LOST;
	}

	/* Halts the processor (unless it faulted) and reads what it latched; a fault the register lost is the interrupt's. */
	halt_and_read(device, &fault_seen, &reading);
	for (index = 0U; index < count && reading.fault_info == 0U; index++)
		reading.fault_info = records[index].fault_info;

	/* The sessions whose work caused the fault, when it was a session's. */
	culprits = 0;
	if (count != 0U)
		culprits = ghci_vm_find_culprits(device, reading.fault_info, reading.active, reading.halted, reading.fence);

	/* Faults no session caused that come too often lose the device. */
	storm = count_storm(device, count, culprits);
	if (storm) {
		lose(device, "faults too often");
		mutex_unlock(&device->cp_lock);
		unlock_outer(device);
		return RECOVERY_LOST;
	}

	/* Step 3: the reset, and the registers it keeps. */
	error = reset_and_check(device);
	if (error != 0) {
		lose(device, "reset did not end");
		mutex_unlock(&device->cp_lock);
		unlock_outer(device);
		return RECOVERY_LOST;
	}

	/*
	 * Step 4: every ticket the reset threw away ends with an error; the
	 * sessions whose submissions were thrown away are marked before the
	 * ring is emptied.
	 */
	fence = end_tickets(device);
	ghci_vm_mark_lost(device, fence);
	ghci_cp_ring_reset(device);

	/* Step 5: the display's flips are counted again while the processor is idle; its lock goes. */
	ghci_display_recover_count(device);
	mutex_unlock(&device->display_lock);

	/* Step 6: the sessions settle, the ring runs again, and a fence of the driver's must come back. */
	ghci_vm_after_reset(device);
	placing(device, 1, fault_seen);
	ghci_cp_run(device);
	error = fence_back(device);
	if (error != 0)
		return check_failed(device, "no fence after the reset");

	/* Step 7: the sessions are opened and mapped again. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->state = GHCI_STATE_REPLAYING;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	error = ghci_vm_replay(device);
	if (error != 0)
		return check_failed(device, "the replay did not finish");

	/* The replayed maps are read, and placed again with the page-table memory they ask for. */
	error = ghci_vm_replay_tables(device);
	if (error != 0)
		return check_failed(device, "the replayed maps did not finish");

	/* Step 8: a last fence must find no new fault; the gate opens in the same section as the count is compared. */
	error = last_check(device, fault_seen);
	if (error != 0)
		return check_failed(device, "a fault after the reset");

	/* Step 9: every lock goes, and whoever waits at the gate, for room or for a ticket wakes. */
	placing(device, 0, fault_seen);
	count = ghci_vm_take_dead(device, dead, DEAD_REPORTS);
	mutex_unlock(&device->cp_lock);
	mutex_unlock(&device->lock);
	waitq_wake_all(&device->gate_wake);
	waitq_wake_all(&device->ring_wake);
	waitq_wake_all(&device->fence_wake);

	/* Step 10: the core hears of each session that died. */
	report_dead(device, dead, count);

	/* Succeeded: the device runs again. */
	kern_logf("ghci: %s: recovered (reset %u)\n", device->name, device->resets);
	return RECOVERY_DONE;
}

/*
 * Takes the fault records of the current recovery generation, dropping
 * older ones.  Returns how many were taken.
 */
static unsigned
take_records(
	struct ghci_device *device,
	struct ghci_fault_record *records)
{
	unsigned long irq;
	unsigned index;
	unsigned count;

	/* Each record in turn; the list is empty afterwards. */
	irq = spin_lock_irqsave(&device->irq_lock);

	count = 0U;
	for (index = 0U; index < device->fault_records; index++) {
		if (device->faults[index].generation != device->recovery_generation)
			continue;
		records[count] = device->faults[index];
		count++;
	}
	device->fault_records = 0U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the records of this recovery. */
	return count;
}

/*
 * Halts a processor that has not faulted, waits a bounded time for it,
 * and reads and logs what the device latched.  Remembers the fault count
 * the last check compares.
 */
static void
halt_and_read(
	struct ghci_device *device,
	unsigned *fault_seen,
	struct halt_reading *reading)
{
	unsigned long irq;
	uint32_t status;
	uint32_t ctrl;
	uint32_t fault_info;
	uint32_t session_active;
	uint64_t fault_address;
	uint64_t fence;
	int error;

	/*
	 * A processor that has not faulted is halted first, so that what it
	 * latched does not change while it is read; one that faults meanwhile
	 * is stopped too, and one that does not stop in time (HALTING lasts
	 * until its packet ends) is read as it is.
	 */
	status = ghci_read(device, GHCI_REG_STATUS);
	reading->halted = 1;
	if ((status & GHCI_STATUS_FAULTED) == 0U) {
		ctrl = ghci_read(device, GHCI_REG_CTRL);
		ghci_write(device, GHCI_REG_CTRL, ctrl | GHCI_CTRL_HALT);
		error = ghci_wait_halted(device);
		if (error != 0) {
			kern_logf("ghci: %s: processor did not halt for the recovery\n", device->name);
			reading->halted = 0;
		}
	}

	/* The fault count the last check compares with. */
	irq = spin_lock_irqsave(&device->irq_lock);

	*fault_seen = device->fault_count;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* What the device latched; the stage and the syndrome are logged raw. */
	status = ghci_read(device, GHCI_REG_STATUS);
	fault_info = ghci_read(device, GHCI_REG_FAULT_INFO);
	session_active = ghci_read(device, GHCI_REG_SESSION_ACTIVE);
	fault_address = ghci_read(device, GHCI_REG_FAULT_ADDR_LO);
	fault_address |= (uint64_t)ghci_read(device, GHCI_REG_FAULT_ADDR_HI) << 32;
	fence = ghci_fence_read(device);
	kern_logf("ghci: %s: recovering (status %08x, fault %08x kind %u unit %u session %u at %llx, active %04x, fence %llu)\n",
	    device->name,
	    (unsigned)status,
	    (unsigned)fault_info,
	    (unsigned)(fault_info & GHCI_FAULT_KIND_MASK),
	    (unsigned)((fault_info >> GHCI_FAULT_UNIT_SHIFT) & GHCI_FAULT_UNIT_MASK),
	    (unsigned)((fault_info >> GHCI_FAULT_SESSION_SHIFT) & GHCI_FAULT_SESSION_MASK),
	    (unsigned long long)fault_address,
	    (unsigned)session_active,
	    (unsigned long long)fence);

	/* Succeeded: what was read. */
	reading->fault_info = fault_info;
	reading->active = session_active;
	reading->fence = fence;
}

/*
 * Counts this recovery's faults: a fault no session caused joins the
 * storm's window; a fault with a culprit is not counted (the culprit is
 * dead and submits no more).  A pass without records (a check that
 * failed) counts nothing.  Returns 1 when the window is full and the
 * device is to be lost.
 */
static int
count_storm(
	struct ghci_device *device,
	unsigned count,
	int culprits)
{
	uint64_t now;
	int full;

	/* A pass that only repeats a failed check is not a new fault. */
	if (count == 0U)
		return 0;

	/* A session's fault is the culprit's, not the device's. */
	if (culprits != 0)
		return 0;

	/* Succeeded: the fault joins the window, which may be full. */
	now = clock_ticks();
	full = count_window(device->storm_ticks, GHCI_FAULT_STORM, &device->storm_count, now);
	return full;
}

/*
 * Adds a fault at now to a window of the last capacity faults.  Returns
 * 1 when the window holds capacity faults within GHCI_FAULT_STORM_MS.
 */
static int
count_window(
	uint64_t *ticks,
	unsigned capacity,
	unsigned *count,
	uint64_t now)
{
	uint64_t span;
	unsigned index;

	/* The window keeps the newest capacity ticks, oldest first. */
	if (*count == capacity) {
		for (index = 1U; index < capacity; index++)
			ticks[index - 1U] = ticks[index];
		*count = capacity - 1U;
	}
	ticks[*count] = now;
	(*count)++;

	/* A window not yet full, or whose oldest fault is old enough, is not a storm. */
	if (*count < capacity)
		return 0;
	span = kern_ms_to_ticks(GHCI_FAULT_STORM_MS);
	if (now - ticks[0] > span)
		return 0;

	/* Succeeded: too many faults within the window. */
	return 1;
}

/*
 * Resets the device, waits for the reset to end, and checks the
 * registers the reset keeps, writing back any that changed.  The ring is
 * empty afterwards.  Returns 0, or EIO for a reset that did not end or
 * an interconnect that is stuck.
 */
static int
reset_and_check(
	struct ghci_device *device)
{
	uint32_t status;
	uint32_t want[4];
	uint32_t offsets[4];
	uint32_t word;
	unsigned index;
	int error;

	/* RESET is self-clearing; a stuck interconnect never comes back. */
	ghci_write(device, GHCI_REG_CTRL, GHCI_CTRL_RESET);
	error = ghci_wait_register(device, GHCI_REG_CTRL, GHCI_CTRL_RESET, 0U, device->reset_wait_ms);
	status = ghci_read(device, GHCI_REG_STATUS);
	if (error != 0 || (status & GHCI_STATUS_AXI_STUCK) != 0U) {
		kern_logf("ghci: %s: reset did not end (status %08x)\n", device->name, (unsigned)status);
		return EIO;
	}
	device->resets++;

	/*
	 * The memory must be ready before any register is set again or the
	 * ring runs; a memory that does not become ready is a reset that
	 * did not end.
	 *
	 * Values referenced: amaterrace-gpu c74accc docs/ghci.md (section
	 * 3.3, CTRL.RESET and STATUS.MEM_READY after a reset; section 10, the
	 * driver's recovery waits for MEM_READY).
	 */
	error = ghci_wait_register(device, GHCI_REG_STATUS, GHCI_STATUS_MEM_READY, GHCI_STATUS_MEM_READY, GHCI_MEMORY_WAIT_MS);
	if (error != 0) {
		kern_logf("ghci: %s: memory not ready after the reset (status %08x)\n", device->name, (unsigned)ghci_read(device, GHCI_REG_STATUS));
		return EIO;
	}

	/* The registers the reset keeps, as the driver wrote them. */
	offsets[0] = GHCI_REG_RING_BASE_LO;
	want[0] = (uint32_t)device->ring.memory->bus;
	offsets[1] = GHCI_REG_RING_BASE_HI;
	want[1] = (uint32_t)(device->ring.memory->bus >> 32);
	offsets[2] = GHCI_REG_RING_SIZE;
	want[2] = device->ring_bytes;
	offsets[3] = GHCI_REG_WDT_CYCLES;
	want[3] = device->wdt_cycles;

	/* Each is read back, and written again (the processor is idle) when it changed. */
	for (index = 0U; index < 4U; index++) {
		word = ghci_read(device, offsets[index]);
		if (word == want[index])
			continue;
		kern_logf("ghci: %s: register %03x read %08x after the reset; written again\n",
		    device->name,
		    (unsigned)offsets[index],
		    (unsigned)word);
		ghci_write(device, offsets[index], want[index]);
	}

	/* IRQ_ENABLE too, from the mask the handler takes. */
	ghci_cp_irq_enable(device);

	/* Succeeded: the device is reset and idle (the ring is emptied once its log was read). */
	return 0;
}

/*
 * Ends the reset generation (step 4): a ticket whose fence the device
 * wrote before the reset passed, unless it is a culprit's (the device
 * wrote the fence of work it skipped), and every later one failed; the
 * fence page then says the last fence placed, so that nobody waits for a
 * fence that will never be written.  The caller holds cp_lock, with the
 * submission log as the reset found it.  Returns the fence the device had
 * written.
 */
static uint64_t
end_tickets(
	struct ghci_device *device)
{
	struct ghci_reset_record *record;
	uint64_t failed[GHCI_CULPRIT_FENCES];
	unsigned failed_count;
	unsigned index;
	unsigned long irq;
	uint64_t word;
	uint64_t end;

	/* The fence the device wrote before the reset, and the culprits' fences in it. */
	word = ghci_fence_read(device);
	end = culprit_fences(device, word, failed, &failed_count);

	/* Ends the generation and starts the next with every fence placed counted as written. */
	irq = spin_lock_irqsave(&device->irq_lock);

	record = &device->history[device->generation % GHCI_RESET_HISTORY];
	record->generation = device->generation;
	record->completed = end;
	record->failed_count = failed_count;
	for (index = 0U; index < failed_count; index++)
		record->failed[index] = failed[index];
	device->generation++;
	device->completed = device->fence_placed;
	device->issued = device->fence_placed;
	ghci_fence_write(device, device->fence_placed);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The waiters of tickets look again. */
	waitq_wake_all(&device->fence_wake);

	/* Succeeded: the fence the reset ended the generation at. */
	return word;
}

/*
 * Finds the culprits' submissions whose fences the device wrote (word):
 * their work was skipped, so their tickets fail.  Their sequences go to
 * failed, at most GHCI_CULPRIT_FENCES of them; more than that end the
 * generation before the first one instead, with none in failed.  The
 * caller holds cp_lock, with the submission log as the device left it.
 * Returns the fence the generation ends at.
 */
static uint64_t
culprit_fences(
	struct ghci_device *device,
	uint64_t word,
	uint64_t *failed,
	unsigned *failed_count)
{
	struct ghci_submit_entry *entry;
	unsigned count;
	unsigned index;
	uint64_t end;
	int passed;

	/* Each submission of the log whose fence the device wrote, of a culprit. */
	end = word;
	count = 0U;
	for (index = 0U; index < device->submit_count; index++) {
		entry = &device->submit_log[(device->submit_first + index) % GHCI_SUBMIT_LOG];
		passed = ghci_fence64_passed(word, entry->seq);
		if (!passed || entry->owner == NULL || entry->owner->state != GHCI_VM_FAULTED)
			continue;
		if (count == GHCI_CULPRIT_FENCES) {
			end = failed[0] - 1U;
			count = 0U;
			break;
		}
		failed[count] = entry->seq;
		count++;
	}

	/* Succeeded: the end and the culprits' fences. */
	*failed_count = count;
	return end;
}

/*
 * Places a fence of the driver's and waits a bounded time for the fence
 * page to reach it, under cp_lock.  Returns 0, or ETIMEDOUT.
 */
static int
fence_back(
	struct ghci_device *device)
{
	struct ghci_ticket ticket;
	int error;

	/* The fence, placed and rung. */
	error = ghci_cp_place_fence(device, NULL, device->ring.wptr, &ticket);
	if (error != 0)
		return ETIMEDOUT;
	ghci_cp_doorbell(device);

	/* The fence page must reach it. */
	error = ghci_cp_await_fence(device, ticket.seq, GHCI_LIVENESS_MS);
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: the processor runs the ring. */
	return 0;
}

/*
 * The last check of a recovery (step 8): a fence comes back, no fault
 * came since the processor halted, FAULT_INFO is clear, and (with a
 * page-table walker) exactly session 0 and the replayed sessions are
 * active.  In the same section of the interrupt's lock as the fault count
 * is compared, the replayed sessions open and the gate opens.
 * Returns 0 with the gate open, or EIO.
 */
static int
last_check(
	struct ghci_device *device,
	unsigned fault_seen)
{
	unsigned long irq;
	uint32_t fault_info;
	uint32_t active;
	uint64_t word;
	int walker;
	int error;

	/* A fence of the driver's after the replay. */
	error = fence_back(device);
	if (error != 0)
		return EIO;

	/* No fault is latched, and only session 0 runs. */
	fault_info = ghci_read(device, GHCI_REG_FAULT_INFO);
	active = ghci_read(device, GHCI_REG_SESSION_ACTIVE);
	walker = ghci_caps_has(&device->caps, GHCI_HAS_HW_WALKER);
	if (fault_info != 0U)
		return EIO;
	if (walker && active != ghci_vm_active_mask(device))
		return EIO;

	/* Opens the gate unless a fault came since the halt. */
	word = ghci_fence_read(device);
	irq = spin_lock_irqsave(&device->irq_lock);

	error = EIO;
	if (device->fault_count == fault_seen) {
		ghci_vm_settle(device);
		device->recovering = 0;
		device->recovery_generation++;
		device->gate_generation++;
		device->state = GHCI_STATE_RUNNING;
		device->completed = word;
		device->progress_tick = clock_ticks();
		device->recovery_failures = 0U;
		error = 0;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A fault since the halt leaves the gate closed for another pass. */
	if (error != 0)
		return error;

	/* Succeeded: the gate is open. */
	return 0;
}

/*
 * Counts a failed check of a recovery: the device is lost after
 * GHCI_RECOVERY_TRIES in a row, else the recovery goes round again.
 * The caller holds the device's lock and cp_lock; both are let go.
 */
static enum recovery_outcome
check_failed(
	struct ghci_device *device,
	const char *what)
{
	enum recovery_outcome outcome;

	/* One more failure in a row; the recovery's own placement ends. */
	placing(device, 0, 0U);
	device->recovery_failures++;
	kern_logf("ghci: %s: recovery check failed: %s (%u)\n", device->name, what, device->recovery_failures);

	/* Too many in a row lose the device. */
	outcome = RECOVERY_AGAIN;
	if (device->recovery_failures >= GHCI_RECOVERY_TRIES) {
		lose(device, "recovery checks failed");
		outcome = RECOVERY_LOST;
	}

	/* The locks go. */
	mutex_unlock(&device->cp_lock);
	mutex_unlock(&device->lock);

	/* Succeeded: the outcome of the pass. */
	return outcome;
}

/*
 * Makes the device lost: every ticket still pending ends with an error,
 * and so does a culprit's whose fence the device wrote, the gate answers
 * LOST, and whoever waits wakes.  The caller holds cp_lock.
 */
static void
lose(
	struct ghci_device *device,
	const char *reason)
{
	struct ghci_reset_record *record;
	uint64_t failed[GHCI_CULPRIT_FENCES];
	unsigned failed_count;
	unsigned index;
	unsigned long irq;
	uint64_t word;
	uint64_t end;

	/*
	 * The fence the device wrote last, and the culprits' fences in it,
	 * which fail as a reset fails them (their work was skipped).
	 */
	word = ghci_fence_read(device);
	end = culprit_fences(device, word, failed, &failed_count);

	/* Ends the generation with every pending ticket failed, and the device's life. */
	irq = spin_lock_irqsave(&device->irq_lock);

	record = &device->history[device->generation % GHCI_RESET_HISTORY];
	record->generation = device->generation;
	record->completed = end;
	record->failed_count = failed_count;
	for (index = 0U; index < failed_count; index++)
		record->failed[index] = failed[index];
	device->generation++;
	device->state = GHCI_STATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Whoever waits at the gate, for room or for a ticket wakes and hears it. */
	kern_logf("ghci: %s: device lost: %s\n", device->name, reason);
	waitq_wake_all(&device->gate_wake);
	waitq_wake_all(&device->ring_wake);
	waitq_wake_all(&device->fence_wake);
}

/*
 * Finds a hang: a fence placed and not written while neither the fence
 * page nor RING_RPTR moved for GHCI_HANG_MS.  It reads only registers
 * and the fence page under the interrupt's lock, so that it needs no
 * lock a thread waiting for the ring's room holds.
 */
static void
hang_check(
	struct ghci_device *device)
{
	struct ghci_fault_record record;
	unsigned long irq;
	uint64_t word;
	uint64_t now;
	uint64_t hang_ticks;
	uint32_t rptr;
	int idle;
	int hung;

	/* The fence page, the reader's index, the time and the hang time. */
	word = ghci_fence_read(device);
	rptr = ghci_read(device, GHCI_REG_RING_RPTR);
	now = clock_ticks();
	hang_ticks = kern_ms_to_ticks(GHCI_HANG_MS);

	/* Compares with what was seen last. */
	irq = spin_lock_irqsave(&device->irq_lock);

	hung = 0;
	idle = ghci_fence64_passed(word, device->issued);
	if (device->recovering || device->state != GHCI_STATE_RUNNING) {
		/* A recovery or a lost device needs no hang check. */
		hung = 0;
	} else if (idle ||
	    word != device->progress_fence ||
	    rptr != device->progress_rptr) {
		/* Nothing is outstanding, or the processor moved: the clock starts again. */
		device->progress_fence = word;
		device->progress_rptr = rptr;
		device->progress_tick = now;
	} else if (now - device->progress_tick >= hang_ticks) {
		/* Neither moved for the hang time: the gate closes for a recovery. */
		hung = 1;
		kern_memset(&record, 0, sizeof(record));
		record.generation = device->recovery_generation;
		record.rptr = rptr;
		record.fence = word;
		record.hang = 1;
		device->recovering = 1;
		if (device->fault_records < GHCI_FAULT_RECORDS) {
			device->faults[device->fault_records] = record;
			device->fault_records++;
		}
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A placement waiting for room ends at once. */
	if (hung) {
		kern_logf("ghci: %s: hang (fence %llu, rptr %u)\n", device->name, (unsigned long long)word, (unsigned)rptr);
		waitq_wake_all(&device->ring_wake);
	}
}

/*
 * Sets or ends the recovery's own placement: while it is set, a wait for
 * the ring's room lasts until a fault newer than fault_seen or
 * GHCI_HANG_MS from now.  The caller holds cp_lock.
 */
static void
placing(
	struct ghci_device *device,
	int on,
	unsigned fault_seen)
{
	unsigned long irq;
	uint64_t deadline;

	/* The deadline, from now. */
	deadline = clock_ticks() + kern_ms_to_ticks(GHCI_HANG_MS);

	/* The wait for room reads them under the interrupt's lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->recovery_placing = on;
	device->recovery_faults = fault_seen;
	device->recovery_deadline = deadline;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/* Tells the core of each session a recovery found dead.  The caller holds no lock. */
static void
report_dead(
	struct ghci_device *device,
	void **owners,
	unsigned count)
{
	unsigned index;

	/* Nothing is reported to a core the device is not registered with. */
	if (device->gpu == NULL)
		return;

	/* Each owner in turn. */
	for (index = 0U; index < count; index++)
		drv_gpu_report_session_error(device->gpu, owners[index], EIO);
}

/* Lets go of the display's lock and the device's lock, in that order. */
static void
unlock_outer(
	struct ghci_device *device)
{
	/* The inner one first. */
	mutex_unlock(&device->display_lock);
	mutex_unlock(&device->lock);
}

/* Runs the GPU core's reset when it is asked for, and publishes its outcome. */
static void
core_reset_step(
	struct ghci_device *device)
{
	unsigned long irq;
	int asked;
	int answer;

	/* Takes the request. */
	irq = spin_lock_irqsave(&device->irq_lock);

	asked = device->reset_asked;
	device->reset_asked = 0;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	if (!asked)
		return;

	/* The reset and its checks. */
	answer = core_reset(device);

	/* The outcome, and the caller that waits on the gate. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->reset_answer = answer;
	device->reset_rounds++;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	waitq_wake_all(&device->gate_wake);
}

/*
 * The GPU core's reset: the device is reset as a recovery resets it,
 * with no session to find a culprit in or to replay (every owner older
 * than the caller has closed), and the device runs again once a fence of
 * the driver's comes back.  The memory a lost device kept and the
 * sessions an isolation held go back then.  Returns 0, or EIO with the
 * device lost.
 */
static int
core_reset(
	struct ghci_device *device)
{
	unsigned long irq;
	unsigned fault_seen;
	int error;

	/* Every lock of the device, in their order. */
	mutex_lock(&device->lock);
	mutex_lock(&device->display_lock);
	mutex_lock(&device->cp_lock);

	/* The gate stays closed through the reset; the faults counted from here are new. */
	irq = spin_lock_irqsave(&device->irq_lock);

	fault_seen = device->fault_count;
	device->recovering = 1;
	device->state = GHCI_STATE_RECOVERING;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/*
	 * The submissions the log remembers may name sessions the core's
	 * closes retired while the device was lost; the log is emptied
	 * before anything reads it.
	 */
	device->submit_first = 0U;
	device->submit_count = 0U;

	/* The reset, and the registers it keeps. */
	error = reset_and_check(device);
	if (error != 0) {
		lose(device, "the core's reset did not end");
		mutex_unlock(&device->cp_lock);
		unlock_outer(device);
		return EIO;
	}

	/* Every ticket still pending ends, the ring is empty, and the display's flips are counted again. */
	(void)end_tickets(device);
	ghci_cp_ring_reset(device);
	ghci_display_recover_count(device);
	mutex_unlock(&device->display_lock);

	/* The sessions settle (the isolated ones retire), the ring runs, and a fence of the driver's comes back. */
	ghci_vm_after_reset(device);
	placing(device, 1, fault_seen);
	ghci_cp_run(device);
	error = fence_back(device);
	if (error == 0)
		error = ghci_vm_replay(device);
	if (error == 0)
		error = ghci_vm_replay_tables(device);
	if (error == 0)
		error = last_check(device, fault_seen);
	placing(device, 0, fault_seen);
	if (error != 0) {
		lose(device, "no fence after the core's reset");
		mutex_unlock(&device->cp_lock);
		mutex_unlock(&device->lock);
		return EIO;
	}

	/* The GPU reaches only the driver's memory now: what the lost device kept goes back. */
	ghci_memory_release_quarantine(device);
	mutex_unlock(&device->cp_lock);
	mutex_unlock(&device->lock);

	/* Whoever waits at the gate, for room or for a ticket wakes. */
	waitq_wake_all(&device->ring_wake);
	waitq_wake_all(&device->fence_wake);

	/* Succeeded: the device runs again. */
	kern_logf("ghci: %s: reset for the GPU core (reset %u)\n", device->name, device->resets);
	return 0;
}
