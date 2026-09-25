/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real fence payloads cross the real AF_UNIX ancillary and descriptor ownership paths. */
#define main handle_fixture_main
#include "handle-fd.c"
#undef main
#include <drivers/gpu/gpu-fence.h>

/* Exercises generation, producer authority and readiness after every original fd owner disappears. */
int
main(void)
{
	struct process sender;
	struct process receiver;
	struct kern_peercred credentials;
	struct socket *left;
	struct socket *right;
	struct kernel_handle *handle;
	struct kernel_handle *received;
	struct drv_gpu_fence_state state;
	struct unix_recv_transaction transaction;
	struct filedesc_reservation reservation;
	struct pollfd descriptor_poll;
	unsigned before;
	uint64_t generation;
	int producer_owner;
	int foreign_owner;
	int descriptor;
	int imported;
	int ready;
	int error;
	ssize_t bytes;
	char byte;

	/* Existing generic ownership regressions run unchanged against the extended handle callback table. */
	error = handle_fixture_main();
	assert(error == 0);
	before = live_allocations;
	memset(&sender, 0, sizeof(sender));
	memset(&receiver, 0, sizeof(receiver));
	memset(&credentials, 0, sizeof(credentials));
	sender.fd = filedesc_create(&sender);
	receiver.fd = filedesc_create(&receiver);
	assert(sender.fd != NULL && receiver.fd != NULL);
	error = unix_socket_pair_create(SOCK_STREAM, 0, &credentials, &left, &right);
	assert(error == 0);

	/* Failed payload allocation never publishes a handle or consumes caller ownership. */
	fail_allocation = 1U;
	error = drv_gpu_fence_create(73U, 0U, &handle);
	assert(error == ENOMEM && handle == NULL);
	error = drv_gpu_fence_create(73U, 0U, &handle);
	assert(error == 0);
	error = drv_gpu_fence_device(handle, 74U);
	assert(error == EXDEV);
	error = drv_gpu_fence_query(handle, 0U, &state);
	assert(error == 0 && state.generation == 1U && state.state == DRV_GPU_FENCE_PENDING);

	/* A bound producer owns a strong reference independent from every process descriptor. */
	error = drv_gpu_fence_bind(handle, 1U, &producer_owner);
	assert(error == 0);
	error = drv_gpu_fence_signal(handle, 1U, &foreign_owner, 0);
	assert(error == EPERM);
	error = drv_gpu_fence_reset(handle, 1U, &state);
	assert(error == EBUSY);
	error = drv_gpu_fence_wait(handle, 1U, 0U, 1U, &state);
	assert(error == EAGAIN);
	error = drv_gpu_fence_wait(handle, 1U, 10U, 0U, &state);
	assert(error == ETIMEDOUT);

	/* SCM_RIGHTS retains the exact payload across sender fd close and descriptor-table destruction. */
	caller_thread.proc = &sender;
	descriptor = handle_fd_create(handle, O_CLOEXEC);
	assert(descriptor >= 0);
	send_handle(left, sender.fd, descriptor);
	error = filedesc_close(sender.fd, descriptor);
	assert(error == 0);
	filedesc_destroy(sender.fd);
	caller_thread.proc = &receiver;
	bytes = unix_socket_receive_begin(right, &byte, 1U, MSG_DONTWAIT, NULL, NULL, 1U, &transaction);
	assert(bytes == 1 && transaction.file_count == 1U);
	error = filedesc_reserve_many(receiver.fd, 1U, 0U, &reservation);
	assert(error == 0);
	error = filedesc_commit_objects(&reservation, transaction.objects, &imported);
	assert(error == 0);
	unix_socket_receive_commit(&transaction);
	received = handle_fd_get(imported, KERNEL_HANDLE_DRIVER);
	assert(received == handle);

	/* Pending work is not readable; verified signal makes every alias level readable. */
	memset(&descriptor_poll, 0, sizeof(descriptor_poll));
	descriptor_poll.fd = imported;
	descriptor_poll.events = POLLIN;
	error = kern_poll_wait(&receiver, &descriptor_poll, 1U, 0U, 1, &ready);
	assert(error == 0 && ready == 0);
	error = drv_gpu_fence_signal(handle, 1U, &producer_owner, 0);
	assert(error == 0);
	error = kern_poll_wait(&receiver, &descriptor_poll, 1U, 0U, 1, &ready);
	assert(error == 0 && ready == 1 && descriptor_poll.revents == POLLIN);
	error = drv_gpu_fence_wait(received, 1U, 0U, 1U, &state);
	assert(error == 0 && state.state == DRV_GPU_FENCE_SIGNALED);

	/* Reset mutates the shared reference payload; it never replaces just one alias's wrapper. */
	error = drv_gpu_fence_reset(received, 1U, &state);
	assert(error == 0 && state.generation == 2U);
	generation = state.generation;
	error = drv_gpu_fence_wait(handle, 1U, 0U, 1U, &state);
	assert(error == ESTALE);
	error = kern_poll_wait(&receiver, &descriptor_poll, 1U, 0U, 1, &ready);
	assert(error == 0 && ready == 0);

	/* Rollback of unaccepted work preserves pending state and its generation for every alias. */
	error = drv_gpu_fence_bind(handle, generation, &producer_owner);
	assert(error == 0);
	error = drv_gpu_fence_unbind(handle, generation, &producer_owner);
	assert(error == 0);
	error = drv_gpu_fence_query(received, generation, &state);
	assert(error == 0 && state.state == DRV_GPU_FENCE_PENDING);

	/* A producer's terminal failure remains distinguishable from successful execution. */
	error = drv_gpu_fence_bind(handle, generation, &producer_owner);
	assert(error == 0);
	error = drv_gpu_fence_signal(handle, generation, &producer_owner, ENODEV);
	assert(error == 0);
	handle_put(handle);
	error = kern_poll_wait(&receiver, &descriptor_poll, 1U, 0U, 1, &ready);
	assert(error == 0 && ready == 1 && descriptor_poll.revents == (POLLIN | POLLERR));
	error = drv_gpu_fence_wait(received, generation, 0U, 1U, &state);
	assert(error == 0 && state.state == DRV_GPU_FENCE_ERROR && state.error == ENODEV);

	/* Final descriptor, message and waiter ownership releases the independent payload exactly once. */
	error = filedesc_close(receiver.fd, imported);
	assert(error == 0);
	handle_put(received);
	filedesc_destroy(receiver.fd);
	caller_thread.proc = NULL;
	socket_close_endpoint(left);
	socket_release(left);
	socket_close_endpoint(right);
	socket_release(right);
	assert(live_allocations == before && spin_depth == 0U);
	puts("GPU fence payload: SCM_RIGHTS, exact-generation reset, producer authority, timeout and level poll PASS");

	/* Succeeded: no fixture-side descriptor, ancillary or fence state implementation was substituted. */
	return 0;
}
