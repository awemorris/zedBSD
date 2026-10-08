/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-263: an AF_UNIX datagram socket whose read side is shut (the other end
 * of its pair closed, or its own SHUT_RD) is readable to poll, so a receive
 * must not wait: it returns 0 once the queue is empty.  The real socket,
 * poll and AF_UNIX code runs on the host with the collaborators of
 * plan/ws014/tests/handle-fd.c (allocation, locks, waits that time out at
 * once, so a receive that would wait returns -EAGAIN here).
 */

#include <kern/handle.h>
#include <kern/fd-object.h>
#include <kern/filedesc.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/process.h>
#include <kern/thread.h>
#include <kern/poll.h>
#include <kern/net/socket.h>
#include <kern/net/packet-buf.h>
#include <kern/namei.h>
#include <kern/cred.h>
#include <kern/clock.h>
#include <kern/signal.h>
#include <kern/record-lock.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Allocation accounting spans every real core object created by this fixture. */
static unsigned live_allocations;

/* One selected allocator failure (none here; the stubs keep the switch). */
static unsigned fail_allocation;

/* Destructors assert this is zero, detecting callbacks under any modeled spinlock. */
static unsigned spin_depth;

/* The current host caller: a receive may wait (thread_current is not NULL). */
static struct thread caller_thread;

/* The stubs' dup2 handoff (unused here). */
static struct filedesc_reservation *abort_on_wait;

/* File-side counters (no file is passed here). */
static unsigned file_releases;

static void test_pair_close(int type);
static void test_shut_read(void);
static ssize_t receive(struct socket *socket, char *byte, int flags, unsigned *count);
static short readiness(struct socket *socket);

/*
 * Runs the datagram end-of-file checks, and the stream's as the reference.
 */
int
main(
	void)
{
	/* The real socket registry and poll channel. */
	socket_core_init();
	poll_init();
	assert(unix_socket_init() == 0);

	/* The other end of a pair closes: datagram (the bug) and stream (as before). */
	test_pair_close(SOCK_DGRAM);
	test_pair_close(SOCK_STREAM);

	/* A datagram socket's own SHUT_RD. */
	test_shut_read();

	/* Every real core allocation and every modeled lock must have drained. */
	assert(live_allocations == 0);
	assert(spin_depth == 0);
	assert(socket_count_current() == 0);
	puts("BUG-263 dgram-eof: a shut datagram socket's receive ends at 0, poll and receive agree PASS");

	/* Succeeded: every check held. */
	return 0;
}


/*
 * Reports target-libc assertions through the host process.
 */
void
__libc_assert_fail(
	const char *expression,
	const char *file,
	int line)
{
	/* Preserve the failing expression and source location before ending this fixture. */
	printf("ASSERT %s:%d: %s\n", file, line, expression);
	abort();
}

/*
 * Supplies counted kernel allocation with one explicit failure point.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *memory;

	/* A chosen failed allocation must create no hidden ownership. */
	if (fail_allocation != 0) {
		fail_allocation--;
		return NULL;
	}

	/* Host storage remains counted until the real core calls kern_free. */
	memory = malloc(bytes);
	if (memory == NULL)
		return NULL;

	/* Every allocation contributes until its matching final release. */
	live_allocations++;
	return memory;
}

/*
 * Supplies zeroed kernel allocations through the same accounting.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *memory;

	/* Refuse overflow before multiplying the allocation's dimensions. */
	if (bytes != 0 && count > SIZE_MAX / bytes)
		return NULL;

	/* The real allocator failure path remains observable to callers. */
	memory = kern_malloc(count * bytes);
	if (memory == NULL)
		return NULL;

	/* Zero initialization mirrors the kernel allocator contract. */
	memset(memory, 0, count * bytes);
	return memory;
}

/*
 * Releases counted storage after its production owner reaches final destruction.
 */
void
kern_free(
	void *memory)
{
	/* A missing allocation has no ownership to account for. */
	if (memory == NULL)
		return;

	/* Final destructors must not double-free or lose allocation accounting. */
	assert(live_allocations != 0);
	live_allocations--;
	free(memory);
}

/*
 * Supplies the current thread used by production handle_fd calls.
 */
struct thread *
thread_current(
	void)
{
	/* The selected process is changed only between bounded test operations. */
	return &caller_thread;
}

/*
 * Initializes observable host spinlocks for the real core.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* No modeled lock begins owned. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

/*
 * Acquires a host lock and detects recursive locking in the production paths.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* A single-threaded fixture cannot legitimately acquire an owned lock again. */
	assert(lock->held.value == 0);
	lock->held.value = 1;
	spin_depth++;

	/* Succeeded: return the modeled enabled-interrupt state. */
	return 1;
}

/*
 * Releases the host lock before any callback can reenter the kernel core.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Every unlock must match one real acquisition. */
	assert(enabled == 1);
	assert(lock->held.value == 1);
	assert(spin_depth != 0);
	lock->held.value = 0;
	spin_depth--;
}

/*
 * Initializes a host mutex without requiring a kernel scheduler.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* The production mutex layout remains observable to cleanup callbacks. */
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);

	/* Succeeded: the mutex starts unowned. */
	return 0;
}

/*
 * Acquires a nonrecursive host mutex for the production Unix stream path.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* Stream serialization must not acquire its own mutex twice. */
	assert(mutex->locked == 0);
	mutex->locked = 1;
}

/*
 * Supplies the interruptible mutex path without injecting an interruption.
 */
int
mutex_lock_interruptible(
	struct mutex *mutex)
{
	/* The ordinary operation must use the same serialization state. */
	mutex_lock(mutex);

	/* Succeeded: the caller owns the stream mutex. */
	return 0;
}

/*
 * Releases the host mutex after the serialized stream operation.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* Every stream mutex release must correspond to its acquisition. */
	assert(mutex->locked == 1);
	mutex->locked = 0;
}

/*
 * Initializes a wait sequence used by descriptor and socket transactions.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Sequences begin nonchanging until a production operation wakes them. */
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

/*
 * Reads the production wait-channel generation.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* A deterministic fixture does not race this scalar sample. */
	return queue->sequence;
}

/*
 * Records a wakeup without supplying a separate event implementation.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* A changed generation makes an earlier wait observation obsolete. */
	queue->sequence++;
}

/*
 * Models only the explicit reservation handoff and bounded timeout cases.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct filedesc_reservation *reservation;

	/* These fixtures never depend on wall-clock or signal scheduling. */
	(void)deadline;
	(void)flags;

	/* dup2 must release its condition lock while the reserving operation completes. */
	if (abort_on_wait != NULL) {
		reservation = abort_on_wait;
		abort_on_wait = NULL;
		spin_unlock_irqrestore(lock, 1);
		filedesc_abort_reserved(reservation);
		(void)spin_lock_irqsave(lock);
		return EAGAIN;
	}

	/* A wake already observed by the real core must force its retry path. */
	if (queue->sequence != observed)
		return EAGAIN;

	/* No unbounded host wait can hide a fixture deadlock. */
	return ETIMEDOUT;
}

/*
 * Retains the file collaborator used to verify mixed object-type compatibility.
 */
void
file_ref(
	struct file *file)
{
	/* Handle operations must never arrive here with a reinterpreted payload. */
	assert(file != NULL);
	refcount_get(&file->f_refs);
}

/*
 * Releases the minimal file collaborator while preserving normal file-close errors.
 */
int
file_close(
	struct file *file)
{
	int last;

	/* Every file carrier owns a real file description reference. */
	assert(file != NULL);
	last = refcount_put(&file->f_refs);
	if (last) {
		assert(spin_depth == 0);
		file_releases++;

		/* Socket files use their actual backend callback and allocated wrapper. */
		if (file->f_ops != NULL && file->f_ops->close != NULL) {
			assert(file->f_ops->close(file) == 0);
			kern_free(file);
		}
	}

	/* Succeeded: this collaborator reports no backend failure. */
	return 0;
}

/*
 * Verifies that process record-lock cleanup applies only to real file inodes.
 */
void
record_lock_release_process_inode(
	struct process *process,
	struct inode *inode)
{
	/* The fixture's pseudo files have no inode; reaching this callback is a defect. */
	(void)process;
	(void)inode;
	abort();
}

/*
 * Supplies no pending signals to the bounded socket and poll calls.
 */
int
signal_pending_unblocked(
	const struct thread *thread)
{
	/* Signal behavior is covered separately from the ownership transport. */
	(void)thread;
	return 0;
}

/*
 * Supplies a nonblocking mutex acquisition for actual stream sends.
 */
int
mutex_trylock(
	struct mutex *mutex)
{
	/* A busy stream is reported without changing ownership. */
	if (mutex->locked != 0)
		return 0;

	/* Succeeded: this call owns the previously idle mutex. */
	mutex->locked = 1;
	return 1;
}

/*
 * Records one waiter wakeup using the same sequence contract.
 */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	/* These bounded operations have no sleeping host thread to select. */
	waitq_wake_all(queue);
}

/*
 * Supplies a stable synthetic scheduler timestamp for immediate operations.
 */
uint64_t
sched_ticks(
	void)
{
	/* The fixture never asserts elapsed-time behavior. */
	return 1;
}

/*
 * Converts socket wait duration into a bounded synthetic deadline.
 */
int
syscall_restart_deadline_after(
	uint64_t ticks,
	uint64_t *deadline)
{
	/* Zero preserves the kernel convention for a missing timeout. */
	*deadline = 0;
	if (ticks != 0)
		*deadline = ticks + 1U;

	/* Succeeded: the caller has a deterministic deadline. */
	return 0;
}

/*
 * Refuses unexpected signal delivery from explicitly MSG_NOSIGNAL test sends.
 */
int
signal_send_thread(
	struct thread *thread,
	int signo)
{
	/* No tested transport path is allowed to deliver a host-side signal. */
	(void)thread;
	(void)signo;
	abort();
}

/*
 * Initializes the unbound path embedded in each real Unix endpoint.
 */
void
path_init(
	struct path *path)
{
	/* Socketpairs never acquire a filesystem name or inode reference. */
	memset(path, 0, sizeof(*path));
}

/*
 * Verifies that the socketpair tests do not enter named-path ownership.
 */
void
path_release(
	struct path *path)
{
	/* Only the unused empty path representation may reach this collaborator. */
	assert(path->p_inode == NULL);
	memset(path, 0, sizeof(*path));
}


/*
 * Receives one byte (blocking unless flags say otherwise) through the files'
 * receive, with room for one file; returns what the receive returned.
 */
static ssize_t
receive(
	struct socket *socket,
	char *byte,
	int flags,
	unsigned *count)
{
	struct file *files[1];
	unsigned truncated;
	ssize_t bytes;

	/* The capacity is one file; an end of file must report none. */
	*count = 1;
	truncated = 1;
	files[0] = NULL;
	bytes = unix_socket_receive_message(socket, byte, 1, flags, NULL, NULL, files, count, &truncated);
	if (bytes >= 0)
		assert(truncated == 0);

	/* Succeeded: the receive's result. */
	return bytes;
}

/*
 * Asks the socket's own poll what it is ready for.
 */
static short
readiness(
	struct socket *socket)
{
	short revents;
	int error;

	/* Reading and writing, as a caller of poll asks. */
	revents = 0;
	error = socket->ops->poll(socket, POLLIN | POLLOUT, &revents);
	assert(error == 0);

	/* Succeeded: the events. */
	return revents;
}

/*
 * One end of a pair sends a byte and closes: the other end reads the byte,
 * then poll says readable and hung up, and a blocking receive returns 0.
 */
static void
test_pair_close(
	int type)
{
	struct kern_peercred credentials;
	struct socket *left;
	struct socket *right;
	ssize_t bytes;
	unsigned count;
	short revents;
	char byte;
	int error;

	/* A pair of the type. */
	memset(&credentials, 0, sizeof(credentials));
	error = unix_socket_pair_create(type, 0, &credentials, &left, &right);
	assert(error == 0);

	/* One byte queued, then the sender gone (a child that died). */
	bytes = unix_socket_send_message(left, "x", 1, MSG_DONTWAIT, NULL, 0, NULL, 0);
	assert(bytes == 1);
	socket_close_endpoint(left);
	socket_release(left);

	/* The queued byte still arrives. */
	bytes = receive(right, &byte, 0, &count);
	assert(bytes == 1 && byte == 'x' && count == 0);

	/* Then poll says readable and hung up. */
	revents = readiness(right);
	assert((revents & POLLIN) != 0);
	assert((revents & POLLHUP) != 0);

	/* And the blocking receive does not wait: 0, no files (it was -EAGAIN, a wait, before the fix). */
	bytes = receive(right, &byte, 0, &count);
	printf("%s: after the peer's close, receive %zd files %u\n", type == SOCK_DGRAM ? "dgram" : "stream", bytes, count);
	(void)fflush(stdout);
	assert(bytes == 0);
	assert(count == 0);

	/* The end closes. */
	socket_close_endpoint(right);
	socket_release(right);
}

/*
 * A datagram socket shut for reading: poll says readable, the receive returns 0.
 */
static void
test_shut_read(
	void)
{
	struct kern_peercred credentials;
	struct socket *left;
	struct socket *right;
	ssize_t bytes;
	unsigned count;
	short revents;
	char byte;
	int error;

	/* A datagram pair, nothing queued. */
	memset(&credentials, 0, sizeof(credentials));
	error = unix_socket_pair_create(SOCK_DGRAM, 0, &credentials, &left, &right);
	assert(error == 0);

	/* Without a shutdown an empty socket would wait (the stubs time out at once). */
	bytes = receive(right, &byte, 0, &count);
	assert(bytes == -(ssize_t)EAGAIN);

	/* Its own read side shut. */
	error = right->ops->shutdown(right, SHUT_RD);
	assert(error == 0);
	revents = readiness(right);
	assert((revents & POLLIN) != 0);
	bytes = receive(right, &byte, 0, &count);
	printf("dgram: after SHUT_RD, receive %zd files %u\n", bytes, count);
	(void)fflush(stdout);
	assert(bytes == 0 && count == 0);

	/* Both ends close. */
	socket_close_endpoint(left);
	socket_release(left);
	socket_close_endpoint(right);
	socket_release(right);
}
