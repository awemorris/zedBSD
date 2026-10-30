/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Invokes the actual static sendmsg/recvmsg/fcntl/ioctl handlers, with real Unix
 * transport and descriptor cores. Only user-copy faults and host services are
 * modeled. The included fixture supplies one maintained set of collaborators.
 */

#define main handle_fd_unused_main
#define syscall_restart_deadline_after handle_fd_unused_deadline
#include "handle-fd.c"
#undef syscall_restart_deadline_after
#undef main
#include "../../../src/kern/syscall.c"

/* The next copyout to this exact user address fails once; ordinary copies remain real. */
static uintptr_t copyout_failure_address;

/*
 * Copies a request from the fixture's address space like a validated user mapping.
 */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	/* A missing user mapping produces the ordinary syscall fault. */
	if (source == 0)
		return EFAULT;

	/* Only the addressing boundary is modeled, not the request structure. */
	memcpy(destination, (const void *)source, size);

	/* Succeeded: the actual handler receives the caller's bytes. */
	return 0;
}

/*
 * Copies results while making one real-handler rollback point deterministic.
 */
int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	/* Fail only the selected copy, after descriptor reservations already exist. */
	if (destination == copyout_failure_address || destination == 0) {
		copyout_failure_address = 0;
		return EFAULT;
	}

	/* Successful copies preserve the actual ABI bytes emitted by the handler. */
	memcpy((void *)destination, source, size);

	/* Succeeded: the user-visible result is available to the fixture. */
	return 0;
}

/*
 * Allocates only the socket's real pseudo-file collaborator for descriptor lookup.
 */
int
file_create_pseudo(
	const struct file_ops *ops,
	int flags,
	void *data,
	struct file **result)
{
	struct file *file;

	/* The handle implementation never uses this allocation path. */
	file = kern_calloc(1, sizeof(*file));
	if (file == NULL)
		return ENFILE;

	/* The real socket-file implementation owns its ops and attached socket. */
	refcount_init(&file->f_refs, 1);
	file->f_ops = ops;
	file->f_flags.value = (unsigned)flags;
	file->f_data = data;
	*result = file;

	/* Succeeded: the socket caller owns one file reference. */
	return 0;
}

/*
 * Rejects file ioctls because the test only exercises the handle wrong-class guard.
 */
int
file_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	/* Reaching the file backend with a handle would fail this fixture. */
	(void)file;
	(void)request;
	(void)argument;
	abort();
}

/*
 * Supplies the ordinary overflow-checked relative deadline collaborator.
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	/* The real handler must reject an unrepresentable future instant. */
	if (delta > UINT64_MAX - now)
		return EOVERFLOW;

	/* Succeeded: the deadline is expressed in the same synthetic tick domain. */
	*deadline = now + delta;
	return 0;
}

/*
 * Rejects unexpected file status mutation in the handle-only status tests.
 */
void
file_status_flags_update(
	struct file *file,
	int mask,
	int flags)
{
	/* A token must fail classification before this file-specific operation. */
	(void)file;
	(void)mask;
	(void)flags;
	abort();
}

/*
 * Rejects unrelated process ownership resolution in the selected fd operations.
 */
struct process *
process_find_ref(
	pid_t pid)
{
	/* None of these tests requests F_SETOWN. */
	(void)pid;
	abort();
}

/*
 * Rejects unexpected process-reference release in the selected syscall paths.
 */
void
process_release(
	struct process *process)
{
	/* The fixture does not acquire process ownership through a syscall. */
	(void)process;
	abort();
}

/*
 * Rejects unrelated process-group validation during handle operations.
 */
int
process_pgrp_in_session(
	pid_t session,
	pid_t pgrp)
{
	/* Group ownership is outside the selected fd-classification checks. */
	(void)session;
	(void)pgrp;
	abort();
}

/*
 * Rejects file record-lock operations before a handle can reach the backend.
 */
int
record_lock_fcntl(
	struct process *process,
	struct file *file,
	int command,
	struct flock_record *request)
{
	/* The generic descriptor classifier must reject a handle first. */
	(void)process;
	(void)file;
	(void)command;
	(void)request;
	abort();
}

/*
 * The calling thread's credential: the superuser's, all zero.  A socketpair
 * send passes it on without a path to check it against.
 */
const struct ucred *
cred_current(void)
{
	static struct ucred root;

	/* The fixture's one credential. */
	return &root;
}

/*
 * Rejects named socket resolution because this fixture uses real socketpairs.
 */
int
namei_path_at(
	struct cwdinfo *context,
	const char *path,
	struct path *result)
{
	/* A socketpair send carries no pathname to resolve. */
	(void)context;
	(void)path;
	(void)result;
	abort();
}

/*
 * Rejects filesystem permission queries in the unnamed socket transport.
 */
int
vfs_access(
	const struct inode *inode,
	const struct ucred *credential,
	int requested)
{
	/* These descriptor operations never authorize themselves through an inode. */
	(void)inode;
	(void)credential;
	(void)requested;
	abort();
}

/*
 * Supplies path identity only for linker-retained but unused named-socket branches.
 */
int
path_equal(
	const struct path *left,
	const struct path *right)
{
	/* A socketpair never publishes a bound filesystem path. */
	(void)left;
	(void)right;
	abort();
}

/*
 * Supplies the packet pool's host interrupt exclusion convention.
 */
bool
hal_irq_disable(
	void)
{
	/* Native host atomics provide the actual memory ordering in this fixture. */
	return true;
}

/*
 * Completes the packet pool's synthetic interrupt exclusion region.
 */
void
hal_irq_enable(
	void)
{
	/* No host interrupt mask was changed by the matching collaborator. */
	return;
}

/*
 * Runs actual syscall copyout rollback and fd-classification acceptance cases.
 */
int
main(
	void)
{
	struct process sender;
	struct process receiver;
	struct kern_peercred credentials;
	struct socket *left;
	struct socket *right;
	struct file *left_file;
	struct file *right_file;
	struct test_payload payload;
	struct kernel_handle *handle;
	struct kernel_handle *received;
	struct sendmsg_args send_request;
	struct recvmsg_args receive_request;
	uintptr_t arguments[6];
	char byte;
	int left_descriptor;
	int right_descriptor;
	int descriptor;
	int delivered;
	intptr_t result;

	/* Create real independent socket endpoints and descriptor namespaces. */
	memset(&sender, 0, sizeof(sender));
	memset(&receiver, 0, sizeof(receiver));
	memset(&credentials, 0, sizeof(credentials));
	sender.fd = filedesc_create(&sender);
	receiver.fd = filedesc_create(&receiver);
	socket_core_init();
	poll_init();
	assert(unix_socket_init() == 0);
	assert(unix_socket_pair_create(SOCK_STREAM, 0, &credentials, &left, &right) == 0);
	assert(socket_file_create(left, &left_file) == 0);
	assert(socket_file_create(right, &right_file) == 0);
	assert(filedesc_install(sender.fd, left_file, &left_descriptor) == 0);
	assert(filedesc_install(receiver.fd, right_file, &right_descriptor) == 0);
	caller_thread.proc = &sender;
	handle = new_handle(&payload);
	descriptor = handle_fd_create(handle, O_CLOEXEC);
	handle_put(handle);

	/* fd flags remain available while status operations and ioctls are classified. */
	memset(arguments, 0, sizeof(arguments));
	arguments[0] = (uintptr_t)descriptor;
	arguments[1] = F_GETFD;
	assert(sys_fcntl_call(arguments) == FD_CLOEXEC);
	arguments[1] = F_SETFD;
	arguments[2] = FD_CLOFORK;
	assert(sys_fcntl_call(arguments) == 0);
	arguments[1] = F_GETFD;
	assert(sys_fcntl_call(arguments) == FD_CLOFORK);
	arguments[1] = F_GETFL;
	assert(sys_fcntl_call(arguments) == -EOPNOTSUPP);
	assert(sys_ioctl_call(arguments) == -ENOTTY);

	/* The actual sendmsg handler resolves the handle and hands its reference to AF_UNIX. */
	memset(&send_request, 0, sizeof(send_request));
	send_request.data = (uint64_t)(uintptr_t)"S";
	send_request.data_length = 1;
	send_request.descriptors = (uint64_t)(uintptr_t)&descriptor;
	send_request.descriptor_count = 1;
	send_request.flags = MSG_DONTWAIT;
	memset(arguments, 0, sizeof(arguments));
	arguments[0] = (uintptr_t)left_descriptor;
	arguments[1] = (uintptr_t)&send_request;
	result = sys_sendmsg_call(arguments);
	assert(result == 1);
	assert(filedesc_close(sender.fd, descriptor) == 0);
	assert(payload.releases == 0);

	/* Fail descriptor copyout after reservation, then retry the same queued message. */
	caller_thread.proc = &receiver;
	memset(&receive_request, 0, sizeof(receive_request));
	receive_request.data = (uint64_t)(uintptr_t)&byte;
	receive_request.data_capacity = 1;
	receive_request.descriptors = (uint64_t)(uintptr_t)&delivered;
	receive_request.descriptor_capacity = 1;
	receive_request.flags = MSG_DONTWAIT | MSG_CMSG_CLOEXEC;
	arguments[0] = (uintptr_t)right_descriptor;
	arguments[1] = (uintptr_t)&receive_request;
	copyout_failure_address = (uintptr_t)&delivered;
	result = sys_recvmsg_call(arguments);
	assert(result == -EFAULT);
	assert(payload.releases == 0);
	assert(filedesc_get_ref(receiver.fd, 1) == NULL);
	result = sys_recvmsg_call(arguments);
	assert(result == 1);
	assert(byte == 'S');
	assert(receive_request.descriptor_count == 1);
	assert(delivered == 1);
	received = handle_fd_get(delivered, KERNEL_HANDLE_DRIVER);
	assert(received != NULL);
	assert(received->object == &payload);
	handle_put(received);

	/* The received close-on-exec descriptor keeps the payload until this exact close. */
	filedesc_close_on_exec(receiver.fd);
	assert(payload.releases == 1);
	filedesc_destroy(sender.fd);
	filedesc_destroy(receiver.fd);
	caller_thread.proc = NULL;
	assert(live_allocations == 0);
	assert(spin_depth == 0);
	puts("kernel handle syscall boundary: SCM_RIGHTS, copyout rollback, fd flags, type errors PASS");

	/* Succeeded: the actual syscall handlers preserved all reference ownership. */
	return 0;
}
