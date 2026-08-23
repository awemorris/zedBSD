/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/process.h>
#include <kern/test-checkpoint.h>
#include <kern/thread.h>
#include <kern/vmspace.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <threads.h>

static mtx_t tree_mutex;
static mtx_t checkpoint_mutex;
static cnd_t checkpoint_condition;
static struct process *parent_child;
static struct process *old_parent;
static struct process *new_parent;
static struct process *parent_result;
static unsigned parent_checkpoint_reached;
static unsigned release_parent_checkpoint;
static unsigned reparent_started;
static unsigned reparent_done;
static unsigned parent_result_ready;
static unsigned release_parent_result;
static unsigned old_parent_freed;
static unsigned exit_committed_count;
static unsigned release_exit_committed;
static int exit_owner[2];
static struct thread *exit_threads[2];
static _Thread_local struct thread *current_test_thread;
static struct process *wait_child;
static unsigned wait_reap_reserved;
static unsigned wait_second_sleeping;
static unsigned release_wait_reap;
static int wait_results[2];
static struct thread wait_threads[2];

static struct process *timer_process;
static unsigned timer_locked_reached;
static unsigned timer_retry_reached;
static unsigned release_timer_threads;
static int timer_results[2];

static struct process *stop_process;
static unsigned stop_callback_reached;
static unsigned release_stop_callback;
static int job_notifications[4];
static unsigned job_notification_count;
static unsigned timer_cleanup_calls;
static unsigned tty_detach_calls;

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	assert(mtx_lock(&tree_mutex) == thrd_success);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)lock;
	(void)irq;
	assert(mtx_unlock(&tree_mutex) == thrd_success);
}

void
spin_lock(struct spinlock *lock)
{
	(void)spin_lock_irqsave(lock);
}

void
spin_unlock(struct spinlock *lock)
{
	spin_unlock_irqrestore(lock, 0);
}

struct thread *
thread_current(void)
{
	return current_test_thread;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	(void)deadline;
	(void)flags;
	assert(queue != NULL && condition_lock != NULL);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	wait_second_sleeping = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	spin_unlock(condition_lock);
	while (queue->sequence == observed)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	spin_lock(condition_lock);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	return 0;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	assert(queue != NULL);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	queue->sequence++;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
}

void
sched_wakeup(struct thread *thread)
{
	(void)thread;
}

void
sched_interrupt(struct thread *thread)
{
	(void)thread;
}

void
sched_notify_task(hal_task_t task)
{
	(void)task;
}

void
sched_sleep_locked(uint64_t deadline, struct spinlock *lock)
{
	(void)deadline;
	(void)lock;
	abort();
}

void
sched_sleep_locked_notify(uint64_t deadline, struct spinlock *lock,
	void (*notify)(void *), void *argument)
{
	(void)deadline;
	assert(current_test_thread != NULL);
	current_test_thread->state = THREAD_SLEEPING;
	spin_unlock(lock);
	notify(argument);
	current_test_thread->state = THREAD_RUNNING;
	spin_lock(lock);
}

void
thread_ref(struct thread *thread)
{
	(void)thread;
}

void
thread_release(struct thread *thread)
{
	(void)thread;
}

int
thread_wait(struct thread *thread, int *status)
{
	struct process *process;

	assert(thread != NULL && thread->state == THREAD_ZOMBIE);
	process = thread->proc;
	if (status != NULL)
		*status = thread->exit_status;
	assert(process->threads == thread && process->thread_count == 1);
	process->threads = thread->proc_next;
	process->thread_count = 0;
	thread->state = THREAD_DEAD;
	return 0;
}

void tty_detach_process(struct process *process)
{ (void)process; tty_detach_calls++; }
void process_timer_cleanup(struct process *process)
{ (void)process; timer_cleanup_calls++; }
void vmspace_put(struct vmspace *vmspace) { (void)vmspace; }
struct vmspace kernel_vmspace;
void vmspace_put_deferred(struct vmspace *vmspace) { (void)vmspace; }
void filedesc_destroy(struct filedesc *files) { (void)files; }
void cred_release(struct ucred *cred) { (void)cred; }
void cwdinfo_release(struct cwdinfo *cwd) { (void)cwd; }
int signal_send_process(struct process *process, int signo)
{ (void)process; (void)signo; return 0; }

int
signal_send_process_info(struct process *process, int signo,
	const struct signal_info *info)
{
	assert(process != NULL && signo == SIGCHLD && info != NULL);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	assert(job_notification_count <
	    sizeof(job_notifications) / sizeof(job_notifications[0]));
	job_notifications[job_notification_count++] = info->code;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	return 0;
}

void
kern_free(void *pointer)
{
	if (pointer == old_parent)
		old_parent_freed++;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "unexpected HAL fatal at %s:%d: %s\n", file, line,
	    message);
	abort();
}

static void
checkpoint(enum kern_test_checkpoint_id id, void *object, void *argument)
{
	(void)argument;
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	if (id == KERN_TEST_PROCESS_PARENT_BEFORE_REF && object == old_parent) {
		parent_checkpoint_reached = 1;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_parent_checkpoint)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	} else if (object == timer_process &&
	    id == KERN_TEST_ITIMER_TICK_LOCKED && !timer_locked_reached) {
		timer_locked_reached = 1;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_timer_threads)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	} else if (object == timer_process &&
	    id == KERN_TEST_ITIMER_TICK_RETRY) {
		timer_retry_reached = 1;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_timer_threads)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	} else if (id == KERN_TEST_THREAD_EXIT_COMMITTED) {
		assert(object == exit_threads[0] || object == exit_threads[1]);
		exit_committed_count++;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_exit_committed)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	} else if (id == KERN_TEST_PROCESS_WAIT_REAP_RESERVED) {
		assert(object == wait_child);
		wait_reap_reserved = 1;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_wait_reap)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	} else if (id == KERN_TEST_PROCESS_STOP_CALLBACK_BEFORE_NOTIFY) {
		assert(object == stop_process);
		stop_callback_reached = 1;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_stop_callback)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_mutex) == thrd_success);
	}
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
}

static int
parent_reader(void *argument)
{
	(void)argument;
	parent_result = process_parent_ref(parent_child);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	parent_result_ready = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	while (!release_parent_result)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	process_release(parent_result);
	return 0;
}

static int
parent_reparenter(void *argument)
{
	(void)argument;
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	reparent_started = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	process_test_reparent(parent_child, new_parent);
	old_parent->state = PROCESS_DEAD;
	process_release(old_parent);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	reparent_done = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	return 0;
}

static void
test_parent_reference_barrier(void)
{
	struct process child, original, replacement;
	thrd_t reader, reparenter;

	memset(&child, 0, sizeof(child));
	memset(&original, 0, sizeof(original));
	memset(&replacement, 0, sizeof(replacement));
	refcount_init(&original.refs, 1);
	refcount_init(&replacement.refs, 1);
	original.pid = 10;
	replacement.pid = 11;
	original.state = replacement.state = PROCESS_RUNNING;
	child.parent = &original;
	parent_child = &child;
	old_parent = &original;
	new_parent = &replacement;
	kern_test_checkpoint_set(checkpoint, NULL);
	assert(thrd_create(&reader, parent_reader, NULL) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!parent_checkpoint_reached)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_create(&reparenter, parent_reparenter, NULL) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!reparent_started)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	/* Reparenting cannot pass the tree lock while the reader is between the
	 * parent snapshot and refcount acquisition. */
	assert(!reparent_done && refcount_load(&original.refs) == 1);
	release_parent_checkpoint = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	while (!parent_result_ready || !reparent_done)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(parent_result == &original);
	assert(old_parent_freed == 0);
	assert(process_parent_pid(&child) == replacement.pid);
	release_parent_result = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_join(reader, NULL) == thrd_success);
	assert(thrd_join(reparenter, NULL) == thrd_success);
	assert(old_parent_freed == 1);
}

static int
timer_tick_thread(void *argument)
{
	unsigned index = (unsigned)(uintptr_t)argument;
	timer_results[index] = process_itimer_tick(timer_process, 1);
	return 0;
}

static void
test_simultaneous_interval_ticks(void)
{
	struct process process;
	thrd_t first, second;

	memset(&process, 0, sizeof(process));
	process.itimer_remaining[1] = 2;
	timer_process = &process;
	assert(thrd_create(&first, timer_tick_thread,
	    (void *)(uintptr_t)0) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!timer_locked_reached)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_create(&second, timer_tick_thread,
	    (void *)(uintptr_t)1) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!timer_retry_reached)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	release_timer_threads = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_join(first, NULL) == thrd_success);
	assert(thrd_join(second, NULL) == thrd_success);
	assert(process.itimer_remaining[1] == 0);
	assert(timer_results[0] + timer_results[1] == 1);
}

static int
thread_exit_committer(void *argument)
{
	unsigned index = (unsigned)(uintptr_t)argument;

	exit_owner[index] = process_test_commit_thread_exit(exit_threads[index],
	    40 + (int)index);
	return 0;
}

static void
test_simultaneous_last_thread_exit(void)
{
	struct process process;
	struct thread threads[2];
	thrd_t workers[2];

	memset(&process, 0, sizeof(process));
	memset(threads, 0, sizeof(threads));
	process.state = PROCESS_RUNNING;
	process.threads = &threads[0];
	process.thread_count = 2;
	threads[0].proc = threads[1].proc = &process;
	threads[0].state = threads[1].state = THREAD_RUNNING;
	threads[0].proc_next = &threads[1];
	exit_threads[0] = &threads[0];
	exit_threads[1] = &threads[1];
	exit_committed_count = 0;
	release_exit_committed = 0;
	exit_owner[0] = exit_owner[1] = 0;

	assert(thrd_create(&workers[0], thread_exit_committer,
	    (void *)(uintptr_t)0) == thrd_success);
	assert(thrd_create(&workers[1], thread_exit_committer,
	    (void *)(uintptr_t)1) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (exit_committed_count != 2)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	/* Both threads are still in the deterministic checkpoint, before either
	 * could enter scheduler retirement.  Ownership must already be unique. */
	assert(threads[0].exit_committed && threads[1].exit_committed);
	assert(process.state == PROCESS_EXITING);
	release_exit_committed = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_join(workers[0], NULL) == thrd_success);
	assert(thrd_join(workers[1], NULL) == thrd_success);
	assert(exit_owner[0] + exit_owner[1] == 1);
	assert(process.exit_status ==
	    ((exit_owner[0] ? 40 : 41) & 0xff) << 8);
}

static int
process_waiter(void *argument)
{
	struct process_wait_event event;
	unsigned index = (unsigned)(uintptr_t)argument;
	pid_t selected;

	current_test_thread = &wait_threads[index];
	selected = process_wait_select(wait_threads[index].proc, -1, 0, &event);
	if (selected > 0) {
		int error = process_wait_commit(&event);
		if (error != 0) {
			process_wait_abort(&event);
			wait_results[index] = -error;
		} else {
			wait_results[index] = selected;
		}
	} else {
		wait_results[index] = selected;
	}
	return 0;
}

static void
test_exit_wait_reservation_survives_until_unlink(void)
{
	struct process parent, child;
	struct thread child_thread;
	thrd_t first, second;

	memset(&parent, 0, sizeof(parent));
	memset(&child, 0, sizeof(child));
	memset(&child_thread, 0, sizeof(child_thread));
	memset(wait_threads, 0, sizeof(wait_threads));
	refcount_init(&parent.refs, 10);
	refcount_init(&child.refs, 10);
	parent.pid = 20;
	parent.state = PROCESS_RUNNING;
	parent.children = &child;
	parent.all_next = &child;
	parent.child_waitq.sequence = 1;
	child.pid = 21;
	child.parent = &parent;
	child.state = PROCESS_ZOMBIE;
	child.exit_status = 7 << 8;
	child.threads = &child_thread;
	child.thread_count = 1;
	child_thread.proc = &child;
	child_thread.state = THREAD_ZOMBIE;
	wait_threads[0].proc = wait_threads[1].proc = &parent;
	wait_child = &child;
	wait_reap_reserved = 0;
	wait_second_sleeping = 0;
	release_wait_reap = 0;
	wait_results[0] = wait_results[1] = 0;
	process_test_set_registry(&parent);

	assert(thrd_create(&first, process_waiter,
	    (void *)(uintptr_t)0) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!wait_reap_reserved)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(child.wait_reserved == PROCESS_WAIT_EXITED);
	assert(thrd_create(&second, process_waiter,
	    (void *)(uintptr_t)1) == thrd_success);
	while (!wait_second_sleeping)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	/* The second waiter has observed the same zombie, but reservation forces it
	 * to sleep until the first commit removes the child. */
	assert(child.parent == &parent && parent.children == &child);
	release_wait_reap = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_join(first, NULL) == thrd_success);
	assert(thrd_join(second, NULL) == thrd_success);
	assert(wait_results[0] == child.pid);
	assert(wait_results[1] == -ECHILD);
	assert(parent.children == NULL && child.state == PROCESS_DEAD);
	current_test_thread = NULL;
}

static void
test_detached_last_thread_zombie_is_reapable(void)
{
	struct process parent, child;
	struct process_wait_event event;
	struct thread waiter;
	pid_t selected;

	memset(&parent, 0, sizeof(parent));
	memset(&child, 0, sizeof(child));
	memset(&waiter, 0, sizeof(waiter));
	refcount_init(&parent.refs, 10);
	refcount_init(&child.refs, 10);
	parent.pid = 30;
	parent.state = PROCESS_RUNNING;
	parent.children = &child;
	parent.all_next = &child;
	parent.child_waitq.sequence = 1;
	child.pid = 31;
	child.parent = &parent;
	child.state = PROCESS_ZOMBIE;
	child.exit_status = 9 << 8;
	child.threads = NULL;
	child.thread_count = 0;
	waiter.proc = &parent;
	current_test_thread = &waiter;
	wait_child = &child;
	release_wait_reap = 1;
	process_test_set_registry(&parent);

	selected = process_wait_select(&parent, child.pid, 0, &event);
	assert(selected == child.pid);
	assert(process_wait_commit(&event) == 0);
	assert(parent.children == NULL && child.state == PROCESS_DEAD);
	current_test_thread = NULL;
}

static void
test_autoreap_wins_exit_wait_but_preserves_job_events(void)
{
	struct process parent, child;
	struct process_wait_event event;
	thrd_t waiter;
	pid_t selected;

	memset(&parent, 0, sizeof(parent));
	memset(&child, 0, sizeof(child));
	memset(wait_threads, 0, sizeof(wait_threads));
	refcount_init(&parent.refs, 10);
	refcount_init(&child.refs, 10);
	parent.pid = 40;
	parent.state = PROCESS_RUNNING;
	parent.children = &child;
	parent.all_next = &child;
	parent.child_waitq.sequence = 1;
	child.pid = 41;
	child.parent = &parent;
	child.flags = PROCESS_AUTOREAP;
	child.state = PROCESS_STOPPED;
	wait_threads[0].proc = wait_threads[1].proc = &parent;
	current_test_thread = &wait_threads[1];
	process_test_set_registry(&parent);

	/* SA_NOCLDWAIT/SIG_IGN affects only terminal status.  A pending job-control
	 * transition remains waitable before the child becomes a zombie. */
	child.wait_stopped = 1;
	child.wait_status = (SIGSTOP << 8) | 0x7f;
	selected = process_wait_select_mask(&parent, child.pid, WNOHANG,
	    PROCESS_WAIT_EVENT_STOPPED, &event);
	assert(selected == child.pid && event.kind == PROCESS_WAIT_STOPPED);
	assert(process_wait_commit(&event) == 0 && !child.wait_stopped);
	child.wait_continued = 1;
	selected = process_wait_select_mask(&parent, child.pid, WNOHANG,
	    PROCESS_WAIT_EVENT_CONTINUED, &event);
	assert(selected == child.pid && event.kind == PROCESS_WAIT_CONTINUED);
	assert(process_wait_commit(&event) == 0 && !child.wait_continued);

	child.state = PROCESS_ZOMBIE;
	child.threads = NULL;
	child.thread_count = 0;
	/* The child still matches the selector, but terminal status belongs solely
	 * to the autoreaper.  WNOHANG therefore reports no event, not a reapable PID. */
	selected = process_wait_select(&parent, child.pid, WNOHANG, &event);
	assert(selected == 0);

	wait_second_sleeping = 0;
	wait_results[0] = 0;
	assert(thrd_create(&waiter, process_waiter,
	    (void *)(uintptr_t)0) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!wait_second_sleeping)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	assert(parent.children == &child &&
	    child.wait_reserved == PROCESS_WAIT_NONE);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);

	/* Production autoreap claim/commit unlinks under the tree lock and wakes the
	 * waiter.  Its rescan now has no matching child and converges to ECHILD. */
	assert(process_test_autoreap_once(&child) == 1);
	assert(thrd_join(waiter, NULL) == thrd_success);
	assert(wait_results[0] == -ECHILD);
	assert(parent.children == NULL && child.state == PROCESS_DEAD);
	current_test_thread = NULL;
}

static int
process_stopper(void *argument)
{
	current_test_thread = argument;
	process_stop_current(SIGTSTP);
	return 0;
}

static void
test_continue_suppresses_stale_stop_notification(void)
{
	struct process parent, child;
	struct thread member;
	thrd_t worker;

	memset(&parent, 0, sizeof(parent));
	memset(&child, 0, sizeof(child));
	memset(&member, 0, sizeof(member));
	refcount_init(&parent.refs, 10);
	refcount_init(&child.refs, 10);
	parent.pid = 50;
	parent.state = PROCESS_RUNNING;
	child.pid = 51;
	child.parent = &parent;
	child.state = PROCESS_RUNNING;
	child.threads = &member;
	child.thread_count = 1;
	member.proc = &child;
	member.state = THREAD_RUNNING;
	stop_process = &child;
	stop_callback_reached = 0;
	release_stop_callback = 0;
	job_notification_count = 0;
	memset(job_notifications, 0, sizeof(job_notifications));

	assert(thrd_create(&worker, process_stopper, &member) == thrd_success);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	while (!stop_callback_reached)
		assert(cnd_wait(&checkpoint_condition,
		    &checkpoint_mutex) == thrd_success);
	/* STOPPED is published, but its callback has not yet validated or notified
	 * the parent.  Let a concurrent SIGCONT complete first. */
	assert(child.state == PROCESS_STOPPED && child.wait_stopped);
	assert(job_notification_count == 0);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);

	assert(process_continue(&child, 1) == 1);
	assert(mtx_lock(&checkpoint_mutex) == thrd_success);
	assert(job_notification_count == 1);
	assert(job_notifications[0] == CLD_CONTINUED);
	release_stop_callback = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_mutex) == thrd_success);
	assert(thrd_join(worker, NULL) == thrd_success);

	assert(job_notification_count == 1);
	assert(child.state == PROCESS_RUNNING && !child.stop_requested);
	assert(!child.wait_stopped && child.wait_continued);
	current_test_thread = NULL;
}

static void
test_last_retired_thread_repeats_idempotent_cleanup(void)
{
	struct process process;
	struct thread member;
	unsigned timer_before = timer_cleanup_calls;
	unsigned tty_before = tty_detach_calls;

	memset(&process, 0, sizeof(process));
	memset(&member, 0, sizeof(member));
	process.state = PROCESS_EXITING;
	process.threads = &member;
	process.thread_count = 1;
	member.proc = &process;
	member.state = THREAD_ZOMBIE;
	process_thread_retired(&member);
	assert(process.state == PROCESS_ZOMBIE);
	assert(timer_cleanup_calls == timer_before + 1);
	assert(tty_detach_calls == tty_before + 1);
}

static void
test_tty_session_detach_generation(void)
{
	struct process leader, child, next_leader;
	struct tty *slot = (struct tty *)(uintptr_t)0x1000;
	struct tty *snapshot;
	uint64_t generation;

	memset(&leader, 0, sizeof(leader));
	memset(&child, 0, sizeof(child));
	memset(&next_leader, 0, sizeof(next_leader));
	leader.state = child.state = next_leader.state = PROCESS_RUNNING;
	leader.session = child.session = 42;
	next_leader.session = 84;
	leader.all_next = &child;
	child.all_next = &next_leader;
	process_test_set_registry(&leader);
	assert(process_controlling_tty_attach(&leader, slot, 1) == 0);
	assert(process_controlling_tty_attach(&child, slot, 1) == 0);
	process_controlling_tty_detach_session(42, slot, 1);
	assert(process_controlling_tty_snapshot(&leader, &snapshot,
	    &generation) == 0 && snapshot == NULL && generation == 0);
	assert(process_controlling_tty_snapshot(&child, &snapshot,
	    &generation) == 0 && snapshot == NULL && generation == 0);

	/* Even before a defensive stale pointer is swept, a reused PTY slot's new
	 * association generation cannot authenticate an old session member. */
	assert(process_controlling_tty_attach(&child, slot, 1) == 0);
	assert(!process_controlling_tty_matches(&child, slot, 2));
	assert(process_controlling_tty_attach(&next_leader, slot, 2) == 0);
	assert(process_controlling_tty_matches(&next_leader, slot, 2));
	process_controlling_tty_detach_session(42, slot, 1);
	assert(process_controlling_tty_matches(&next_leader, slot, 2));
	child.state = PROCESS_EXITING;
	assert(process_controlling_tty_attach(&child, slot, 3) == ESRCH);
}

int
main(void)
{
	assert(mtx_init(&tree_mutex, mtx_recursive) == thrd_success);
	assert(mtx_init(&checkpoint_mutex, mtx_plain) == thrd_success);
	assert(cnd_init(&checkpoint_condition) == thrd_success);
	test_parent_reference_barrier();
	test_simultaneous_interval_ticks();
	test_simultaneous_last_thread_exit();
	test_exit_wait_reservation_survives_until_unlink();
	test_detached_last_thread_zombie_is_reapable();
	test_autoreap_wins_exit_wait_but_preserves_job_events();
	test_continue_suppresses_stale_stop_notification();
	test_last_retired_thread_repeats_idempotent_cleanup();
	test_tty_session_detach_generation();
	kern_test_checkpoint_set(NULL, NULL);
	puts("zedBSD process lifetime/itimer host tests: PASS");
	return 0;
}
