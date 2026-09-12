/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the production GPU and character-device cores on the host.
 *
 * The harness supplies allocator, credentials, user-copy and lock services.
 * Its synthetic inode owns the same cdev reference as a devfs inode; it does
 * not claim to execute the complete VFS or emulate any graphics hardware.
 */

#include <drivers/gpu.h>
#include <drivers/pci.h>
#include <kern/cdev.h>
#include <kern/cred.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/uaccess.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GPU_COUNT 40U

/* One backend instance whose counters prove callback ownership and cleanup. */
struct test_backend {
	unsigned opens;
	unsigned closes;
	unsigned created;
	unsigned destroyed;
	unsigned live;
	int open_error;
	int create_error;
	uint64_t max_bytes;
	unsigned commands;
	unsigned reads;
	unsigned writes;
	unsigned presents;
	unsigned bad_capset;
	unsigned bad_blob_id;
	uint32_t last_command;
	struct test_session *present_owner;
};

/* One backend session remains allocated until its matching close callback. */
struct test_session {
	struct test_backend *backend;
	unsigned live;
};

/* One resource belongs to its creating session until destroy or final close. */
struct test_resource {
	struct test_session *session;
	uint64_t bytes;
	unsigned kind;
	uint8_t data[4096];
};

/* One open-file description retains its synthetic inode's device generation. */
struct test_file {
	struct inode inode;
	struct file file;
	struct cdev *device;
};

/* One simulated PCI attachment owns its backend until service removal drains. */
struct pci_gpu_fixture {
	struct test_backend backend;
	struct drv_gpu_device *gpu;
	unsigned hardware_live;
	unsigned attaches;
	unsigned publishes;
	unsigned detaches;
};

/* PCI callback state outlives each checked hardware attachment and its retries. */
static struct pci_gpu_fixture pci_fixture;

/* The driver borrows this immutable table only while the fixture is registered. */
static struct drv_pci_driver pci_driver;

/* The PCI-specific adapter belongs to this driver, never to the GPU header. */
static struct drv_pci_service_interface pci_service;

/* The single-threaded caller credential is switched between access tests. */
static struct ucred test_credential;

/* A missing caller tests that internal opens do not imply root permission. */
static unsigned credential_present = 1;

/* Outstanding credential references must return to zero after each open. */
static unsigned credential_references;

/* Each allocated core or backend object contributes until its final release. */
static unsigned allocations;

/* The next requested copy failure tests rollback after a real allocation. */
static unsigned reject_copyout;

/* A countdown selects either descriptor copyin or its subsequent payload copy. */
static unsigned reject_copyin;

/* Zero disables failure; a countdown selects one subsequent allocation. */
static unsigned reject_allocation;

/* Backend callbacks must never execute while the core owns an IRQ spinlock. */
static unsigned held_spinlocks;

/* Retirement wakes descriptor waiters after the node becomes unavailable. */
static unsigned poll_notifications;

/* One callback consumes this target to probe nested session admission once. */
static struct file *reentrant_target;

/* The nested ioctl's result remains visible after the outer callback returns. */
static int reentrant_error;

/* A callback consumes this registration once to exercise removal reentry. */
static struct drv_gpu_device *retire_target;

/* The callback's removal result remains observable after its ioctl finishes. */
static int retirement_error;

/* The callback contract is filled once at test startup and then left unchanged. */
static struct drv_gpu_ops backend_ops;

static int backend_open(void *opaque, void **result);
static void backend_close(void *opaque, void *session_data);
static int backend_get_info(void *opaque, void *session_data, struct gpu_info *info);
static int backend_create(void *opaque, void *session_data, const struct gpu_resource_create *request, void **result);
static void backend_destroy(void *opaque, void *session_data, void *resource_data);
static void expect_error(int actual, int expected);
static int open_file(struct test_file *opened, const char *name, int flags);
static void close_file(struct test_file *opened);
static void prepare_create(struct gpu_resource_create *request);
static int destroy_handle(struct test_file *opened, uint64_t handle);
static void test_validation(void);
static void test_immediate_registration(void);
static void test_access_and_handles(void);
static void test_missing_operations(void);
static void test_capacity_and_reentry(void);
static void test_registration_rollback(void);
static void test_dynamic_devices(void);
static void test_callback_retirement(void);
static void optional_ops_prepare(struct drv_gpu_ops *ops);
static int backend_capset(void *opaque, void *session_data, struct gpu_capset *request);
static int backend_blob(void *opaque, void *session_data, const struct gpu_blob_create *request, void **result, uint32_t *resource_id);
static int backend_resource_read(void *opaque, void *session_data, void *object, uint64_t offset, void *buffer, uint32_t bytes);
static int backend_resource_write(void *opaque, void *session_data, void *object, uint64_t offset, const void *buffer, uint32_t bytes);
static int backend_command(void *opaque, void *session_data, const void *buffer, uint32_t bytes);
static int backend_present(void *opaque, void *session_data, void *object, const struct gpu_present *request);
static void test_optional_validation(void);
static void test_optional_requests(void);
static void test_optional_transfer(struct test_file *opened, struct test_file *foreign, struct test_backend *backend, uint64_t handle);
static void test_optional_command(struct test_file *opened, struct test_backend *backend);
static void test_optional_present(struct test_file *opened, struct test_backend *backend, uint64_t storage, uint64_t blob);

static void test_pci_ownership(void);
static int pci_config_read(void *argument, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *result);
static int pci_config_write(void *argument, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t word);
static int pci_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int pci_detach(struct drv_pci_device *device, unsigned flags);
static int pci_publish_gpu(struct drv_pci_device *device, void *argument);
static int pci_unpublish_gpu(struct drv_pci_device *device, void *argument);

/*
 * Runs the isolated GPU framework acceptance checks.
 */
int
main(
	void)
{
	/* Defines the storage-only backend before any registration borrows it. */
	backend_ops.version = DRV_GPU_INTERFACE_VERSION;
	backend_ops.size = sizeof(backend_ops);
	backend_ops.capabilities = GPU_CAP_RESOURCE;
	backend_ops.open = backend_open;
	backend_ops.close = backend_close;
	backend_ops.get_info = backend_get_info;
	backend_ops.resource_create = backend_create;
	backend_ops.resource_destroy = backend_destroy;

	/* Validates malformed interfaces before any device becomes visible. */
	test_validation();
	test_immediate_registration();

	/* Exercises the post-boot publication and per-open ownership paths. */
	test_access_and_handles();
	test_missing_operations();
	test_capacity_and_reentry();
	test_callback_retirement();
	test_registration_rollback();
	test_dynamic_devices();
	test_optional_validation();
	test_optional_requests();

	/* Records the separately exercised protocol and copied-buffer acceptance paths. */
	puts("GPU optional operations: capability, blob, transfer, command and presentation PASS");

	/* Confirms every retained generation and caller credential was released. */
	assert(allocations == 0);
	assert(credential_references == 0);
	assert(held_spinlocks == 0);
	puts("GPU framework: dynamic registration, permissions, handles and retirement PASS");

	/* Checks bus ownership after the isolated allocation balance is proven. */
	test_pci_ownership();

	/* Succeeded: every framework assertion passed without a real GPU. */
	return 0;
}

/*
 * Supplies counted kernel allocations and one explicit failure checkpoint.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *allocation;

	/* Refuses the selected allocation before allocating any storage. */
	if (reject_allocation != 0) {
		/* Zero selects this call; earlier calls retain their usual behavior. */
		reject_allocation--;

		/* Only the selected allocation receives the injected failure. */
		if (reject_allocation == 0)
			return NULL;
	}

	/* Tracks each successful object until kern_free releases it. */
	allocation = calloc(count, size);
	if (allocation == NULL)
		return NULL;

	allocations++;

	/* Succeeded: the caller owns one counted allocation. */
	return allocation;
}

/*
 * Supplies the uninitialized-allocation entry through the same accounting.
 */
void *
kern_malloc(
	size_t size)
{
	void *allocation;

	/* Retains identical failure and lifetime accounting for both allocators. */
	allocation = kern_calloc(1, size);
	if (allocation == NULL)
		return NULL;

	/* Succeeded: returns the counted storage. */
	return allocation;
}

/*
 * Releases one counted object after its final owner retires it.
 */
void
kern_free(
	void *allocation)
{
	/* Mirrors the kernel's harmless release of a missing allocation. */
	if (allocation == NULL)
		return;

	/* Decrements the count only for storage owned by this test process. */
	assert(allocations != 0);
	allocations--;
	free(allocation);

	/* Succeeded: the counted allocation has been released. */
	return;
}

/*
 * Initializes the host spinlock without requiring a kernel scheduler.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Describes an initially unowned lock for the production core. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;

	/* Succeeded: the fixture lock is ready for its first owner. */
	return;
}

/*
 * Acquires an IRQ-style lock and makes callback violations observable.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	unsigned busy;

	/* The host test has one caller, so recursive acquisition is a defect. */
	busy = __atomic_exchange_n(&lock->held.value, 1U, __ATOMIC_ACQUIRE);
	assert(busy == 0);
	held_spinlocks++;

	/* Succeeded: returns the synthetic enabled-interrupt state. */
	return 1;
}

/*
 * Releases an IRQ-style lock and restores the synthetic interrupt state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Balances the acquisition before another callback can execute. */
	assert(enabled == 1);
	assert(held_spinlocks != 0);
	held_spinlocks--;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);

	/* Succeeded: the fixture exposes no remaining lock owner. */
	return;
}

/*
 * Records readiness notifications without installing a host event loop.
 */
void
poll_notify(
	void)
{
	/* Notification must not retain a core IRQ lock across a wakeup. */
	assert(held_spinlocks == 0);
	poll_notifications++;

	/* Succeeded: the retirement wakeup has been recorded. */
	return;
}

/*
 * Retains the currently selected synthetic caller credential.
 */
struct ucred *
cred_current_ref(
	void)
{
	/* A kernel caller supplies no implicit root credential. */
	if (credential_present == 0)
		return NULL;

	/* Accounts for the reference that the GPU open must release. */
	credential_references++;

	/* Succeeded: the caller holds the selected credential. */
	return &test_credential;
}

/*
 * Tests the effective user identity using the kernel's root convention.
 */
int
cred_is_superuser(
	const struct ucred *credential)
{
	/* Missing or non-root callers have no GPU administration authority. */
	if (credential == NULL)
		return 0;

	/* Distinguishes an ordinary identity from effective root. */
	if (credential->euid != 0)
		return 0;

	/* Succeeded: the effective user is root. */
	return 1;
}

/*
 * Releases a credential reference owned by the current operation.
 */
void
cred_release(
	struct ucred *credential)
{
	/* Matches the production helper's nullable release convention. */
	if (credential == NULL)
		return;

	/* Prevents a leak or duplicate release from passing silently. */
	assert(credential == &test_credential);
	assert(credential_references != 0);
	credential_references--;

	/* Succeeded: this operation holds no caller credential reference. */
	return;
}

/*
 * Copies a bounded synthetic userspace request into the kernel buffer.
 */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	/* Selects either the fixed request or the following embedded buffer copy. */
	if (reject_copyin != 0) {
		reject_copyin--;

		/* Only the selected copy faults; cleanup uses ordinary allocator behavior. */
		if (reject_copyin == 0)
			return EFAULT;
	}

	/* A missing userspace address faults before callback dispatch. */
	if (source == 0)
		return EFAULT;

	/* The fixture owns both ranges throughout the synchronous request. */
	memcpy(destination, (const void *)source, size);

	/* Succeeded: the kernel owns an independent request snapshot. */
	return 0;
}

/*
 * Copies a result to userspace or fails the selected publication once.
 */
int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	/* Models invalid output independently of an earlier successful copyin. */
	if (reject_copyout != 0) {
		reject_copyout--;
		return EFAULT;
	}

	/* Rejects a missing userspace result buffer. */
	if (destination == 0)
		return EFAULT;

	/* Publishes bytes only after all failure conditions were checked. */
	memcpy((void *)destination, source, size);

	/* Succeeded: the caller can observe the complete result. */
	return 0;
}

/*
 * Accepts PCI diagnostics while return-code assertions record each outcome.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* Expected rollback failures need no host logging backend. */
	(void)format;

	/* Succeeded: diagnostics add no host-side resource ownership. */
	return;
}

/* Opens one backend session without holding a core spinlock. */
static int
backend_open(
	void *opaque,
	void **result)
{
	struct test_backend *backend;
	struct test_session *session;

	/* Checks callback context before allocating anything. */
	assert(held_spinlocks == 0);
	backend = opaque;

	/* A failed backend open transfers no session to the GPU core. */
	if (backend->open_error != 0)
		return backend->open_error;

	/* Allocates one backend-owned session for this open description. */
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	session->backend = backend;
	backend->opens++;
	*result = session;

	/* Succeeded: close now owns the matching session release. */
	return 0;
}

/* Releases a backend session only after its resources have been destroyed. */
static void
backend_close(
	void *opaque,
	void *session_data)
{
	struct test_backend *backend;
	struct test_session *session;

	/* Verifies the callback's lifetime and execution context. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	assert(session->backend == backend);
	assert(session->live == 0);
	/* Final close releases any backend-owned presentation reservation. */
	if (backend->present_owner == session)
		backend->present_owner = NULL;

	backend->closes++;
	kern_free(session);

	/* Succeeded: all session-owned resources and the session are released. */
	return;
}

/* Returns the storage-only capabilities of the synthetic backend. */
static int
backend_get_info(
	void *opaque,
	void *session_data,
	struct gpu_info *info)
{
	struct test_session *session;
	struct file *target;
	struct drv_gpu_device *retiring;
	struct gpu_info nested;

	/* Confirms the query belongs to a live session of this instance. */
	assert(held_spinlocks == 0);
	session = session_data;
	assert(session->backend == opaque);

	/* Probes session admission once without recursively repeating the probe. */
	target = reentrant_target;
	if (target != NULL) {
		/* Clears the trigger before the nested operation enters a callback. */
		reentrant_target = NULL;
		memset(&nested, 0, sizeof(nested));
		nested.version = GPU_ABI_VERSION;
		nested.size = sizeof(nested);
		reentrant_error = cdev_file_ops.ioctl(
			target,
			GPU_GET_INFO,
			(uintptr_t)&nested);
	}

	/* Reentrant removal must retain this callback's backend session. */
	retiring = retire_target;
	if (retiring != NULL) {
		/* Clears the one-shot request before invoking ordinary removal. */
		retire_target = NULL;
		retirement_error = drv_gpu_unregister(retiring);
	}

	/* Reports a bounded storage contract without claiming display support. */
	info->max_resource_bytes = 4096;

	/* Different instances can expose different limits through the same ops. */
	if (session->backend->max_bytes != 0)
		info->max_resource_bytes = session->backend->max_bytes;

	/* Supplies a backend label independently of its per-instance limits. */
	strcpy(info->driver_name, "framework-test");

	/* Succeeded: the core can normalize and copy out these capabilities. */
	return 0;
}

/* Allocates one backend resource and records its owning session. */
static int
backend_create(
	void *opaque,
	void *session_data,
	const struct gpu_resource_create *request,
	void **result)
{
	struct test_backend *backend;
	struct test_session *session;
	struct test_resource *resource;

	/* Rejects a selected backend error without allocating state. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	assert(session->backend == backend);
	assert(request->bytes != 0);
	if (backend->create_error != 0)
		return backend->create_error;

	/* Allocates a resource whose owner is checked during destruction. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	resource->session = session;
	resource->bytes = request->bytes;
	resource->kind = 1;
	session->live++;
	backend->created++;
	backend->live++;
	*result = resource;

	/* Succeeded: exactly one matching destroy must follow. */
	return 0;
}

/* Destroys only the resource belonging to the supplied backend session. */
static void
backend_destroy(
	void *opaque,
	void *session_data,
	void *resource_data)
{
	struct test_backend *backend;
	struct test_session *session;
	struct test_resource *resource;

	/* Verifies that no foreign handle reached the backend callback. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	resource = resource_data;
	assert(session->backend == backend);
	assert(resource->session == session);
	assert(session->live != 0);
	assert(backend->live != 0);

	/* Balances the ownership recorded by the successful create callback. */
	session->live--;
	backend->live--;
	backend->destroyed++;
	kern_free(resource);

	/* Succeeded: the allocation no longer belongs to the session. */
	return;
}

/* Stops at a debugger-visible comparison when an operation reports wrongly. */
static void
expect_error(
	int actual,
	int expected)
{
	/* Reports both codes before stopping the failing acceptance check. */
	if (actual != expected) {
		fprintf(
			stderr,
			"GPU framework error: got %d, expected %d\n",
			actual,
			expected);
		abort();
	}

	/* Succeeded: the operation reported its expected outcome. */
	return;
}

/* Opens through the real cdev frontend with an explicitly retained inode. */
static int
open_file(
	struct test_file *opened,
	const char *name,
	int flags)
{
	int error;

	/* Acquires the immutable generation exactly as devfs lookup would. */
	memset(opened, 0, sizeof(*opened));
	opened->device = cdev_find_ref(name);
	if (opened->device == NULL)
		return ENOENT;

	/* Gives cdev_open_file a real inode and independently mutable f_data. */
	opened->inode.i_data = opened->device;
	opened->file.f_inode = &opened->inode;
	opened->file.f_ops = &cdev_file_ops;
	atomic_store_release(&opened->file.f_flags, (unsigned)flags);
	error = cdev_file_ops.open(&opened->file);
	if (error != 0) {
		cdev_release(opened->device);
		opened->device = NULL;
		return error;
	}

	/* Succeeded: the fixture now owns an open session and inode reference. */
	return 0;
}

/* Models the final close followed by release of the devfs inode reference. */
static void
close_file(
	struct test_file *opened)
{
	int error;

	/* The real cdev frontend must release all per-open backend state. */
	error = cdev_file_ops.close(&opened->file);
	expect_error(error, 0);
	cdev_release(opened->device);
	opened->device = NULL;

	/* Succeeded: the session and synthetic inode references are released. */
	return;
}

/* Builds one well-formed request so each test changes only its own input. */
static void
prepare_create(
	struct gpu_resource_create *request)
{
	/* Initializes every reserved and output field before kernel copyin. */
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->bytes = 4096;
	request->usage = GPU_RESOURCE_USAGE_STORAGE;

	/* Succeeded: the request is ready for one controlled test variation. */
	return;
}

/* Destroys the given handle through the real ioctl dispatch chain. */
static int
destroy_handle(
	struct test_file *opened,
	uint64_t handle)
{
	struct gpu_resource_destroy request;
	int error;

	/* Builds the exact fixed-width userspace release request. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = handle;
	error = cdev_file_ops.ioctl(
		&opened->file,
		GPU_RESOURCE_DESTROY,
		(uintptr_t)&request);
	if (error != 0)
		return error;

	/* Succeeded: the core destroyed the session's matching resource. */
	return 0;
}

/* Rejects malformed contracts without publishing or retaining a device. */
static void
test_validation(
	void)
{
	struct drv_gpu_device *registered;
	const struct drv_gpu_ops *ops;
	struct drv_gpu_ops interface;
	struct test_backend backend;
	int error;

	/* Missing mandatory callbacks cannot become externally visible. */
	interface = backend_ops;
	interface.open = NULL;

	/* Initializes backend counters without creating a live session. */
	memset(&backend, 0, sizeof(backend));

	/* Missing operations or an output location cannot transfer ownership. */
	error = drv_gpu_register(NULL, &backend, &registered);
	expect_error(error, EINVAL);
	assert(registered == NULL);
	error = drv_gpu_register(&backend_ops, &backend, NULL);
	expect_error(error, EINVAL);
	error = drv_gpu_unregister(NULL);
	expect_error(error, EINVAL);

	/* Passes this instance and its borrowed operations to the generic core. */
	registered = NULL;
	ops = &interface;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, EINVAL);
	assert(registered == NULL);

	/* A different interface version cannot silently use the current layout. */
	interface = backend_ops;
	interface.version++;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, EOPNOTSUPP);

	/* Resource support requires both allocation and infallible destruction. */
	interface = backend_ops;
	interface.resource_destroy = NULL;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, EINVAL);
	assert(registered == NULL);
	assert(allocations == 0);

	/* Succeeded: malformed operations retained no registration. */
	return;
}

/* Publishes every registration immediately without a GPU-specific boot hook. */
static void
test_immediate_registration(
	void)
{
	struct drv_gpu_device *registered;
	struct test_backend backend;
	struct cdev *device;
	int error;

	/* Registers before any GPU or character-device initialization hook. */
	memset(&backend, 0, sizeof(backend));
	error = drv_gpu_register(&backend_ops, &backend, &registered);
	expect_error(error, 0);
	assert(registered != NULL);
	device = cdev_find_ref("gpu0");
	assert(device != NULL);
	cdev_release(device);

	/* A caller with no open sessions can immediately retire the instance. */
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	registered = NULL;
	device = cdev_find_ref("gpu0");
	assert(device == NULL);
	assert(allocations == 0);

	/* Succeeded: ordinary registration published and retired directly. */
	return;
}

/* Exercises session rights, non-aliasing handles and delayed retirement. */
static void
test_access_and_handles(
	void)
{
	struct drv_gpu_device *registered;
	const struct drv_gpu_ops *ops;
	struct test_backend backend;
	struct test_file first;
	struct test_file second;
	struct test_file reader;
	struct test_file denied;
	struct gpu_resource_create request;
	struct gpu_info info;
	struct cdev *retained;
	struct inode stale_inode;
	struct file stale_file;
	uint64_t first_handle;
	uint64_t second_handle;
	unsigned created;
	unsigned notifications;
	short revents;
	int error;

	/* Publishes a storage backend through ordinary dynamic registration. */
	memset(&backend, 0, sizeof(backend));

	/* Passes this instance and its borrowed operations to the generic core. */
	registered = NULL;
	ops = &backend_ops;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, 0);

	/* Ordinary and absent credentials cannot open the administration node. */
	test_credential.euid = 1000;
	error = open_file(&denied, "gpu0", O_RDWR);
	expect_error(error, EACCES);
	test_credential.euid = 0;
	credential_present = 0;
	error = open_file(&denied, "gpu0", O_RDWR);
	expect_error(error, EACCES);
	credential_present = 1;

	/* A backend open failure must leave no session counted by retirement. */
	backend.open_error = EIO;
	error = open_file(&denied, "gpu0", O_RDWR);
	expect_error(error, EIO);
	backend.open_error = 0;

	/* Opens two resource owners and one query-only open description. */
	error = open_file(&first, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&second, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&reader, "gpu0", O_RDONLY);
	expect_error(error, 0);

	/* A valid query returns normalized ABI fields and backend information. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = cdev_file_ops.ioctl(&reader.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, 0);
	assert(info.capabilities == GPU_CAP_RESOURCE);
	assert(info.max_resources == GPU_SESSION_RESOURCE_MAX);
	assert(info.max_resource_bytes == 4096);

	/* Read-only rights cannot be upgraded by changing mutable file flags. */
	prepare_create(&request);
	atomic_store_release(&reader.file.f_flags, O_RDWR);
	error = cdev_file_ops.ioctl(
		&reader.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, EACCES);

	/* Every successful open retains its original authorization on reuse. */
	test_credential.euid = 1000;
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, 0);
	first_handle = request.handle;
	assert(first_handle != 0);
	test_credential.euid = 0;

	/* A second session cannot release the first session's allocation. */
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&second.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, 0);
	second_handle = request.handle;
	assert(second_handle != first_handle);
	error = destroy_handle(&second, first_handle);
	expect_error(error, EINVAL);
	error = destroy_handle(&first, first_handle);
	expect_error(error, 0);
	error = destroy_handle(&first, first_handle);
	expect_error(error, EINVAL);

	/* Reusing the freed slot cannot revive its old generation handle. */
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, 0);
	assert(request.handle != first_handle);
	error = destroy_handle(&first, first_handle);
	expect_error(error, EINVAL);

	/* A userspace publication fault destroys the unreturned allocation. */
	created = backend.created;
	prepare_create(&request);
	reject_copyout = 1;
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, EFAULT);
	assert(backend.created == created + 1);
	assert(backend.live == 2);

	/* Backend allocation failures cannot consume a live handle or resource. */
	backend.create_error = EIO;
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, EIO);
	backend.create_error = 0;
	assert(backend.live == 2);

	/* Fixed-width requests reject incompatible layouts before dispatch. */
	prepare_create(&request);
	request.version++;
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, EINVAL);
	error = cdev_file_ops.ioctl(&first.file, GPU_RESOURCE_CREATE, 0);
	expect_error(error, EFAULT);
	error = cdev_file_ops.ioctl(&first.file, 0x1234UL, 0);
	expect_error(error, EOPNOTSUPP);

	/* A retained inode models lookup finishing just before unpublication. */
	retained = cdev_find_ref("gpu0");
	assert(retained != NULL);
	memset(&stale_inode, 0, sizeof(stale_inode));
	stale_inode.i_data = retained;

	/* Attempts an open using the generation acquired before retirement. */
	memset(&stale_file, 0, sizeof(stale_file));
	stale_file.f_inode = &stale_inode;
	atomic_store_release(&stale_file.f_flags, O_RDWR);
	notifications = poll_notifications;
	error = drv_gpu_unregister(registered);
	expect_error(error, EBUSY);
	assert(poll_notifications > notifications);
	assert(registered != NULL);
	assert(backend.closes == 0);

	/* New lookups and stale-inode opens cannot reach the retired backend. */
	error = open_file(&denied, "gpu0", O_RDWR);
	expect_error(error, ENOENT);
	error = cdev_file_ops.open(&stale_file);
	expect_error(error, ENODEV);
	error = cdev_file_ops.ioctl(&first.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, ENODEV);
	error = cdev_file_ops.poll(&first.file, POLLIN, &revents);
	expect_error(error, 0);
	assert(revents == (POLLERR | POLLHUP));

	/* Final closes still destroy resources before releasing backend sessions. */
	close_file(&first);
	close_file(&second);
	close_file(&reader);
	assert(backend.live == 0);
	assert(backend.created == backend.destroyed);
	assert(backend.opens == backend.closes);
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	registered = NULL;

	/* The cdev generation keeps its wrapper alive through the final inode. */
	cdev_release(retained);
	assert(allocations == 0);
	assert(credential_references == 0);

	/* Succeeded: authorization and handle lifetimes remained isolated. */
	return;
}

/* Reports unsupported resource operations without calling missing callbacks. */
static void
test_missing_operations(
	void)
{
	struct drv_gpu_ops interface;
	struct drv_gpu_device *registered;
	const struct drv_gpu_ops *ops;
	struct test_backend backend;
	struct test_file opened;
	struct gpu_resource_create request;
	int error;

	/* A query-only backend is valid when capability and callbacks agree. */
	interface = backend_ops;
	interface.capabilities = 0;
	interface.resource_create = NULL;
	interface.resource_destroy = NULL;

	/* Starts with no live sessions or resources in the query-only backend. */
	memset(&backend, 0, sizeof(backend));

	/* Passes this instance and its borrowed operations to the generic core. */
	registered = NULL;
	ops = &interface;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, 0);
	error = open_file(&opened, "gpu0", O_RDWR);
	expect_error(error, 0);

	/* Missing optional operations fail without allocating backend storage. */
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&opened.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, EOPNOTSUPP);
	assert(backend.created == 0);

	/* Releases the query-only session and its device generation. */
	close_file(&opened);
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	assert(allocations == 0);

	/* Succeeded: optional callbacks were never called when absent. */
	return;
}

/* Bounds one session's resources without excluding independent sessions. */
static void
test_capacity_and_reentry(
	void)
{
	struct drv_gpu_device *registered;
	const struct drv_gpu_ops *ops;
	struct test_backend backend;
	struct test_file first;
	struct test_file second;
	struct gpu_info info;
	struct gpu_resource_create request;
	unsigned index;
	int error;

	/* Publishes two independent sessions of the same storage backend. */
	memset(&backend, 0, sizeof(backend));

	/* Passes this instance and its borrowed operations to the generic core. */
	registered = NULL;
	ops = &backend_ops;
	error = drv_gpu_register(ops, &backend, &registered);
	expect_error(error, 0);
	error = open_file(&first, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&second, "gpu0", O_RDWR);
	expect_error(error, 0);

	/* Reentry into the active description returns busy without deadlocking. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	reentrant_target = &first.file;
	reentrant_error = 0;
	error = cdev_file_ops.ioctl(&first.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, 0);
	expect_error(reentrant_error, EBUSY);
	assert(reentrant_target == NULL);

	/* A callback may reach another session without a device-wide lock. */
	reentrant_target = &second.file;
	reentrant_error = EBUSY;
	error = cdev_file_ops.ioctl(&first.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, 0);
	expect_error(reentrant_error, 0);
	assert(reentrant_target == NULL);

	/* Fills the first session's public resource limit with real callbacks. */
	for (index = 0; index < GPU_SESSION_RESOURCE_MAX; index++) {
		/* Every successful handle occupies exactly one owned table slot. */
		prepare_create(&request);
		error = cdev_file_ops.ioctl(
			&first.file,
			GPU_RESOURCE_CREATE,
			(uintptr_t)&request);
		expect_error(error, 0);
		assert(request.handle != 0);
	}

	/* Exhaustion is reported before another backend resource is allocated. */
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&first.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, ENOSPC);
	assert(backend.created == GPU_SESSION_RESOURCE_MAX);

	/* A full neighboring session does not consume this session's capacity. */
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&second.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, 0);
	assert(backend.created == GPU_SESSION_RESOURCE_MAX + 1U);

	/* Final close releases every occupied slot before backend session close. */
	close_file(&first);
	close_file(&second);
	assert(backend.live == 0);
	assert(backend.created == backend.destroyed);
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	assert(allocations == 0);

	/* Succeeded: independent sessions retained separate admission and resources. */
	return;
}

/* Defers backend retirement requested from inside its own active callback. */
static void
test_callback_retirement(
	void)
{
	struct drv_gpu_device *registered;
	struct test_backend backend;
	struct test_file opened;
	struct gpu_info info;
	int error;

	/* Establishes a normal live session before requesting callback removal. */
	memset(&backend, 0, sizeof(backend));
	error = drv_gpu_register(&backend_ops, &backend, &registered);
	expect_error(error, 0);
	error = open_file(&opened, "gpu0", O_RDWR);
	expect_error(error, 0);

	/* The active callback can withdraw its node without freeing its session. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	retire_target = registered;
	retirement_error = 0;
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, 0);
	expect_error(retirement_error, EBUSY);
	assert(retire_target == NULL);
	assert(backend.closes == 0);

	/* Future operations see removal while the final close still owns cleanup. */
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_INFO, (uintptr_t)&info);
	expect_error(error, ENODEV);
	close_file(&opened);
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	registered = NULL;
	assert(backend.opens == backend.closes);
	assert(allocations == 0);

	/* Succeeded: removal reentry preserved callback lifetime without locking it. */
	return;
}

/* Rolls back wrapper and node allocation or a conflicting common-device name. */
static void
test_registration_rollback(
	void)
{
	struct drv_gpu_device *registered;
	struct test_backend backend;
	struct cdev *conflict;
	struct cdev_ops empty_ops;
	unsigned failure_at;
	unsigned baseline;
	int error;

	/* Failed registration cannot retain the caller's backend state. */
	memset(&backend, 0, sizeof(backend));

	/* Refuses each of the separate GPU-wrapper and cdev-node allocations. */
	for (failure_at = 1; failure_at <= 2; failure_at++) {
		/* A failed allocation must leave no handle, node or borrowed state. */
		reject_allocation = failure_at;
		error = drv_gpu_register(&backend_ops, &backend, &registered);
		expect_error(error, ENOMEM);
		assert(registered == NULL);
		assert(reject_allocation == 0);
		assert(allocations == 0);
	}

	/* Occupies the expected numeric name in the common character namespace. */
	memset(&empty_ops, 0, sizeof(empty_ops));
	error = cdev_register_managed(
		"gpu0",
		0,
		&empty_ops,
		NULL,
		NULL,
		&conflict);
	expect_error(error, 0);
	baseline = allocations;

	/* A name conflict releases the candidate without disturbing its owner. */
	error = drv_gpu_register(&backend_ops, &backend, &registered);
	expect_error(error, EEXIST);
	assert(registered == NULL);
	assert(allocations == baseline);
	error = cdev_unregister(conflict);
	expect_error(error, 0);
	cdev_release(conflict);

	/* Retrying with the same operations publishes and retires normally. */
	error = drv_gpu_register(&backend_ops, &backend, &registered);
	expect_error(error, 0);
	error = drv_gpu_unregister(registered);
	expect_error(error, 0);
	registered = NULL;
	assert(allocations == 0);

	/* Succeeded: neither allocation nor publication failure leaks a device. */
	return;
}

/* Keeps many shared-operation instances independent across dynamic replacement. */
static void
test_dynamic_devices(
	void)
{
	struct drv_gpu_device *registered[TEST_GPU_COUNT];
	struct test_backend backends[TEST_GPU_COUNT];
	struct test_backend replacement;
	struct test_file opened;
	struct gpu_info info;
	struct cdev *retained;
	struct cdev **snapshot;
	struct inode stale_inode;
	struct file stale_file;
	char name[32];
	unsigned index;
	unsigned count;
	int error;

	/* Exceeds both former fixed limits while sharing exactly one ops table. */
	memset(backends, 0, sizeof(backends));
	for (index = 0; index < TEST_GPU_COUNT; index++) {
		/* Gives each instance a distinct query result to detect misdispatch. */
		backends[index].max_bytes = 4096U + index;
		error = drv_gpu_register(
			&backend_ops,
			&backends[index],
			&registered[index]);
		expect_error(error, 0);
		assert(registered[index] != NULL);
	}

	/* A coherent common-device snapshot includes every published GPU. */
	error = cdev_snapshot_alloc(&snapshot, &count);
	expect_error(error, 0);
	assert(count == TEST_GPU_COUNT);

	/* Releases the snapshot's independent references without retiring nodes. */
	for (index = 0; index < count; index++)
		cdev_release(snapshot[index]);

	/* Snapshot storage belongs to its caller rather than to the registry. */
	kern_free(snapshot);

	/* Resolves each dynamic name and dispatches to its own private state. */
	for (index = 0; index < TEST_GPU_COUNT; index++) {
		/* Opens a fresh session through the real common-device frontend. */
		snprintf(name, sizeof(name), "gpu%u", index);
		error = open_file(&opened, name, O_RDONLY);
		expect_error(error, 0);

		/* The shared table must not cause one instance to use another's data. */
		memset(&info, 0, sizeof(info));
		info.version = GPU_ABI_VERSION;
		info.size = sizeof(info);
		error = cdev_file_ops.ioctl(
			&opened.file,
			GPU_GET_INFO,
			(uintptr_t)&info);
		expect_error(error, 0);
		assert(info.max_resource_bytes == backends[index].max_bytes);
		close_file(&opened);
		assert(backends[index].opens == 1);
		assert(backends[index].closes == 1);
	}

	/* Keeps one old inode across reuse of its visible numeric device name. */
	retained = cdev_find_ref("gpu5");
	assert(retained != NULL);
	memset(&stale_inode, 0, sizeof(stale_inode));
	stale_inode.i_data = retained;

	/* The synthetic file retains the exact retired cdev generation. */
	memset(&stale_file, 0, sizeof(stale_file));
	stale_file.f_inode = &stale_inode;
	atomic_store_release(&stale_file.f_flags, O_RDWR);

	/* Frees the numeric slot while the stale inode still owns its wrapper. */
	error = drv_gpu_unregister(registered[5]);
	expect_error(error, 0);
	registered[5] = NULL;

	/* A later registration uses ordinary registration and distinct state. */
	memset(&replacement, 0, sizeof(replacement));
	replacement.max_bytes = 8192;
	error = drv_gpu_register(&backend_ops, &replacement, &registered[5]);
	expect_error(error, 0);
	error = open_file(&opened, "gpu5", O_RDONLY);
	expect_error(error, 0);
	close_file(&opened);
	assert(replacement.opens == 1);
	assert(backends[5].opens == 1);

	/* Reused names must never let the old inode enter the new backend. */
	error = cdev_file_ops.open(&stale_file);
	expect_error(error, ENODEV);
	assert(replacement.opens == 1);
	cdev_release(retained);

	/* Repeated late registration reuses one hole without losing its peers. */
	for (index = 0; index < 3; index++) {
		/* Removal consumes the handle before a fresh instance takes its slot. */
		error = drv_gpu_unregister(registered[5]);
		expect_error(error, 0);
		registered[5] = NULL;
		error = drv_gpu_register(&backend_ops, &replacement, &registered[5]);
		expect_error(error, 0);
	}

	/* Every instance releases its own wrapper and common-device generation. */
	for (index = 0; index < TEST_GPU_COUNT; index++) {
		/* No session remains, so removal must consume this handle at once. */
		error = drv_gpu_unregister(registered[index]);
		expect_error(error, 0);
		registered[index] = NULL;
	}

	/* Succeeded: dynamic capacity and replacement leave no retained objects. */
	assert(allocations == 0);
	return;
}

/* Verifies PCI ownership through a driver-local ordinary GPU API adapter. */
static void
test_pci_ownership(
	void)
{
	static const struct drv_pci_id ids[] = {
		{0x1234U, 0x5678U, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0, 0, 0}
	};
	struct drv_pci_bus_ops operations;
	struct drv_pci_bus *bus;
	struct drv_pci_device *device;
	struct drv_pci_driver *owner;
	struct test_file opened;
	struct test_file denied;
	struct gpu_resource_create request;
	unsigned baseline;
	int error;

	/* Freezes the driver-local adapter before PCI can invoke its callbacks. */
	pci_service.publish = pci_publish_gpu;
	pci_service.unpublish = pci_unpublish_gpu;

	/* Enumerates one simulated display function through the production core. */
	memset(&operations, 0, sizeof(operations));
	operations.config_read = pci_config_read;
	operations.config_write = pci_config_write;
	error = drv_pci_init();
	expect_error(error, 0);
	error = drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus);
	expect_error(error, 0);
	error = drv_pci_bus_scan(bus);
	expect_error(error, 0);
	device = drv_pci_find_id(0x1234U, 0x5678U, NULL);
	assert(device != NULL);

	/* PCI retains its enumerated bus and device independently of attachment. */
	baseline = allocations;

	/* Registering the PCI driver attaches hardware before publishing its GPU. */
	memset(&pci_driver, 0, sizeof(pci_driver));
	pci_driver.name = "gpu-registration-fixture";
	pci_driver.ids = ids;
	pci_driver.id_count = 1;
	pci_driver.attach = pci_attach;
	pci_driver.detach = pci_detach;
	error = drv_pci_driver_register(&pci_driver);
	expect_error(error, 0);
	assert(pci_fixture.attaches == 1);
	assert(pci_fixture.publishes == 1);
	assert(pci_fixture.hardware_live == 1);
	assert(pci_fixture.gpu != NULL);

	/* Keeps a real session and resource alive when PCI first attempts removal. */
	error = open_file(&opened, "gpu0", O_RDWR);
	expect_error(error, 0);
	prepare_create(&request);
	error = cdev_file_ops.ioctl(
		&opened.file,
		GPU_RESOURCE_CREATE,
		(uintptr_t)&request);
	expect_error(error, 0);
	error = drv_pci_device_detach(device, DRV_PCI_DETACH_FORCE);
	expect_error(error, EBUSY);
	assert(pci_fixture.detaches == 0);
	assert(pci_fixture.hardware_live == 1);
	assert(pci_fixture.gpu != NULL);
	owner = drv_pci_device_driver(device);
	assert(owner == &pci_driver);

	/* The failed detach has nevertheless withdrawn new GPU opens. */
	error = open_file(&denied, "gpu0", O_RDWR);
	expect_error(error, ENOENT);

	/* A retained file cleans up before PCI retries the checked service removal. */
	close_file(&opened);
	assert(pci_fixture.backend.live == 0);
	assert(pci_fixture.backend.opens == pci_fixture.backend.closes);
	error = drv_pci_device_detach(device, 0);
	expect_error(error, 0);
	assert(pci_fixture.gpu == NULL);
	assert(pci_fixture.hardware_live == 0);
	assert(pci_fixture.detaches == 1);
	assert(allocations == baseline + 1U);

	/* A failed ordinary GPU registration makes PCI roll hardware back. */
	reject_allocation = 1;
	error = drv_pci_device_probe(device);
	expect_error(error, ENOMEM);
	assert(pci_fixture.gpu == NULL);
	assert(pci_fixture.hardware_live == 0);
	assert(pci_fixture.detaches == 2);
	owner = drv_pci_device_driver(device);
	assert(owner == NULL);

	/* A later successful probe and removal use the same static adapter. */
	error = drv_pci_device_probe(device);
	expect_error(error, 0);
	error = drv_pci_device_detach(device, 0);
	expect_error(error, 0);
	error = drv_pci_driver_unregister(&pci_driver);
	expect_error(error, 0);
	assert(allocations == baseline);
	assert(credential_references == 0);

	/* Succeeded: only PCI's permanent enumeration objects remain core-owned. */
	puts("PCI to GPU registration: ordering, busy removal and rollback PASS");
	return;
}

/* Supplies one display function without a physical configuration space. */
static int
pci_config_read(
	void *argument,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *result)
{
	/* The simulated identity has no mutable state or access-width effects. */
	(void)argument;
	(void)width;

	/* Marks all unused bus locations as absent for ordinary enumeration. */
	if (address->device != 1 || address->function != 0) {
		*result = 0xffffffffU;
		return 0;
	}

	/* Identifies the fixture as a display device with no mapped BARs. */
	*result = 0;
	if (offset == 0) {
		*result = 0x56781234U;
	} else if (offset == 8) {
		*result = 0x03000001U;
	}

	/* Succeeded: returns the selected immutable configuration word. */
	return 0;
}

/* Accepts ordinary enumeration writes for the immutable simulated device. */
static int
pci_config_write(
	void *argument,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t word)
{
	/* No BAR or command state is needed for the lifecycle-only fixture. */
	(void)argument;
	(void)address;
	(void)offset;
	(void)width;
	(void)word;

	/* Succeeded: PCI may continue after the configuration transaction. */
	return 0;
}

/* Initializes hardware state and stages its driver-local service adapter. */
static int
pci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	int error;

	/* The matched ID does not alter this fixture's one hardware contract. */
	(void)id;

	/* Establishes attachment lifetime before PCI publishes the subsystem. */
	assert(pci_fixture.hardware_live == 0);
	pci_fixture.hardware_live = 1;
	pci_fixture.attaches++;
	error = drv_pci_device_set_driver_data(device, &pci_fixture);
	expect_error(error, 0);
	error = drv_pci_device_set_service(device, &pci_service, &pci_fixture);
	expect_error(error, 0);
	assert(pci_fixture.gpu == NULL);

	/* Succeeded: PCI now owns the next publication step. */
	return 0;
}

/* Releases hardware only after PCI completes ordinary GPU unregistration. */
static int
pci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	/* Neither the device identity nor force bypasses checked GPU ownership. */
	(void)device;
	(void)flags;

	/* Verifies that all backend callbacks and resource owners have drained. */
	assert(pci_fixture.hardware_live == 1);
	assert(pci_fixture.gpu == NULL);
	assert(pci_fixture.backend.live == 0);
	assert(pci_fixture.backend.opens == pci_fixture.backend.closes);
	pci_fixture.hardware_live = 0;
	pci_fixture.detaches++;

	/* Succeeded: PCI may clear the hardware binding. */
	return 0;
}

/* Calls the bus-independent GPU registration API from the PCI-owned step. */
static int
pci_publish_gpu(
	struct drv_pci_device *device,
	void *argument)
{
	struct pci_gpu_fixture *fixture;
	struct drv_pci_driver *owner;
	int error;

	/* Confirms hardware attachment and PCI binding precede GPU visibility. */
	fixture = argument;
	assert(fixture == &pci_fixture);
	assert(fixture->hardware_live == 1);
	assert(fixture->gpu == NULL);
	owner = drv_pci_device_driver(device);
	assert(owner == &pci_driver);
	fixture->publishes++;

	/* A normal GPU register call is all this bus-local adapter needs. */
	error = drv_gpu_register(&backend_ops, &fixture->backend, &fixture->gpu);
	if (error != 0)
		return error;

	/* Succeeded: this instance retains its ordinary GPU registration handle. */
	return 0;
}

/* Keeps the ordinary GPU handle until checked removal releases its users. */
static int
pci_unpublish_gpu(
	struct drv_pci_device *device,
	void *argument)
{
	struct pci_gpu_fixture *fixture;
	int error;

	/* PCI's binding identity is independent of the ordinary GPU API. */
	(void)device;

	/* Retains the instance throughout every unsuccessful removal attempt. */
	fixture = argument;
	assert(fixture == &pci_fixture);
	assert(fixture->hardware_live == 1);
	assert(fixture->gpu != NULL);
	error = drv_gpu_unregister(fixture->gpu);
	if (error != 0)
		return error;

	/* Successful unregistration consumes the caller's GPU handle. */
	fixture->gpu = NULL;

	/* Succeeded: PCI can now call the hardware destructor. */
	return 0;
}

/* Supplies an optional backend with independently testable protocol operations. */
static void
optional_ops_prepare(
	struct drv_gpu_ops *ops)
{
	/* Retains the ordinary lifecycle while advertising each implemented addition. */
	*ops = backend_ops;
	ops->capabilities = GPU_CAP_RESOURCE | GPU_CAP_CAPSET | GPU_CAP_BLOB |
		GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_PRESENT;
	ops->get_capset = backend_capset;
	ops->blob_create = backend_blob;
	ops->resource_read = backend_resource_read;
	ops->resource_write = backend_resource_write;
	ops->command = backend_command;
	ops->present = backend_present;

	/* Succeeded: the optional contract is ready for ordinary registration. */
	return;
}

/* Supplies a small deterministic capability payload for the copied query. */
static int
backend_capset(
	void *opaque,
	void *session_data,
	struct gpu_capset *request)
{
	struct test_backend *backend;
	struct test_session *session;

	/* Confirms that capset dispatch retains session ownership and no spinlock. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	assert(session->backend == backend);

	/* The synthetic backend implements one protocol identifier and version. */
	if (request->capset_id != 4 || request->capset_version != 0)
		return EOPNOTSUPP;

	/* The complete capability payload must fit the caller's supplied capacity. */
	if (request->capacity < 8)
		return ENOSPC;

	/* Writes a recognizable initialized payload into the inline kernel buffer. */
	memcpy(request->data, "CAPSET04", 8);
	request->bytes = 8;

	/* A malformed backend response tests the core's output-bound enforcement. */
	if (backend->bad_capset != 0)
		request->bytes = request->capacity + 1U;

	/* Succeeded: the core must still validate the returned payload extent. */
	return 0;
}

/* Creates a typed blob using the same allocation ownership as ordinary storage. */
static int
backend_blob(
	void *opaque,
	void *session_data,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *resource_id)
{
	struct test_backend *backend;
	struct gpu_resource_create storage;
	struct test_resource *resource;
	int error;

	/* Reuses the existing allocator so failure accounting remains independent. */
	backend = opaque;
	memset(&storage, 0, sizeof(storage));
	storage.bytes = request->bytes;
	error = backend_create(opaque, session_data, &storage, result);
	if (error != 0)
		return error;

	/* Gives the core a protocol identifier distinct from its opaque handle. */
	resource = *result;
	resource->kind = 2;
	*resource_id = backend->created;

	/* A missing protocol identity requires core rollback of the new object. */
	if (backend->bad_blob_id != 0)
		*resource_id = 0;

	/* Succeeded: destroy owns this allocation even for a rejected output ID. */
	return 0;
}

/* Reads a validated resource range into an exclusively kernel-owned buffer. */
static int
backend_resource_read(
	void *opaque,
	void *session_data,
	void *object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	struct test_backend *backend;
	struct test_session *session;
	struct test_resource *resource;

	/* Validates core dispatch ownership without interpreting any user pointer. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	resource = object;
	assert(session->backend == backend);
	assert(resource->session == session);
	assert(offset <= resource->bytes);
	assert(bytes <= resource->bytes - offset);
	assert(offset + bytes <= sizeof(resource->data));

	/* Copies ordinary test storage so the core's readback can be compared exactly. */
	memcpy(buffer, resource->data + (size_t)offset, bytes);
	backend->reads++;

	/* Succeeded: the supplied kernel buffer contains the requested bytes. */
	return 0;
}

/* Writes a validated kernel snapshot into a session-owned resource range. */
static int
backend_resource_write(
	void *opaque,
	void *session_data,
	void *object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	struct test_backend *backend;
	struct test_session *session;
	struct test_resource *resource;

	/* Confirms that bounds and ownership were resolved before callback dispatch. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	resource = object;
	assert(session->backend == backend);
	assert(resource->session == session);
	assert(offset <= resource->bytes);
	assert(bytes <= resource->bytes - offset);
	assert(offset + bytes <= sizeof(resource->data));

	/* Persists the copied bytes so later readback observes the exact snapshot. */
	memcpy(resource->data + (size_t)offset, buffer, bytes);
	backend->writes++;

	/* Succeeded: the backend consumed every byte before returning. */
	return 0;
}

/* Records one copied command word without claiming completion of GPU execution. */
static int
backend_command(
	void *opaque,
	void *session_data,
	const void *buffer,
	uint32_t bytes)
{
	struct test_backend *backend;
	struct test_session *session;

	/* Checks the protocol extent passed by the core rather than an encoded pointer. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	assert(session->backend == backend);
	assert(bytes != 0 && bytes <= GPU_COMMAND_MAX);
	assert((bytes & 3U) == 0);

	/* Captures the first command word before the staging buffer is released. */
	memcpy(&backend->last_command, buffer, sizeof(backend->last_command));
	backend->commands++;

	/* Succeeded: only command receipt is recorded by this mock protocol. */
	return 0;
}

/* Arbitrates display ownership only after the core resolves a bounded image. */
static int
backend_present(
	void *opaque,
	void *session_data,
	void *object,
	const struct gpu_present *request)
{
	struct test_backend *backend;
	struct test_session *session;
	struct test_resource *resource;

	/* A backend must receive only ordinary storage belonging to its session. */
	assert(held_spinlocks == 0);
	backend = opaque;
	session = session_data;
	resource = object;
	assert(session->backend == backend);
	assert(resource->session == session);
	assert(resource->kind == 1);
	assert(request->offset + (uint64_t)request->stride * request->height <=
		resource->bytes);

	/* Another live presenting session retains backend display ownership. */
	if (backend->present_owner != NULL && backend->present_owner != session)
		return EBUSY;

	/* Records the session whose final close must relinquish presentation. */
	backend->present_owner = session;
	backend->presents++;

	/* Succeeded: the display backend accepted one fully bounded image. */
	return 0;
}

/* Rejects incomplete optional contracts without publishing an unusable device. */
static void
test_optional_validation(
	void)
{
	struct drv_gpu_ops ops;
	struct drv_gpu_device *device;
	struct test_backend backend;
	int error;

	/* Initializes backend state without allocating a live registration. */
	memset(&backend, 0, sizeof(backend));

	/* Transfer support requires both directions even if only reads are attempted. */
	optional_ops_prepare(&ops);
	ops.resource_write = NULL;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);
	assert(device == NULL);

	/* Blob-only support still requires the shared resource destructor. */
	optional_ops_prepare(&ops);
	ops.capabilities = GPU_CAP_BLOB;
	ops.resource_create = NULL;
	ops.resource_destroy = NULL;
	ops.get_capset = NULL;
	ops.resource_read = NULL;
	ops.resource_write = NULL;
	ops.command = NULL;
	ops.present = NULL;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);

	/* Hidden optional callbacks cannot contradict advertised capabilities. */
	optional_ops_prepare(&ops);
	ops.capabilities &= ~GPU_CAP_COMMAND;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);

	/* Unknown capability bits remain unsupported after extending the known set. */
	optional_ops_prepare(&ops);
	ops.capabilities |= 0x80000000U;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EOPNOTSUPP);
	assert(allocations == 0);

	/* Succeeded: malformed extensions retained no device or backend ownership. */
	return;
}

/* Exercises copied capability data, typed blobs and original descriptor rights. */
static void
test_optional_requests(
	void)
{
	struct drv_gpu_ops ops;
	struct drv_gpu_device *device;
	struct test_backend backend;
	struct test_file opened;
	struct test_file foreign;
	struct test_file reader;
	struct test_file writer;
	struct gpu_capset capset;
	struct gpu_blob_create blob;
	struct gpu_resource_create storage;
	struct gpu_present present;
	uint64_t blob_handle;
	uint64_t storage_handle;
	unsigned created;
	unsigned index;
	int error;
	int comparison;

	/* Registers all optional operations through the unchanged device API. */
	optional_ops_prepare(&ops);
	memset(&backend, 0, sizeof(backend));
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, 0);
	error = open_file(&opened, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&foreign, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&reader, "gpu0", O_RDONLY);
	expect_error(error, 0);
	error = open_file(&writer, "gpu0", O_WRONLY);
	expect_error(error, 0);

	/* Queries one complete inline capset without transmitting a user pointer. */
	memset(&capset, 0, sizeof(capset));
	capset.version = GPU_ABI_VERSION;
	capset.size = sizeof(capset);
	capset.capset_id = 4;
	capset.capacity = GPU_CAPSET_MAX;
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_CAPSET, (uintptr_t)&capset);
	expect_error(error, 0);
	assert(capset.bytes == 8);
	comparison = memcmp(capset.data, "CAPSET04", 8);
	assert(comparison == 0);

	/* The unfilled inline reply tail must contain no prior kernel storage. */
	for (index = capset.bytes; index < GPU_CAPSET_MAX; index++)
		assert(capset.data[index] == 0);

	/* Output counts and excessive capacities cannot be accepted as new inputs. */
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_CAPSET, (uintptr_t)&capset);
	expect_error(error, EINVAL);
	capset.bytes = 0;
	capset.capacity = GPU_CAPSET_MAX + 1U;
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_CAPSET, (uintptr_t)&capset);
	expect_error(error, EINVAL);
	capset.capacity = GPU_CAPSET_MAX;
	capset.version++;
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_CAPSET, (uintptr_t)&capset);
	expect_error(error, EINVAL);
	capset.version = GPU_ABI_VERSION;

	/* A backend's oversized result is rejected before publishing any response. */
	backend.bad_capset = 1;
	error = cdev_file_ops.ioctl(&opened.file, GPU_GET_CAPSET, (uintptr_t)&capset);
	expect_error(error, EIO);
	backend.bad_capset = 0;

	/* Read-only descriptors cannot gain mutation rights from later flag changes. */
	atomic_store_release(&reader.file.f_flags, O_RDWR);
	error = cdev_file_ops.ioctl(&reader.file, GPU_BLOB_CREATE, 0);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&reader.file, GPU_RESOURCE_WRITE, 0);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&reader.file, GPU_COMMAND, 0);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&reader.file, GPU_PRESENT, 0);
	expect_error(error, EACCES);

	/* Write-only descriptors similarly cannot acquire readback authority. */
	atomic_store_release(&writer.file.f_flags, O_RDWR);
	error = cdev_file_ops.ioctl(&writer.file, GPU_RESOURCE_READ, 0);
	expect_error(error, EACCES);

	/* A blob copyout failure destroys the unreturned backend allocation. */
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = 4096;
	blob.flags = GPU_BLOB_MAPPABLE;
	created = backend.created;
	reject_copyout = 1;
	error = cdev_file_ops.ioctl(&opened.file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, EFAULT);
	assert(backend.created == created + 1U);
	assert(backend.live == 0);
	assert(blob.handle == 0);

	/* Missing protocol resource IDs also roll back successfully allocated blobs. */
	backend.bad_blob_id = 1;
	error = cdev_file_ops.ioctl(&opened.file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, EIO);
	assert(backend.live == 0);
	backend.bad_blob_id = 0;

	/* A normal blob returns distinct opaque and protocol identity fields. */
	error = cdev_file_ops.ioctl(&opened.file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, 0);
	assert(blob.handle != 0);
	assert(blob.resource_id != 0);
	blob_handle = blob.handle;

	/* Reusing nonzero output fields cannot accidentally create another blob. */
	error = cdev_file_ops.ioctl(&opened.file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, EINVAL);
	blob.handle = 0;
	blob.resource_id = 0;
	blob.flags = 0x80000000U;
	error = cdev_file_ops.ioctl(&opened.file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, EINVAL);

	/* Allocates ordinary storage for typed presentation and transfer checks. */
	prepare_create(&storage);
	storage.bytes = 4096;
	error = cdev_file_ops.ioctl(&opened.file, GPU_RESOURCE_CREATE, (uintptr_t)&storage);
	expect_error(error, 0);
	storage_handle = storage.handle;
	test_optional_transfer(&opened, &foreign, &backend, blob_handle);
	test_optional_transfer(&opened, &foreign, &backend, storage_handle);
	test_optional_command(&opened, &backend);
	test_optional_present(&opened, &backend, storage_handle, blob_handle);

	/* Gives another session valid storage before testing backend display exclusion. */
	prepare_create(&storage);
	storage.bytes = 1024;
	error = cdev_file_ops.ioctl(&foreign.file, GPU_RESOURCE_CREATE, (uintptr_t)&storage);
	expect_error(error, 0);
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.handle = storage.handle;
	present.width = 16;
	present.height = 16;
	present.stride = 64;
	present.format = GPU_PIXEL_BGRA8888;
	error = cdev_file_ops.ioctl(&foreign.file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EBUSY);

	/* Final close frees display ownership before another session presents. */
	close_file(&opened);
	error = cdev_file_ops.ioctl(&foreign.file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, 0);
	close_file(&foreign);
	close_file(&reader);
	close_file(&writer);
	assert(backend.live == 0);
	assert(backend.created == backend.destroyed);
	assert(backend.present_owner == NULL);
	error = drv_gpu_unregister(device);
	expect_error(error, 0);
	assert(allocations == 0);

	/* Succeeded: optional operations preserved capability, type and session ownership. */
	return;
}

/* Verifies exact resource bounds, session isolation and copied-buffer rollback. */
static void
test_optional_transfer(
	struct test_file *opened,
	struct test_file *foreign,
	struct test_backend *backend,
	uint64_t handle)
{
	struct gpu_transfer transfer;
	uint8_t source[16];
	uint8_t destination[16];
	unsigned writes;
	unsigned baseline;
	int error;
	int comparison;

	/* Copies one recognizable range into the final bytes of a resource. */
	memset(source, 0x5a, sizeof(source));
	memset(destination, 0, sizeof(destination));
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = handle;
	transfer.offset = 4096 - sizeof(source);
	transfer.address = (uintptr_t)source;
	transfer.bytes = sizeof(source);
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_WRITE, (uintptr_t)&transfer);
	expect_error(error, 0);

	/* Reads the same exact range through independent kernel staging memory. */
	transfer.address = (uintptr_t)destination;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, 0);
	comparison = memcmp(source, destination, sizeof(source));
	assert(comparison == 0);

	/* Foreign sessions cannot use a handle even when it names an existing resource. */
	error = cdev_file_ops.ioctl(&foreign->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EINVAL);

	/* One byte beyond the exact allocated extent is rejected before dispatch. */
	transfer.offset++;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EINVAL);
	transfer.offset = UINT64_MAX;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EINVAL);
	transfer.offset = 0;

	/* Per-copy limits are checked before resource and pointer access. */
	transfer.bytes = GPU_COPY_MAX + 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EINVAL);
	transfer.bytes = sizeof(source);
	transfer.reserved = 1;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EINVAL);
	transfer.reserved = 0;

	/* The encoded last-byte address must not wrap the native address space. */
	transfer.address = UINT64_MAX;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EFAULT);
	transfer.address = 0;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_WRITE, (uintptr_t)&transfer);
	expect_error(error, EFAULT);
	transfer.address = (uintptr_t)source;

	/* Payload copyin failure must free staging without changing backend bytes. */
	baseline = allocations;
	writes = backend->writes;
	reject_copyin = 2;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_WRITE, (uintptr_t)&transfer);
	expect_error(error, EFAULT);
	assert(backend->writes == writes);
	assert(allocations == baseline);

	/* Failed readback copyout retains the resource and releases staging memory. */
	transfer.address = (uintptr_t)destination;
	reject_copyout = 1;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_READ, (uintptr_t)&transfer);
	expect_error(error, EFAULT);
	assert(allocations == baseline);

	/* Allocation failure cannot dispatch a write with missing staging storage. */
	reject_allocation = 1;
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_WRITE, (uintptr_t)&transfer);
	expect_error(error, ENOMEM);
	assert(backend->writes == writes);
	assert(allocations == baseline);

	/* Succeeded: resource copies retained exact bounds and independent ownership. */
	return;
}

/* Exercises command-word bounds and prevents failed copies from reaching the driver. */
static void
test_optional_command(
	struct test_file *opened,
	struct test_backend *backend)
{
	struct gpu_command command;
	uint32_t words[2];
	unsigned submitted;
	unsigned baseline;
	int error;

	/* Submits one aligned copied command stream through the optional operation. */
	words[0] = 0x12345678U;
	words[1] = 0x87654321U;
	memset(&command, 0, sizeof(command));
	command.version = GPU_ABI_VERSION;
	command.size = sizeof(command);
	command.address = (uintptr_t)words;
	command.bytes = sizeof(words);
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, 0);
	assert(backend->last_command == words[0]);
	submitted = backend->commands;
	baseline = allocations;

	/* Non-word lengths, empty streams and oversized requests never reach dispatch. */
	command.bytes = 3;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EINVAL);
	command.bytes = 0;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EINVAL);
	command.bytes = GPU_COMMAND_MAX + 4U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EINVAL);
	command.bytes = sizeof(words);

	/* Unknown submission flags and wrapping pointers are refused independently. */
	command.flags = 1;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EINVAL);
	command.flags = 0;
	command.address = UINT64_MAX;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EFAULT);
	command.address = (uintptr_t)words;

	/* A failed payload copy cannot leave staging or partially submit a command. */
	reject_copyin = 2;
	error = cdev_file_ops.ioctl(&opened->file, GPU_COMMAND, (uintptr_t)&command);
	expect_error(error, EFAULT);
	assert(backend->commands == submitted);
	assert(allocations == baseline);

	/* Succeeded: command bounds and copy ownership prevent malformed dispatch. */
	return;
}

/* Checks that only complete images in ordinary storage reach presentation. */
static void
test_optional_present(
	struct test_file *opened,
	struct test_backend *backend,
	uint64_t storage,
	uint64_t blob)
{
	struct gpu_present present;
	unsigned presented;
	int error;

	/* Presents a valid image whose extent fits the supplied storage resource. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.handle = storage;
	present.width = 16;
	present.height = 16;
	present.stride = 64;
	present.format = GPU_PIXEL_BGRA8888;
	present.frame = 7;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, 0);
	presented = backend->presents;

	/* A protocol blob must be copied to storage before it can be presented. */
	present.handle = blob;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.handle = storage;

	/* Invalid dimensions are rejected before row-byte arithmetic. */
	present.width = 4097;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.width = 16;
	present.height = 0;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.height = 16;

	/* Row overlap, excessive row extent and unsupported formats cannot display. */
	present.stride = 63;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.stride = UINT32_MAX;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.stride = 64;
	present.format = 0;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.format = GPU_PIXEL_RGBA8888;
	present.offset = 4096;
	error = cdev_file_ops.ioctl(&opened->file, GPU_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	assert(backend->presents == presented);

	/* Succeeded: only the complete typed image reached backend display ownership. */
	return;
}
