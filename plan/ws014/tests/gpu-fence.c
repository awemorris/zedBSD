/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Real GPU dispatch and fd ownership exercise completion capabilities without a renderer implementation. */
#define main gpu_framework_unused_main
#include "gpu-framework.c"
#undef main
#include <kern/process.h>
#include <kern/filedesc.h>
#include <kern/handle.h>
#include <drivers/gpu/gpu-fence.h>
#include <uapi/gpu-fence.h>

/* One backend completion remains owned until explicit delivery or the matching session drain. */
struct fence_pending {
	void *session;
	struct drv_gpu_completion *completion;
};

/* The deterministic backend retains each actual callback obligation independently. */
static struct fence_pending pending[GPU_SUBMIT_MAX];

/* A selected immediate delivery tests completion and consume racing the submitting wrapper. */
static struct test_file *immediate_file;
static struct gpu_command_submit_sync *immediate_request;

/* One rejected post proves rollback leaves neither a pending record nor signal authority. */
static int submit_error;

void gpu_test_set_process(struct process *process);
static int fence_submit(void *opaque, void *session, const void *command, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void fence_drain(void *opaque, void *session);
static int fence_ioctl(struct test_file *file, unsigned long command, void *request);
static void fence_create_request(struct gpu_fence_create *request);
static int fence_state_call(struct test_file *file, unsigned long command, int fd, uint64_t generation, int status, struct gpu_fence_state *result);
static int fence_bind_call(struct test_file *file, int fd, uint64_t generation, uint64_t sequence, uint32_t flags);
static void fence_submit_request(struct gpu_command_submit_sync *request, int fd, uint64_t generation);
static void fence_notification(struct test_file *file, uint64_t sequence, int status);

#ifndef GPU_FENCE_HELPERS_ONLY
/* Verifies publication rollback, independent-open authority and actual final-close error termination. */
int
main(void)
{
	struct drv_gpu_ops operations;
	struct drv_gpu_command_ops commands;
	struct drv_gpu_device *device;
	struct drv_gpu_device *other_device;
	struct test_backend backend;
	struct test_backend other_backend;
	struct test_file source;
	struct test_file receiver;
	struct test_file foreign;
	struct process process;
	struct gpu_fence_create created;
	struct gpu_fence_state state;
	struct gpu_command_submit_sync submit;
	struct kernel_handle *handle;
	unsigned before;
	unsigned index;
	int fd;
	int error;

	/* Each open has its own backend context while the shared typed descriptor lives in a real fd table. */
	memset(&process, 0, sizeof(process));
	process.fd = filedesc_create(&process);
	assert(process.fd != NULL);
	gpu_test_set_process(&process);
	memset(&backend, 0, sizeof(backend));
	memset(&other_backend, 0, sizeof(other_backend));
	memset(&operations, 0, sizeof(operations));
	commands.submit = fence_submit;
	commands.drain = fence_drain;
	operations.version = DRV_GPU_INTERFACE_VERSION;
	operations.size = sizeof(operations);
	operations.capabilities = GPU_CAP_NOTIFICATION | GPU_CAP_FENCE;
	operations.open = backend_open;
	operations.close = backend_close;
	operations.get_info = backend_get_info;
	operations.commands = &commands;
	error = drv_gpu_register(&operations, &backend, &device);
	assert(error == 0);
	error = drv_gpu_register(&operations, &other_backend, &other_device);
	assert(error == 0);
	error = open_file(&source, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&receiver, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&foreign, "gpu1", O_RDWR);
	assert(error == 0);

	/* A failed output copy never installs an undisclosed fd or leaks a standalone payload. */
	before = allocations;
	fence_create_request(&created);
	reject_copyout = 1U;
	error = fence_ioctl(&source, GPU_FENCE_CREATE, &created);
	assert(error == EFAULT && allocations == before);
	assert(process.fd->entries[0].state == FILEDESC_SLOT_FREE);
	fence_create_request(&created);
	error = fence_ioctl(&source, GPU_FENCE_CREATE, &created);
	assert(error == 0 && created.fd == 0 && created.generation == 1U);
	fd = created.fd;

	/* The kernel rejects incompatible device identities before any user-space import can claim support. */
	error = fence_state_call(&foreign, GPU_FENCE_QUERY, fd, 0U, 0, &state);
	assert(error == EXDEV);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, fd, 0U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_PENDING);
	error = fence_state_call(&source, GPU_FENCE_WAIT, fd, 1U, 0, &state);
	assert(error == EAGAIN);

	/* Imported aliases cannot signal the producing open's reserved work. */
	error = fence_bind_call(&source, fd, 1U, 0U, 0U);
	assert(error == 0);
	error = fence_state_call(&receiver, GPU_FENCE_SIGNAL, fd, 1U, 0, &state);
	assert(error == EPERM);
	error = fence_state_call(&receiver, GPU_FENCE_RESET, fd, 1U, 0, &state);
	assert(error == EBUSY);
	error = fence_bind_call(&source, fd, 1U, 0U, GPU_FENCE_BIND_RELEASE);
	assert(error == 0);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, fd, 1U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_PENDING);

	/* Rejected backend work keeps the signal payload pending and available for a retry. */
	submit_error = EAGAIN;
	fence_submit_request(&submit, fd, 1U);
	error = fence_ioctl(&source, GPU_COMMAND_SUBMIT_SYNC, &submit);
	assert(error == EAGAIN);
	error = fence_bind_call(&source, fd, 1U, 0U, 0U);
	assert(error == 0);
	error = fence_bind_call(&source, fd, 1U, 0U, GPU_FENCE_BIND_RELEASE);
	assert(error == 0);
	submit_error = 0;

	/* A successful marker retirement is notification only and must not falsely signal native work. */
	fence_submit_request(&submit, fd, 1U);
	error = fence_ioctl(&source, GPU_COMMAND_SUBMIT_SYNC, &submit);
	assert(error == 0 && submit.command.sequence != 0U);
	fence_notification(&source, submit.command.sequence, 0);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, fd, 1U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_PENDING);
	error = fence_state_call(&source, GPU_FENCE_SIGNAL, fd, 1U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_SIGNALED);
	error = fence_state_call(&receiver, GPU_FENCE_RESET, fd, 1U, 0, &state);
	assert(error == 0 && state.generation == 2U);
	error = fence_state_call(&source, GPU_FENCE_SIGNAL, fd, 1U, 0, &state);
	assert(error != 0);

	/* Immediate failed completion plus another observer's consume cannot erase error propagation. */
	fence_submit_request(&submit, fd, 2U);
	immediate_file = &source;
	immediate_request = &submit;
	error = fence_ioctl(&source, GPU_COMMAND_SUBMIT_SYNC, &submit);
	assert(error == 0);
	immediate_file = NULL;
	immediate_request = NULL;
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, fd, 2U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_ERROR && state.error == ENOMEM);

	/* Closing the source GPU open actually terminates every unfinished producer generation. */
	fence_create_request(&created);
	error = fence_ioctl(&source, GPU_FENCE_CREATE, &created);
	assert(error == 0);
	error = fence_bind_call(&source, created.fd, 1U, 0U, 0U);
	assert(error == 0);
	close_file(&source);
	error = fence_state_call(&receiver, GPU_FENCE_QUERY, created.fd, 1U, 0, &state);
	assert(error == 0 && state.state == GPU_FENCE_ERROR && state.error == ENODEV);

	/* The payload outlives final GPU close and stays independently queryable by a retained handle. */
	handle = handle_fd_get(created.fd, KERNEL_HANDLE_DRIVER);
	assert(handle != NULL);
	close_file(&receiver);
	close_file(&foreign);
	for (index = 0U; index < GPU_SUBMIT_MAX; index++)
		assert(pending[index].completion == NULL);

	/* Independent fence payloads retain no stale device or session callback pointers. */
	error = drv_gpu_unregister(device);
	assert(error == 0);
	error = drv_gpu_unregister(other_device);
	assert(error == 0);
	filedesc_destroy(process.fd);
	gpu_test_set_process(NULL);
	handle_put(handle);
	assert(allocations == 0U && held_spinlocks == 0U);
	puts("GPU fence: fd rollback, device/owner validation, submit rejection, native-result boundary, consume race and producer exit PASS");

	/* Succeeded: actual kernel descriptor, GPU completion and shared payload ownership all retired. */
	return 0;
}

#endif

/* Retains an asynchronous callback or delivers an explicitly selected immediate completion. */
static int
fence_submit(
	void *opaque,
	void *session,
	const void *command,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct gpu_command_wait wait;
	unsigned index;
	int error;

	/* No backend operation is permitted under the common registry lock. */
	(void)opaque;
	assert(held_spinlocks == 0U);
	assert(command == NULL && bytes == 0U);
	assert(flags == GPU_COMMAND_CONTEXT_FENCE && timeline == 1U);
	if (submit_error != 0)
		return submit_error;

	/* A fast callback can race another same-fd observer before the submitter resumes. */
	if (immediate_file != NULL) {
		drv_gpu_complete(completion, ENOMEM);
		memset(&wait, 0, sizeof(wait));
		wait.version = GPU_ABI_VERSION;
		wait.size = sizeof(wait);
		wait.sequence = immediate_request->command.sequence;
		wait.flags = GPU_WAIT_CONSUME;
		error = fence_ioctl(immediate_file, GPU_COMMAND_WAIT, &wait);
		assert(error == 0 && wait.status == ENOMEM);
		return 0;
	}

	/* A deterministic peer retains the actual completion rather than manufacturing state in the GPU core. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		if (pending[index].completion == NULL) {
			pending[index].session = session;
			pending[index].completion = completion;
			return 0;
		}
	}

	/* A backend capacity refusal transfers no callback obligation. */
	return EAGAIN;
}

/* Drains only callbacks belonging to the closing independent backend session. */
static void
fence_drain(
	void *opaque,
	void *session)
{
	unsigned index;

	/* The actual common close invokes this barrier before destroying embedded request storage. */
	(void)opaque;
	assert(held_spinlocks == 0U);
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		if (pending[index].completion != NULL && pending[index].session == session) {
			drv_gpu_complete(pending[index].completion, ENODEV);
			pending[index].completion = NULL;
		}
	}

	/* Succeeded: no peer callback retains the retiring session. */
	return;
}

/* Calls the real cdev operation through a retained GPU open description. */
static int
fence_ioctl(
	struct test_file *file,
	unsigned long command,
	void *request)
{
	int error;

	/* No fixture-specific dispatcher substitutes for ordinary device validation. */
	error = file->file.f_ops->ioctl(&file->file, command, (uintptr_t)request);

	/* Succeeded or failed: return the actual kernel result. */
	return error;
}

/* Initializes the exact fence creation ABI with all output fields invalid. */
static void
fence_create_request(
	struct gpu_fence_create *request)
{
	/* A new creation attempt owns no descriptor or generation until copyout succeeds. */
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->fd = -1;
	request->flags = GPU_HANDLE_CLOEXEC;

	/* Succeeded: the ordinary creation validator receives one complete request. */
	return;
}

/* Initializes each state call independently so returned fields never become hidden inputs. */
static int
fence_state_call(
	struct test_file *file,
	unsigned long command,
	int fd,
	uint64_t generation,
	int status,
	struct gpu_fence_state *result)
{
	int error;

	/* Query and mutation inputs remain explicit while output fields begin zeroed. */
	memset(result, 0, sizeof(*result));
	result->version = GPU_ABI_VERSION;
	result->size = sizeof(*result);
	result->fd = fd;
	result->generation = generation;
	result->error = status;
	error = fence_ioctl(file, command, result);

	/* Succeeded or failed: expose the actual state ioctl outcome. */
	return error;
}

/* Names a pending producer generation without exporting its session authority. */
static int
fence_bind_call(
	struct test_file *file,
	int fd,
	uint64_t generation,
	uint64_t sequence,
	uint32_t flags)
{
	struct gpu_fence_bind request;
	int error;

	/* The immutable fd capability and exact generation select the payload, not a raw pointer. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.fd = fd;
	request.generation = generation;
	request.sequence = sequence;
	request.flags = flags;
	error = fence_ioctl(file, GPU_FENCE_BIND, &request);

	/* Succeeded or failed: kernel owner arbitration remains authoritative. */
	return error;
}

/* Builds a context marker with a separately retained signal dependency. */
static void
fence_submit_request(
	struct gpu_command_submit_sync *request,
	int fd,
	uint64_t generation)
{
	/* Empty stream markers test notification ownership independently from command encoding. */
	memset(request, 0, sizeof(*request));
	request->command.version = GPU_ABI_VERSION;
	request->command.size = sizeof(request->command);
	request->command.flags = GPU_COMMAND_CONTEXT_FENCE;
	request->command.timeline = 1U;
	request->wait_fd = -1;
	request->signal_fd = fd;
	request->signal_generation = generation;

	/* Succeeded: only native result verification may later signal this dependency successfully. */
	return;
}

/* Delivers one retained callback then consumes its actual common completion record. */
static void
fence_notification(
	struct test_file *file,
	uint64_t sequence,
	int status)
{
	struct gpu_command_wait request;
	unsigned index;
	int error;

	/* The fixture uses exactly one pending callback in this scenario. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		if (pending[index].completion != NULL) {
			drv_gpu_complete(pending[index].completion, status);
			pending[index].completion = NULL;
			break;
		}
	}

	/* Consuming transport notification remains independent from the shared fence payload state. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.sequence = sequence;
	request.flags = GPU_WAIT_CONSUME;
	error = fence_ioctl(file, GPU_COMMAND_WAIT, &request);
	assert(error == 0 && request.status == (uint32_t)status);

	/* Succeeded: notification observation did not substitute for a native GPU result. */
	return;
}
