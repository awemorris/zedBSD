/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Production PTY output regression for a concurrent F_SETFL during a wait.
 */

#include <stdio.h>
#include <stdlib.h>

#include "kern/tty.c"

#define CHECK(condition) do { \
	/* Counts each check before evaluating its condition. */ \
	checks++; \
	\
	/* Stops immediately on an unexpected result. */ \
	if (!(condition)) { \
		fprintf( \
			stderr, \
			"PTY flags line %d: %s\n", \
			__LINE__, \
			#condition); \
		abort(); \
	} \
} while (0)

static unsigned checks;
static unsigned sleeps;
static unsigned change_flags;
static unsigned poll_calls;
static struct pty_pair pair;
static struct pty_handle handle;
static struct file output_file;

static void reset_pair(unsigned used, unsigned nonblocking, unsigned change);
static void check_unlocked(void);

/*
 * Reports the absence of a current process in the fixture.
 */
struct thread *
thread_current(
	void)
{
	/* Disables process-specific job control in this fixture. */
	return NULL;
}

/*
 * Acquires a fixture lock and detects recursive acquisition.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* Verifies ownership before acquiring the requested lock. */
	CHECK(lock == &pair.lock || lock == &pair.slave.lock);
	CHECK(lock->held.value == 0);
	lock->held.value = 1;

	/* Returns the simulated enabled interrupt state. */
	return 1;
}

/*
 * Releases a fixture lock and checks its saved interrupt state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Restores the unlocked state after checking the saved state. */
	CHECK(enabled == 1 && lock->held.value == 1);
	lock->held.value = 0;
}

/*
 * Reads the output wait generation under its condition lock.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* Verifies that the caller protects the output wait generation. */
	CHECK(queue == &pair.output_waitq);
	CHECK(pair.lock.held.value == 1);

	/* Returns the current generation for the pending wait. */
	return queue->sequence;
}

/*
 * Publishes an output wakeup under its condition lock.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Advances the generation while its condition lock is held. */
	CHECK(queue == &pair.output_waitq);
	CHECK(pair.lock.held.value == 1);
	queue->sequence++;
}

/*
 * Schedules a flag change and master read during the output wait.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* Checks the full ring and the first byte of the transformed newline. */
	CHECK(queue == &pair.output_waitq && lock == &pair.lock);
	CHECK(lock->held.value == 1 && observed == queue->sequence);
	CHECK(deadline == 0 && flags == WAITQ_INTERRUPTIBLE);
	CHECK(++sleeps == 1);
	CHECK(pair.output_used == PTY_OUTPUT_MAX);
	CHECK(pair.output[PTY_OUTPUT_MAX - 1U] == '\r');

	/*
	 * Deterministically schedules a master read and optional F_SETFL while
	 * the producer sleeps.  A signal then returns it with a short output.
	 * The wait contract reacquires the condition lock before returning.
	 */
	lock->held.value = 0;

	/* Applies the optional concurrent file-status update. */
	if (change_flags)
		output_file.f_flags.value |= O_NONBLOCK;

	/* Drains the ring before reacquiring the condition lock. */
	pair.output_tail = pair.output_head;
	pair.output_used = 0;
	lock->held.value = 1;

	/* Interrupts the producer after it queued a positive byte count. */
	return EINTR;
}

/*
 * Counts output readiness notifications.
 */
void
poll_notify(
	void)
{
	/* Records the notification without running a separate poll worker. */
	poll_calls++;
}

/*
 * Rejects an unexpected controlling-terminal query.
 *
 * No current process is installed, so job-control calls are unreachable.
 */
int
process_controlling_tty_matches(
	struct process *process,
	struct tty *tty,
	uint64_t generation)
{
	(void)process;
	(void)tty;
	(void)generation;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Rejects an unexpected job-control signal decision.
 */
int
signal_job_control_decision(
	const struct thread *thread,
	int signo)
{
	(void)thread;
	(void)signo;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Rejects an unexpected process-group query.
 */
int
process_pgrp_is_orphaned(
	const struct process *process)
{
	(void)process;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Rejects an unexpected process-group signal.
 */
int
process_signal_pgrp(
	pid_t session,
	pid_t pgrp,
	int signo)
{
	(void)session;
	(void)pgrp;
	(void)signo;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Rejects an unexpected filtered process-group signal.
 */
int
process_signal_pgrp_except(
	pid_t session,
	pid_t pgrp,
	int signo,
	struct process *excluded)
{
	(void)session;
	(void)pgrp;
	(void)signo;
	(void)excluded;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Rejects an unexpected process stop.
 */
void
process_stop_current(
	int signo)
{
	(void)signo;

	/* Fails if the fixture enters process-specific job control. */
	abort();
}

/*
 * Checks flag changes at the PTY output wait boundary.
 */
int
main(
	void)
{
	/* F_SETFL during the wait forbids admitting another output attempt. */
	reset_pair(PTY_OUTPUT_MAX - 1U, 0, 1);
	CHECK(pty_slave_write(&output_file, "\n", 1) == -EAGAIN);
	CHECK(sleeps == 1 &&
	    poll_calls == 1 &&
	    pair.output_used == 0);
	CHECK((file_status_flags_get(&output_file) & O_NONBLOCK) != 0);
	check_unlocked();

	/* Unchanged blocking mode retains the established retry behavior. */
	reset_pair(PTY_OUTPUT_MAX - 1U, 0, 0);
	CHECK(pty_slave_write(&output_file, "\n", 1) == 1);
	CHECK(sleeps == 1 &&
	    poll_calls == 2 &&
	    pair.output_used == 2);
	CHECK(pair.output[0] == '\r' && pair.output[1] == '\n');
	check_unlocked();

	/* Nonblocking transformed output needs room for both bytes at once. */
	reset_pair(PTY_OUTPUT_MAX - 1U, 1, 0);
	CHECK(pty_slave_write(&output_file, "\n", 1) == -EAGAIN);
	CHECK(sleeps == 0 && poll_calls == 0);
	CHECK(pair.output_used == PTY_OUTPUT_MAX - 1U);
	CHECK(pair.output[PTY_OUTPUT_MAX - 1U] == 0);
	check_unlocked();

	/* Emits a transformed newline when the ring has sufficient room. */
	reset_pair(0, 1, 0);
	CHECK(pty_slave_write(&output_file, "\n", 1) == 1);
	CHECK(sleeps == 0 &&
	    poll_calls == 1 &&
	    pair.output_used == 2);
	CHECK(pair.output[0] == '\r' && pair.output[1] == '\n');
	check_unlocked();

	/* Rejects a write after the master has closed. */
	reset_pair(0, 0, 0);
	pair.master_open = 0;
	CHECK(pty_slave_write(&output_file, "\n", 1) == -EIO);
	CHECK(sleeps == 0 &&
	    poll_calls == 0 &&
	    pair.output_used == 0);
	check_unlocked();

	/* Reports completion of every regression check. */
	printf("PTY write flag changes: PASS (%u checks)\n", checks);
	return 0;
}

/* Initializes one slave and its output ring for a test case. */
static void
reset_pair(
	unsigned used,
	unsigned nonblocking,
	unsigned change)
{
	/* Initializes the live pair and its partially populated output ring. */
	memset(&pair, 0, sizeof(pair));
	pair.active = pair.master_open = 1;
	pair.generation = 7;
	pair.slave.termios.c_oflag = OPOST | ONLCR;
	pair.output_used = used;
	pair.output_head = used;

	/* Binds the slave handle to the live pair generation. */
	memset(&handle, 0, sizeof(handle));
	handle.generation = 7;
	handle.pair = &pair;

	/* Opens the fixture file with the requested initial status flags. */
	memset(&output_file, 0, sizeof(output_file));
	output_file.f_data = &handle;
	output_file.f_flags.value = nonblocking ? O_NONBLOCK : 0;

	/* Resets observations and selects the wait-time flag transition. */
	sleeps = poll_calls = 0;
	change_flags = change;
}

/* Checks that the write released both fixture locks. */
static void
check_unlocked(
	void)
{
	/* Verifies both lock domains after each complete write attempt. */
	CHECK(pair.lock.held.value == 0);
	CHECK(pair.slave.lock.held.value == 0);
}
