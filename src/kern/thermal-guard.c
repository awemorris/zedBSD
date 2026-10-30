/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal emergency: what the guard does once a firmware zone stayed
 * at its critical level, which is to turn the power off.
 *
 * The guard tells the readers, then asks init to stop the machine with
 * SIGUSR2 when init has a handler for it.  Init acknowledges, shortens the
 * services' grace and asks for a power-off.  When init does not
 * acknowledge within ten seconds (it may be frozen for a system sleep), or
 * the power is still on two minutes after the signal, or init has no
 * handler, the guard cuts the power itself: a helper thread writes the
 * volumes out while the guard waits up to ten seconds for it, then the
 * platform turns the power off without asking the firmware to prepare,
 * and the machine halts when even that fails.
 *
 * Once begun the emergency is never called off, even when the temperature
 * falls.  While it lasts, the system device turns a halt or a reboot into
 * a power-off and refuses a system sleep.
 *
 * There is one way of stopping the machine.  Init claims it when it asks
 * for a halt, a power-off or a reboot; the guard claims it before it cuts
 * the power.  The guard that finds init stopping the machine waits for it
 * up to the two minutes before it cuts anyway.
 */

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/mount.h>
#include <kern/platform.h>
#include <kern/process.h>
#include <kern/sched.h>
#include <kern/signal.h>
#include <kern/thermal.h>
#include <kern/thermal_private.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long the guard waits for init's acknowledgement, and for the power to go once init has it. */
#define THERMAL_ACK_WAIT_MS		10000U
#define THERMAL_INIT_WAIT_MS		120000U

/* How long the guard waits for the volumes to be written out before it cuts the power. */
#define THERMAL_SYNC_WAIT_MS		10000U

/* The process id of init, which is asked to stop the machine. */
#define THERMAL_INIT_PID		1

/* Who holds the one way of stopping the machine. */
#define THERMAL_STOP_FREE		0U
#define THERMAL_STOP_INIT		1U
#define THERMAL_STOP_GUARD		2U

/*
 * The emergency's state, KERN_THERMAL_EMERGENCY_*.  It only moves
 * forward; NONE to the first other value is the one moment the emergency
 * begins, and every reader that sees another value knows it began.  The
 * guard publishes the sensor and the temperature below before it leaves
 * NONE, with release, and a reader loads the state with acquire.
 */
static volatile unsigned thermal_emergency_state;

/*
 * Whether the guard's lock and queue are prepared.  The guard prepares
 * them once, before the state leaves NONE; nothing else touches them while
 * the state is NONE.
 */
static volatile unsigned thermal_emergency_prepared;

/* The sensor that began the emergency and its temperature then; written once, before the state leaves NONE. */
static uint32_t thermal_emergency_sensor;
static int32_t thermal_emergency_millicelsius;

/*
 * Who holds the one way of stopping the machine, THERMAL_STOP_*.  It
 * leaves FREE once; init takes it with its first halt, power-off or
 * reboot, the guard when it is about to cut the power.  The guard may take
 * it over from init once init had its two minutes.
 */
static volatile unsigned thermal_stop_owner;

/*
 * The guard's lock: the condition lock of the guard's queue, which init's
 * acknowledgement and the end of the volumes' sync wake.  Nothing else is
 * taken under it.
 */
static struct spinlock thermal_emergency_lock;
static struct wait_queue thermal_emergency_queue;

/* Whether the helper thread finished writing the volumes out; under the guard's lock. */
static bool thermal_sync_done;

static void thermal_emergency_prepare(void);
static bool thermal_init_has_handler(struct process *init);
static bool thermal_signal_init(void);
static void thermal_wait_until(uint64_t deadline, unsigned awaited);
static bool thermal_state_advance(unsigned from, unsigned to);
static void thermal_wait_for_init_stop(uint64_t deadline);
static void thermal_sync_volumes(void);
static void thermal_sync_main(void *argument);
static void thermal_cut_power(void);

/*
 * Tells whether the thermal emergency began.
 */
bool
kern_thermal_emergency_active(void)
{
	unsigned state;

	/* The emergency began once its state left NONE. */
	state = atomic_raw_load_acquire(&thermal_emergency_state);
	if (state == KERN_THERMAL_EMERGENCY_NONE)
		return false;

	/* Succeeded: it began. */
	return true;
}

/*
 * Claims the one way of stopping the machine for init.
 */
int
kern_thermal_shutdown_claim(void)
{
	unsigned expected;
	int won;

	/* Takes the way when it is free. */
	expected = THERMAL_STOP_FREE;
	won = atomic_raw_compare_exchange(&thermal_stop_owner, &expected, THERMAL_STOP_INIT);
	if (won)
		return 0;

	/* Refuses init once the guard cuts the power itself. */
	if (expected == THERMAL_STOP_GUARD)
		return EBUSY;

	/* Succeeded: init already holds it. */
	return 0;
}

/*
 * Takes init's acknowledgement that it began stopping the machine.
 */
int
kern_thermal_emergency_ack(
	int pid)
{
	unsigned long irq;
	unsigned state;
	bool advanced;

	/* Only init may answer. */
	if (pid != THERMAL_INIT_PID)
		return EPERM;

	/* Refuses an answer nothing waits for, before the guard's lock is touched. */
	state = atomic_raw_load_acquire(&thermal_emergency_state);
	if (state != KERN_THERMAL_EMERGENCY_SIGNALED)
		return EINVAL;

	/* Moves the emergency to INIT and wakes the guard that waits for the answer. */
	irq = spin_lock_irqsave(&thermal_emergency_lock);

	advanced = thermal_state_advance(KERN_THERMAL_EMERGENCY_SIGNALED, KERN_THERMAL_EMERGENCY_INIT);
	waitq_wake_all(&thermal_emergency_queue);

	spin_unlock_irqrestore(&thermal_emergency_lock, irq);

	/* Refuses an answer that came after the guard gave up waiting. */
	if (!advanced)
		return EINVAL;

	/* Succeeded: the guard now gives init its two minutes. */
	kern_logf("thermal: init acknowledged the emergency\n");
	return 0;
}

/*
 * Begins the thermal emergency and carries it out.
 *
 * The guard is the only caller.  The readers hear of it first, then init
 * is asked; the guard waits as the emergency's state says and cuts the
 * power when init did not.
 */
void
kern_thermal_emergency_run(
	uint32_t sensor,
	int32_t millicelsius)
{
	unsigned long irq;
	uint64_t signal_tick;
	uint64_t ack_deadline;
	uint64_t init_deadline;
	unsigned state;
	bool signaled;
	bool advanced;

	/* Prepares the lock and the queue the answer wakes. */
	thermal_emergency_prepare();

	/* Notes what began it, for GET_STATUS, before anyone can see it began. */
	thermal_emergency_sensor = sensor;
	thermal_emergency_millicelsius = millicelsius;

	/*
	 * Begins it, waiting for init's answer from the first moment, so that
	 * a halt or a reboot init asks for from now on turns into a power-off.
	 */
	advanced = thermal_state_advance(KERN_THERMAL_EMERGENCY_NONE, KERN_THERMAL_EMERGENCY_SIGNALED);
	if (!advanced)
		return;
	kern_logf("thermal: emergency: sensor %u at %d mC reached its critical level; turning the power off\n",
		  sensor, millicelsius);

	/* Tells the readers; the state stays in GET_STATUS when a ring drops the event. */
	kern_thermal_event(KERN_THERMAL_EVENT_EMERGENCY, sensor, millicelsius);

	/* Asks init to stop the machine, when init has a way to hear it; the waits count from here. */
	signal_tick = sched_ticks();
	ack_deadline = signal_tick + kern_ms_to_ticks(THERMAL_ACK_WAIT_MS);
	init_deadline = signal_tick + kern_ms_to_ticks(THERMAL_INIT_WAIT_MS);
	signaled = thermal_signal_init();
	if (!signaled) {
		/* Init cannot be asked: the guard cuts the power, after init stopping on its own. */
		(void)thermal_state_advance(KERN_THERMAL_EMERGENCY_SIGNALED, KERN_THERMAL_EMERGENCY_CUTTING);
		kern_logf("thermal: init has no handler for the emergency; cutting the power\n");
		thermal_wait_for_init_stop(init_deadline);
		thermal_cut_power();
		return;
	}

	/* Waits for init's acknowledgement. */
	thermal_wait_until(ack_deadline, KERN_THERMAL_EMERGENCY_SIGNALED);

	/* Gives up on an init that did not answer, unless the answer just came. */
	irq = spin_lock_irqsave(&thermal_emergency_lock);

	advanced = thermal_state_advance(KERN_THERMAL_EMERGENCY_SIGNALED, KERN_THERMAL_EMERGENCY_CUTTING);

	spin_unlock_irqrestore(&thermal_emergency_lock, irq);

	/* An init that did not answer is frozen or stuck: the guard cuts the power. */
	if (advanced) {
		kern_logf("thermal: init did not acknowledge the emergency; cutting the power\n");
		thermal_wait_for_init_stop(init_deadline);
		thermal_cut_power();
		return;
	}

	/* Init answered: the power should go within two minutes of the signal. */
	thermal_wait_until(init_deadline, KERN_THERMAL_EMERGENCY_INIT);

	/* The power is still on: the guard takes the way of stopping the machine from init and cuts it. */
	state = atomic_raw_load_acquire(&thermal_emergency_state);
	if (state == KERN_THERMAL_EMERGENCY_INIT)
		kern_logf("thermal: the power is still on two minutes into the emergency; cutting it\n");
	(void)thermal_state_advance(KERN_THERMAL_EMERGENCY_INIT, KERN_THERMAL_EMERGENCY_CUTTING);
	thermal_wait_for_init_stop(init_deadline);
	thermal_cut_power();
}

/*
 * Copies the emergency's state into a status.
 */
void
kern_thermal_emergency_get(
	struct kern_thermal_status *status)
{
	unsigned state;

	/* The state, then what began it, which was written before the state left NONE. */
	state = atomic_raw_load_acquire(&thermal_emergency_state);
	status->emergency = state;
	status->emergency_sensor = 0;
	status->emergency_millicelsius = 0;
	if (state != KERN_THERMAL_EMERGENCY_NONE) {
		status->emergency_sensor = thermal_emergency_sensor;
		status->emergency_millicelsius = thermal_emergency_millicelsius;
	}
}

/* Prepares the guard's lock and queue once, before the state leaves NONE. */
static void
thermal_emergency_prepare(void)
{
	unsigned prepared;

	/* Nothing to do the second time. */
	prepared = atomic_raw_load_acquire(&thermal_emergency_prepared);
	if (prepared != 0U)
		return;

	/* Prepares them, then says so. */
	spin_init(&thermal_emergency_lock, LOCK_RANK_DEVICE, "thermal emergency");
	waitq_init(&thermal_emergency_queue, "thermal emergency");
	atomic_raw_store_release(&thermal_emergency_prepared, 1U);
}

/*
 * Tells whether init has a handler for SIGUSR2: one that ignores it or
 * takes the default would not stop the machine (the default ends init,
 * which the kernel does not survive).
 */
static bool
thermal_init_has_handler(
	struct process *init)
{
	struct signal_action action;
	int error;

	/* Reads init's disposition without changing it. */
	error = signal_action_set(init, SIGUSR2, NULL, &action);
	if (error != 0)
		return false;

	/* The default and ignoring are no handler. */
	if (action.handler == (uintptr_t)SIG_DFL)
		return false;
	if (action.handler == (uintptr_t)SIG_IGN)
		return false;

	/* Succeeded: init handles it. */
	return true;
}

/* Sends init SIGUSR2 when it has a handler for it; tells whether it was sent. */
static bool
thermal_signal_init(void)
{
	struct process *init;
	bool handled;
	int error;

	/* Finds init, which may not be running yet early in the boot. */
	init = process_find_ref(THERMAL_INIT_PID);
	if (init == NULL)
		return false;

	/* Sends the signal only to an init that handles it. */
	handled = thermal_init_has_handler(init);
	error = 0;
	if (handled)
		error = signal_send_process(init, SIGUSR2);
	process_release(init);

	/* An init without a handler, or one the signal did not reach, cannot be asked. */
	if (!handled)
		return false;
	if (error != 0) {
		kern_logf("thermal: init could not be signalled (%d)\n", error);
		return false;
	}

	/* Succeeded: init was asked. */
	return true;
}

/*
 * Sleeps until a deadline, or until the emergency's state is no longer
 * the awaited one (init's answer moved it on).  The sync's end also wakes
 * the queue; a caller that waits for the sync gives
 * KERN_THERMAL_EMERGENCY_NONE, which the state never is again, and looks
 * at the sync itself.
 */
static void
thermal_wait_until(
	uint64_t deadline,
	unsigned awaited)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t now;
	unsigned state;

	/* Sleeps until the deadline or the change, again after any early return. */
	irq = spin_lock_irqsave(&thermal_emergency_lock);

	for (;;) {
		now = sched_ticks();
		if (now >= deadline)
			break;
		state = atomic_raw_load_acquire(&thermal_emergency_state);
		if (awaited != KERN_THERMAL_EMERGENCY_NONE && state != awaited)
			break;
		if (awaited == KERN_THERMAL_EMERGENCY_NONE && thermal_sync_done)
			break;
		observed = waitq_sequence(&thermal_emergency_queue);
		(void)waitq_sleep(&thermal_emergency_queue, &thermal_emergency_lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&thermal_emergency_lock, irq);
}

/*
 * Moves the emergency's state from one value to the next when it still
 * has the first; tells whether it moved.
 */
static bool
thermal_state_advance(
	unsigned from,
	unsigned to)
{
	unsigned expected;
	int won;

	/* Moves it only from the expected value. */
	expected = from;
	won = atomic_raw_compare_exchange(&thermal_emergency_state, &expected, to);
	if (!won)
		return false;

	/* Succeeded: the state moved. */
	return true;
}

/*
 * Takes the way of stopping the machine for the guard.  When init is
 * stopping the machine already, waits for it until the deadline, then
 * takes the way over all the same: the power going comes first.
 */
static void
thermal_wait_for_init_stop(
	uint64_t deadline)
{
	unsigned expected;
	uint64_t now;
	int won;

	/* Takes the way when nobody has it. */
	expected = THERMAL_STOP_FREE;
	won = atomic_raw_compare_exchange(&thermal_stop_owner, &expected, THERMAL_STOP_GUARD);
	if (won)
		return;

	/* Init is stopping the machine: gives it until the deadline. */
	now = sched_ticks();
	if (now < deadline) {
		kern_logf("thermal: init is stopping the machine; waiting for it\n");
		thermal_wait_until(deadline, KERN_THERMAL_EMERGENCY_CUTTING);
	}

	/* Takes the way over. */
	atomic_raw_store_release(&thermal_stop_owner, THERMAL_STOP_GUARD);
}

/*
 * Writes the volumes out on a helper thread and waits for it up to the
 * limit; a sync that hangs on the hardware does not keep the power on.
 */
static void
thermal_sync_volumes(void)
{
	struct thread *thread;
	uint64_t deadline;
	unsigned long irq;
	bool done;
	int error;

	/* Starts the helper; without it the power goes unsynced. */
	error = kthread_create(thermal_sync_main, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("thermal: the volumes are not written out: no sync thread (%d)\n", error);
		return;
	}
	thread_start(thread);

	/* Waits for it up to the limit. */
	deadline = sched_ticks() + kern_ms_to_ticks(THERMAL_SYNC_WAIT_MS);
	thermal_wait_until(deadline, KERN_THERMAL_EMERGENCY_NONE);

	/* Says when it did not finish. */
	irq = spin_lock_irqsave(&thermal_emergency_lock);

	done = thermal_sync_done;

	spin_unlock_irqrestore(&thermal_emergency_lock, irq);

	if (!done)
		kern_logf("thermal: the volumes were not written out in time; cutting the power anyway\n");
}

/* The helper thread: writes every volume out and tells the guard. */
static void
thermal_sync_main(
	void *argument)
{
	unsigned long irq;
	int error;

	/* No argument: it writes every mounted volume. */
	(void)argument;

	/* Writes the volumes out. */
	error = mount_sync_all();
	if (error != 0)
		kern_logf("thermal: writing the volumes out failed (%d)\n", error);

	/* Tells the guard it is done. */
	irq = spin_lock_irqsave(&thermal_emergency_lock);

	thermal_sync_done = true;
	waitq_wake_all(&thermal_emergency_queue);

	spin_unlock_irqrestore(&thermal_emergency_lock, irq);
}

/*
 * Cuts the power: writes the volumes out, turns the power off without the
 * firmware's preparation, and halts when the power stays on.
 */
static void
thermal_cut_power(void)
{
	int error;

	/* Writes the volumes out, for as long as the limit allows. */
	thermal_sync_volumes();

	/* Turns the power off the platform's quickest way, which does not return when it works. */
	kern_logf("thermal: emergency power-off\n");
	if (kern_platform_poweroff_emergency != NULL)
		error = kern_platform_poweroff_emergency();
	else
		error = kern_platform_poweroff();

	/* The power stayed on: says so in GET_STATUS and halts. */
	(void)thermal_state_advance(KERN_THERMAL_EMERGENCY_CUTTING, KERN_THERMAL_EMERGENCY_HALTED);
	kern_logf("thermal: the emergency power-off failed (%d); halting\n", error);
	kern_platform_halt();
}
