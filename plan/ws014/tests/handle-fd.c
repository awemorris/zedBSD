/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises production handle, descriptor, poll and AF_UNIX ownership paths.
 * Host collaborators supply allocation, locks, time and unreachable pathname I/O;
 * no descriptor or ancillary ownership operation is reimplemented by the fixture.
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

/* One payload's callback count remains observable after its wrapper is freed. */
struct test_payload {
	unsigned releases;
};

/* Allocation accounting spans every real core object created by this fixture. */
static unsigned live_allocations;

/* One selected allocator failure verifies publication rollback without hidden production switches. */
static unsigned fail_allocation;

/* Destructors assert this is zero, detecting callbacks under any modeled spinlock. */
static unsigned spin_depth;

/* The current host caller supplies the real handle_fd API's process namespace. */
static struct thread caller_thread;

/* A selected reservation is aborted when dup2 reaches its genuine wait boundary. */
static struct filedesc_reservation *abort_on_wait;

/* File-side counters prove handle operations never use file destruction by accident. */
static unsigned file_releases;

/* The immutable test type only releases its payload through the public contract. */
static const struct kernel_handle_ops payload_ops;

static void payload_release(void *opaque);
static void test_lifecycle(void);
static void test_reservations(void);
static void test_ancillary(void);
static void test_file_peek(void);
static struct kernel_handle *new_handle(struct test_payload *payload);
static void send_handle(struct socket *socket, struct filedesc *table, int descriptor);

/*
 * Runs bounded production ownership checks and verifies all resources are retired.
 */
int
main(
	void)
{
	/* The real socket registry and poll channel own no allocated fixture state. */
	socket_core_init();
	poll_init();
	assert(unix_socket_init() == 0);

	/* Generic descriptor lifetime precedes integration with queued ancillary ownership. */
	test_lifecycle();
	test_reservations();
	test_ancillary();

	/* Rights-bearing peek refusal preserves queued ownership for receive or disconnect. */
	test_file_peek();

	/* Every real core allocation and every modeled lock must have drained. */
	assert(live_allocations == 0);
	assert(spin_depth == 0);
	assert(filedesc_count() == 0);
	assert(socket_count_current() == 0);
	puts("kernel handle/fd/SCM_RIGHTS: lifecycle, reservation, typed transfer, abort and cleanup PASS");

	/* Succeeded: all production ownership assertions passed. */
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

/* Final payload destruction is allowed only after all descriptor/message references drain. */
static void
payload_release(
	void *opaque)
{
	struct test_payload *payload;

	/* Any callback under a table or socket spinlock violates the shared contract. */
	assert(spin_depth == 0);
	payload = opaque;
	assert(payload->releases == 0);
	payload->releases++;
}

/* The immutable callback table lives until every fixture-created handle is gone. */
static const struct kernel_handle_ops payload_ops = {
	.release = payload_release,
};

/* Creates one test payload through the actual independent kernel-handle allocator. */
static struct kernel_handle *
new_handle(
	struct test_payload *payload)
{
	struct kernel_handle *handle;
	int error;

	/* Successful creation transfers only payload destruction responsibility. */
	payload->releases = 0;
	error = handle_create(KERNEL_HANDLE_DRIVER, &payload_ops, payload, &handle);
	assert(error == 0);

	/* Succeeded: the caller owns the initial handle reference. */
	return handle;
}

/* Verifies create/get/close, type rejection, duplication, fork, exec and poll validity. */
static void
test_lifecycle(
	void)
{
	struct process process;
	struct process child;
	struct test_payload payload;
	struct kernel_handle *handle;
	struct kernel_handle *held;
	struct fd_object object;
	struct file file;
	struct file *file_result;
	struct pollfd descriptor_poll;
	unsigned flags;
	unsigned file_before;
	int descriptor;
	int duplicate;
	int ready;
	int error;

	/* A fresh process starts with no descriptor references. */
	memset(&process, 0, sizeof(process));
	memset(&child, 0, sizeof(child));
	process.fd = filedesc_create(&process);
	assert(process.fd != NULL);
	caller_thread.proc = &process;
	file_before = file_releases;

	/* Publication retains an extra reference without consuming the caller's reference. */
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, O_CLOEXEC);
	assert(descriptor == 0);
	assert(refcount_load(&handle->refcnt) == 2);
	handle_put(handle);
	assert(handle_fd_get(descriptor, KERNEL_HANDLE_DRIVER + 1U) == NULL);
	assert(filedesc_get_ref(process.fd, descriptor) == NULL);
	assert(filedesc_take(process.fd, descriptor, &file_result) == EBADF);
	assert(filedesc_get_file(process.fd, descriptor, &file_result) == EOPNOTSUPP);

	/* A valid release-only handle has no readiness and is never POLLNVAL. */
	descriptor_poll.fd = descriptor;
	descriptor_poll.events = POLLIN | POLLOUT;
	descriptor_poll.revents = 0;
	error = kern_poll_wait(&process, &descriptor_poll, 1, 0, 1, &ready);
	assert(error == 0);
	assert(ready == 0);
	assert(descriptor_poll.revents == 0);

	/* Descriptor flags and duplication do not depend on object class. */
	assert(filedesc_get_flags(process.fd, descriptor, &flags) == 0);
	assert(flags == FILEDESC_CLOEXEC);
	assert(filedesc_dup(process.fd, descriptor, 1, FILEDESC_CLOFORK, &duplicate) == 0);
	assert(duplicate == 1);
	assert(filedesc_clone(process.fd, &child, &child.fd) == 0);
	assert(filedesc_get_object_ref(child.fd, duplicate, &object) == EBADF);
	assert(filedesc_get_object_ref(child.fd, descriptor, &object) == 0);
	assert(object.type == FD_OBJECT_HANDLE);
	assert(fd_object_put(&object) == 0);

	/* A lookup reference survives both descriptor close and descriptor-number reuse. */
	held = handle_fd_get(descriptor, KERNEL_HANDLE_DRIVER);
	assert(held != NULL);
	assert(filedesc_close(process.fd, descriptor) == 0);
	memset(&file, 0, sizeof(file));
	refcount_init(&file.f_refs, 1);
	assert(filedesc_install_at(process.fd, &file, descriptor) == 0);
	assert(handle_fd_get(descriptor, KERNEL_HANDLE_DRIVER) == NULL);
	assert(payload.releases == 0);

	/* Replacing a file with a handle releases only the displaced file reference. */
	assert(filedesc_dup2(process.fd, duplicate, descriptor, 0, 0) == 0);
	assert(file_releases == file_before + 1U);
	assert(filedesc_set_flags(process.fd, descriptor, FILEDESC_CLOEXEC) == 0);
	filedesc_close_on_exec(process.fd);
	filedesc_close_on_exec(child.fd);
	assert(payload.releases == 0);
	assert(filedesc_close(process.fd, duplicate) == 0);
	assert(payload.releases == 0);
	handle_put(held);
	assert(payload.releases == 1);

	/* Every table and wrapper must finish without a hidden file-pool allocation. */
	filedesc_destroy(child.fd);
	filedesc_destroy(process.fd);
	caller_thread.proc = NULL;
	assert(file_releases == file_before + 1U);
}

/* Verifies failed publication, atomic reservation rollback and dup2 reservation waits. */
static void
test_reservations(
	void)
{
	struct process process;
	struct test_payload payload;
	struct kernel_handle *handle;
	struct fd_object object;
	struct filedesc_reservation reservation;
	int descriptor;
	int committed;
	int error;

	/* A one-slot table makes publication exhaustion deterministic. */
	memset(&process, 0, sizeof(process));
	process.fd = filedesc_create(&process);
	assert(process.fd != NULL);
	caller_thread.proc = &process;
	assert(filedesc_set_limit(process.fd, 1) == 0);
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, 0);
	assert(descriptor == 0);
	assert(handle_fd_create(handle, 0) == -EMFILE);
	assert(handle_fd_create(handle, O_RDWR) == -EINVAL);
	assert(filedesc_dup(process.fd, descriptor, 2, 0, &committed) == EMFILE);
	assert(refcount_load(&handle->refcnt) == 2);

	/* Reserved-but-uncommitted slots are not lookup or close targets. */
	assert(filedesc_set_limit(process.fd, 3) == 0);
	assert(filedesc_reserve_many(process.fd, 1, FILEDESC_CLOEXEC, &reservation) == 0);
	assert(reservation.slots[0] == 1);
	assert(filedesc_get_object_ref(process.fd, 1, &object) == EBADF);
	assert(filedesc_close(process.fd, 1) == EBADF);
	fd_object_clear(&object);
	assert(filedesc_commit_objects(&reservation, &object, &committed) == EINVAL);
	assert(reservation.active != 0);
	filedesc_abort_reserved(&reservation);

	/* dup2 waits for the unrelated reservation, then retains its source normally. */
	assert(filedesc_reserve_many(process.fd, 1, 0, &reservation) == 0);
	abort_on_wait = &reservation;
	assert(filedesc_dup2(process.fd, descriptor, reservation.slots[0], 0, 0) == 0);
	assert(abort_on_wait == NULL);
	assert(filedesc_close(process.fd, 1) == 0);

	/* Successful commit consumes and empties its explicit owned carrier. */
	assert(filedesc_reserve_many(process.fd, 1, FILEDESC_CLOEXEC, &reservation) == 0);
	object.type = FD_OBJECT_HANDLE;
	object.data.handle = handle;
	handle_get(handle);
	error = filedesc_commit_objects(&reservation, &object, &committed);
	assert(error == 0);
	assert(object.type == FD_OBJECT_NONE);
	assert(committed == 1);
	assert(filedesc_close(process.fd, descriptor) == 0);
	filedesc_close_on_exec(process.fd);
	assert(payload.releases == 0);
	handle_put(handle);
	assert(payload.releases == 1);
	filedesc_destroy(process.fd);
	caller_thread.proc = NULL;

	/* Wrapper allocation failure leaves the payload entirely caller-owned. */
	payload.releases = 0;
	fail_allocation = 1;
	assert(handle_create(KERNEL_HANDLE_DRIVER, &payload_ops, &payload, &handle) == ENOMEM);
	assert(handle == NULL);
	assert(payload.releases == 0);
}

/* Sends a retained production descriptor reference with one real stream byte. */
static void
send_handle(
	struct socket *socket,
	struct filedesc *table,
	int descriptor)
{
	struct fd_object object;
	ssize_t sent;

	/* The real sendmsg path performs this same locked descriptor lookup. */
	assert(filedesc_get_object_ref(table, descriptor, &object) == 0);
	sent = unix_socket_send_objects(socket, "H", 1, MSG_DONTWAIT, NULL, 0, &object, 1);
	assert(sent == 1);
	assert(object.type == FD_OBJECT_NONE);
}

/* Verifies actual queued rights survive sender close and every receiver outcome. */
static void
test_ancillary(
	void)
{
	struct process sender;
	struct process receiver;
	struct test_payload payload;
	struct kernel_handle *handle;
	struct socket *left;
	struct socket *right;
	struct kern_peercred credentials;
	struct unix_recv_transaction transaction;
	struct filedesc_reservation reservation;
	struct fd_object object;
	struct kernel_handle *received;
	char byte;
	int descriptor;
	int imported;
	ssize_t size;

	/* Distinct real tables model independent sender and receiver fd namespaces. */
	memset(&sender, 0, sizeof(sender));
	memset(&receiver, 0, sizeof(receiver));
	memset(&credentials, 0, sizeof(credentials));
	sender.fd = filedesc_create(&sender);
	receiver.fd = filedesc_create(&receiver);
	assert(sender.fd != NULL);
	assert(receiver.fd != NULL);
	assert(unix_socket_pair_create(SOCK_STREAM, 0, &credentials, &left, &right) == 0);
	caller_thread.proc = &sender;

	/* The queued message becomes the last owner after sender close and local put. */
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, 0);
	assert(descriptor == 0);
	handle_put(handle);
	send_handle(left, sender.fd, descriptor);
	assert(filedesc_close(sender.fd, descriptor) == 0);
	assert(payload.releases == 0);

	/* A failed user-copy transaction releases temporary refs while retaining the queue. */
	size = unix_socket_receive_begin(right, &byte, 1, MSG_DONTWAIT, NULL, NULL, 1, &transaction);
	assert(size == 1);
	assert(byte == 'H');
	assert(transaction.file_count == 1);
	assert(transaction.objects[0].type == FD_OBJECT_HANDLE);
	assert(filedesc_reserve_many(receiver.fd, 1, FILEDESC_CLOEXEC, &reservation) == 0);
	filedesc_abort_reserved(&reservation);
	unix_socket_receive_abort(&transaction);
	assert(payload.releases == 0);

	/* A full receiver table also preserves queued references and message data. */
	assert(filedesc_set_limit(receiver.fd, 0) == 0);
	size = unix_socket_receive_begin(right, &byte, 1, MSG_DONTWAIT, NULL, NULL, 1, &transaction);
	assert(size == 1);
	assert(filedesc_reserve_many(receiver.fd, 1, 0, &reservation) == EMFILE);
	unix_socket_receive_abort(&transaction);
	assert(payload.releases == 0);
	assert(filedesc_set_limit(receiver.fd, KERN_OPEN_MAX) == 0);

	/* Successful receive transfers an owned reference into a different namespace. */
	size = unix_socket_receive_begin(right, &byte, 1, MSG_DONTWAIT, NULL, NULL, 1, &transaction);
	assert(size == 1);
	assert(filedesc_reserve_many(receiver.fd, 1, FILEDESC_CLOEXEC, &reservation) == 0);
	assert(filedesc_commit_objects(&reservation, transaction.objects, &imported) == 0);
	unix_socket_receive_commit(&transaction);
	caller_thread.proc = &receiver;
	received = handle_fd_get(imported, KERNEL_HANDLE_DRIVER);
	assert(received != NULL);
	assert(received->object == &payload);
	assert(filedesc_close(receiver.fd, imported) == 0);
	assert(payload.releases == 0);
	handle_put(received);
	assert(payload.releases == 1);

	/* Truncated ancillary receive closes undisclosed rights when the data is consumed. */
	caller_thread.proc = &sender;
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, 0);
	handle_put(handle);
	send_handle(left, sender.fd, descriptor);
	assert(filedesc_close(sender.fd, descriptor) == 0);
	size = unix_socket_receive_begin(right, &byte, 1, MSG_DONTWAIT, NULL, NULL, 0, &transaction);
	assert(size == 1);
	assert(transaction.file_count == 0);
	assert(transaction.control_truncated != 0);
	unix_socket_receive_commit(&transaction);
	assert(payload.releases == 1);

	/* Closing the receiver releases references still retained only by unread messages. */
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, 0);
	handle_put(handle);
	send_handle(left, sender.fd, descriptor);
	assert(filedesc_close(sender.fd, descriptor) == 0);
	socket_close_endpoint(right);
	socket_release(right);
	assert(payload.releases == 1);

	/* A refused send consumes its temporary reference and leaves caller ownership intact. */
	handle = new_handle(&payload);
	object.type = FD_OBJECT_HANDLE;
	object.data.handle = handle;
	handle_get(handle);
	size = unix_socket_send_objects(left, "H", 1, MSG_DONTWAIT | MSG_NOSIGNAL, NULL, 0, &object, 1);
	assert(size == -EPIPE);
	assert(object.type == FD_OBJECT_NONE);
	assert(payload.releases == 0);
	handle_put(handle);
	assert(payload.releases == 1);

	/* Socket and descriptor cleanup must leave no retained wrapper or queue allocation. */
	socket_close_endpoint(left);
	socket_release(left);
	filedesc_destroy(sender.fd);
	filedesc_destroy(receiver.fd);
	caller_thread.proc = NULL;
}

/* Verifies rejected rights-bearing peeks preserve normal receive and unread cleanup. */
static void
test_file_peek(
	void)
{
	struct socket *left;
	struct socket *right;
	struct kern_peercred credentials;
	struct file source;
	struct file *files[1];
	unsigned count;
	unsigned truncated;
	unsigned before;
	ssize_t bytes;
	char byte;
	int error;

	/* The real stream owns its queued ancillary record independently from this caller. */
	memset(&credentials, 0, sizeof(credentials));
	error = unix_socket_pair_create(SOCK_STREAM, 0, &credentials, &left, &right);
	assert(error == 0);

	/* A separately retained send reference becomes the only owner after sender close. */
	memset(&source, 0, sizeof(source));
	refcount_init(&source.f_refs, 1);
	before = file_releases;
	file_ref(&source);
	files[0] = &source;
	bytes = unix_socket_send_message(left, "P", 1, MSG_DONTWAIT, NULL, 0, files, 1);
	assert(bytes == 1);
	error = file_close(&source);
	assert(error == 0 && file_releases == before);

	/* Refusing rights-bearing peek returns no reference and leaves queue ownership intact. */
	count = 1;
	truncated = 0;
	files[0] = NULL;
	bytes = unix_socket_receive_message(right, &byte, 1, MSG_PEEK | MSG_DONTWAIT, NULL, NULL, files, &count, &truncated);
	assert(bytes == -(ssize_t)EOPNOTSUPP);
	assert(files[0] == NULL && file_releases == before);

	/* The same original message can subsequently transfer its own ownership normally. */
	count = 1;
	files[0] = NULL;
	bytes = unix_socket_receive_message(right, &byte, 1, MSG_DONTWAIT, NULL, NULL, files, &count, &truncated);
	assert(bytes == 1 && byte == 'P');
	assert(count == 1 && truncated == 0 && files[0] == &source);
	assert(file_releases == before);

	/* Once the original queue is consumed, closing the returned reference destroys exactly once. */
	error = file_close(files[0]);
	assert(error == 0 && file_releases == before + 1U);

	/* Repeat with an unread original so endpoint destruction exercises the other final owner. */
	memset(&source, 0, sizeof(source));
	refcount_init(&source.f_refs, 1);
	file_ref(&source);
	files[0] = &source;
	bytes = unix_socket_send_message(left, "Q", 1, MSG_DONTWAIT, NULL, 0, files, 1);
	assert(bytes == 1);
	error = file_close(&source);
	assert(error == 0 && file_releases == before + 1U);

	/* Refusing the second peek also leaves only the unread message owning the file. */
	count = 1;
	files[0] = NULL;
	bytes = unix_socket_receive_message(right, &byte, 1, MSG_PEEK | MSG_DONTWAIT, NULL, NULL, files, &count, &truncated);
	assert(bytes == -(ssize_t)EOPNOTSUPP);
	assert(files[0] == NULL && file_releases == before + 1U);

	/* Unread ancillary cleanup owns the remaining reference and destroys it once. */
	socket_close_endpoint(right);
	socket_release(right);
	assert(file_releases == before + 2U);

	/* Retiring the sender cannot destroy either previously completed file a second time. */
	socket_close_endpoint(left);
	socket_release(left);
	assert(file_releases == before + 2U);

	/* Succeeded: peek refusal preserves each queued file until its final owner releases it. */
	return;
}
