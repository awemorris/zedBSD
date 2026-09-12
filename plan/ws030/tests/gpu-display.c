/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the real GPU display ioctl dispatcher with the existing host kernel collaborators.
 * The included fixture's main is never run; its allocator, file, cdev, PCI and backend helpers
 * remain the single implementation of that host model.
 */

#define main gpu_framework_unused_main
#include "../../ws014/tests/gpu-framework.c"
#undef main

/* One exclusive modeled plane retains its open owner until explicit lease release. */
struct display_test_state {
	struct test_backend *backend;
	void *owner;
	uint64_t next_lease;
	uint64_t lease;
	uint64_t completed;
	unsigned queries;
	unsigned modes;
	unsigned claims;
	unsigned releases;
	unsigned presents;
	unsigned waits;
	unsigned malformed_query;
};

/* The single-threaded display model is empty before and after its bounded main routine. */
static struct display_test_state display_state;

static int display_test_query(void *opaque, void *session, struct gpu_display_info *request);
static int display_test_mode(void *opaque, void *session, struct gpu_display_mode *request);
static int display_test_claim(void *opaque, void *session, struct gpu_display_claim *request);
static int display_test_release(void *opaque, void *session, const struct gpu_display_release *request);
static int display_test_present(void *opaque, void *session, void *object, struct gpu_display_present *request);
static int display_test_wait(void *opaque, void *session, struct gpu_display_wait *request);
static void display_test_discovery(struct test_file *opened, struct test_file *reader, struct test_file *writer);
static uint64_t display_test_ownership(struct test_file *opened, struct test_file *foreign, struct test_file *reader);
static void display_test_frames(struct test_file *opened, struct test_file *foreign, uint64_t lease);

/*
 * Checks native display admission, request normalization, lease ownership and bounded pixel access.
 */
int
main(
	void)
{
	struct drv_gpu_ops ops;
	struct drv_gpu_device *device;
	struct drv_gpu_display_ops display_ops;
	struct test_backend backend;
	struct test_file opened;
	struct test_file foreign;
	struct test_file reader;
	struct test_file writer;
	uint64_t lease;
	int error;

	/* Initializes the same ordinary backend used by the full framework fixture. */
	memset(&backend, 0, sizeof(backend));
	backend_ops.version = DRV_GPU_INTERFACE_VERSION;
	backend_ops.size = sizeof(backend_ops);
	backend_ops.capabilities = GPU_CAP_RESOURCE;
	backend_ops.open = backend_open;
	backend_ops.close = backend_close;
	backend_ops.get_info = backend_get_info;
	backend_ops.resource_create = backend_create;
	backend_ops.resource_destroy = backend_destroy;
	memset(&display_state, 0, sizeof(display_state));
	display_state.backend = &backend;
	display_state.next_lease = 1U;

	/* An advertised native display must provide its entire immutable operation contract. */
	memset(&display_ops, 0, sizeof(display_ops));
	optional_ops_prepare(&ops);
	ops.capabilities |= GPU_CAP_DISPLAY;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);
	ops.display = &display_ops;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);
	display_ops.query = display_test_query;
	display_ops.mode = display_test_mode;
	display_ops.claim = display_test_claim;
	display_ops.release = display_test_release;
	display_ops.present = display_test_present;
	display_ops.wait = display_test_wait;
	ops.capabilities &= ~GPU_CAP_DISPLAY;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, EINVAL);
	ops.capabilities |= GPU_CAP_DISPLAY;
	error = drv_gpu_register(&ops, &backend, &device);
	expect_error(error, 0);

	/* Different descriptions keep distinct original read/write and native lease authority. */
	error = open_file(&opened, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&foreign, "gpu0", O_RDWR);
	expect_error(error, 0);
	error = open_file(&reader, "gpu0", O_RDONLY);
	expect_error(error, 0);
	error = open_file(&writer, "gpu0", O_WRONLY);
	expect_error(error, 0);
	display_test_discovery(&opened, &reader, &writer);
	lease = display_test_ownership(&opened, &foreign, &reader);
	display_test_frames(&opened, &foreign, lease);

	/* Native lease release precedes ordinary resource and session cleanup. */
	assert(display_state.owner == NULL);
	assert(display_state.lease == 0U);
	close_file(&writer);
	close_file(&reader);
	close_file(&foreign);
	close_file(&opened);
	error = drv_gpu_unregister(device);
	expect_error(error, 0);
	assert(backend.live == 0U);
	assert(backend.opens == backend.closes);
	assert(allocations == 0U);
	assert(credential_references == 0U);
	assert(held_spinlocks == 0U);
	puts("GPU display: complete ops, access rights, normalized discovery, foreign leases, copyout rollback, packed bounds, completion and teardown PASS");

	/* Succeeded: no display lease, resource, cdev generation or session survived this fixture. */
	return 0;
}

/* Verifies that discovery receives no caller-controlled output fields. */
static int
display_test_query(
	void *opaque,
	void *session,
	struct gpu_display_info *request)
{
	struct gpu_display_info expected;
	int match;

	/* The real core must pass a fresh output object rather than echoing user-provided state. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	assert(session != NULL);
	memset(&expected, 0, sizeof(expected));
	expected.index = request->index;
	match = memcmp(request, &expected, sizeof(expected));
	assert(match == 0);
	display_state.queries++;
	request->count = 2U;

	/* Count-only queries deliberately leave every per-output field absent. */
	if (request->index == GPU_DISPLAY_COUNT_ONLY)
		return 0;

	/* Each valid ordinal receives a nonzero identity and the current topology generation. */
	if (request->index >= request->count)
		return EINVAL;
	request->display_id = request->index + 1U;
	request->generation = 7U;
	request->plane_count = 1U;
	request->flags = GPU_DISPLAY_CONNECTED | GPU_DISPLAY_FIFO | GPU_DISPLAY_VIRTUAL_CLOCK;
	request->formats = GPU_DISPLAY_FORMAT_RGBA8888;
	request->max_frame_bytes = 4096U;
	memset(request->name, 'Q', sizeof(request->name));
	if (display_state.malformed_query != 0U)
		request->display_id = 0U;

	/* Succeeded: the core must normalize the ABI header and terminate this full-length name. */
	return 0;
}

/* Checks that mode enumeration discards input geometry and validation keeps only its defined inputs. */
static int
display_test_mode(
	void *opaque,
	void *session,
	struct gpu_display_mode *request)
{
	/* Mode discovery does not acquire or transfer native display ownership. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	assert(session != NULL);
	assert(request->display_id == 1U);
	if (request->generation != 7U)
		return ESTALE;
	display_state.modes++;

	/* Enumeration supplies geometry as output, so poisoned input dimensions must already be zero. */
	if (request->operation == GPU_DISPLAY_MODE_ENUMERATE) {
		assert(request->width == 0U);
		assert(request->height == 0U);
		assert(request->refresh_millihz == 0U);
		request->count = 1U;
		request->width = 16U;
		request->height = 16U;
		request->refresh_millihz = 50000U;
	} else {
		assert(request->operation == GPU_DISPLAY_MODE_VALIDATE);
		assert(request->index == 0U);
		assert(request->width != 0U);
		assert(request->height != 0U);
		assert(request->refresh_millihz != 0U);
	}

	/* Succeeded: side-effect-free mode handling did not change the modeled lease owner. */
	return 0;
}

/* Acquires one exclusive native lease tied to its original open description. */
static int
display_test_claim(
	void *opaque,
	void *session,
	struct gpu_display_claim *request)
{
	/* All user-copy and spinlock ownership has ended before native arbitration begins. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	assert(request->lease == 0U);
	display_state.claims++;
	if (request->display_id != 1U || request->plane_index != 0U)
		return EINVAL;
	if (request->generation != 7U)
		return ESTALE;
	if (display_state.owner != NULL)
		return EBUSY;

	/* Lease identities are never reused across failed copyout and later successful claims. */
	display_state.owner = session;
	display_state.lease = display_state.next_lease++;
	display_state.completed = 0U;
	request->lease = display_state.lease;

	/* Succeeded: only this exact backend session may present or release the lease. */
	return 0;
}

/* Consumes only a matching lease owned by this exact backend session. */
static int
display_test_release(
	void *opaque,
	void *session,
	const struct gpu_display_release *request)
{
	/* A borrowed or stale lease cannot release another open's active display ownership. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	display_state.releases++;
	if (display_state.owner != session)
		return EACCES;
	if (request->lease != display_state.lease)
		return EINVAL;

	/* Clearing native ownership allows a later independent claim without retaining caller state. */
	display_state.owner = NULL;
	display_state.lease = 0U;
	display_state.completed = 0U;

	/* Succeeded: the consumed lease no longer authorizes any native display operation. */
	return 0;
}

/* Reads only the complete resource interval validated by the real GPU core. */
static int
display_test_present(
	void *opaque,
	void *session,
	void *object,
	struct gpu_display_present *request)
{
	struct test_resource *resource;
	uint64_t bytes;

	/* The backend receives its actual kernel object, never the caller's resource handle as a pointer. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	resource = object;
	assert(resource->session == session);
	assert(resource->kind == 1U);
	display_state.presents++;
	if (display_state.owner != session)
		return EACCES;
	if (request->lease != display_state.lease)
		return EINVAL;
	if (request->generation != 7U)
		return ESTALE;

	/* Packed-row arithmetic and full-image extent must have been checked before callback admission. */
	bytes = (uint64_t)request->stride * request->height;
	assert(request->width != 0U);
	assert(request->height != 0U);
	assert(request->stride >= request->width * 4U);
	assert(request->offset <= resource->bytes);
	assert(bytes <= resource->bytes - request->offset);
	assert(request->flags == GPU_DISPLAY_PRESENT_FIFO);
	assert(request->sequence == 0U);
	display_state.completed++;
	request->sequence = display_state.completed;

	/* Succeeded: the source is no longer read when the completed sequence reaches the caller. */
	return 0;
}

/* Observes the current completed frame only through the same open's active lease. */
static int
display_test_wait(
	void *opaque,
	void *session,
	struct gpu_display_wait *request)
{
	/* Output-only values cannot become trusted completion state without backend observation. */
	assert(held_spinlocks == 0U);
	assert(opaque == display_state.backend);
	assert(request->completed_sequence == 0U);
	assert(request->present_time_ns == 0U);
	assert(request->generation == 0U);
	display_state.waits++;
	if (display_state.owner != session)
		return EACCES;
	if (request->lease != display_state.lease)
		return EINVAL;
	if (request->sequence > display_state.completed)
		return EAGAIN;
	request->completed_sequence = display_state.completed;
	request->present_time_ns = display_state.completed * UINT64_C(20000000);
	request->generation = 7U;

	/* Succeeded: copied output is tied to the backend's actual completed presentation sequence. */
	return 0;
}

/* Tests read-only enumeration, ABI rejection and complete query normalization. */
static void
display_test_discovery(
	struct test_file *opened,
	struct test_file *reader,
	struct test_file *writer)
{
	struct gpu_display_info query;
	struct gpu_display_mode mode;
	unsigned before;
	int error;

	/* A write-only description cannot gain observation authority through a copied query. */
	error = cdev_file_ops.ioctl(&writer->file, GPU_DISPLAY_QUERY, 0U);
	expect_error(error, EACCES);

	/* Non-input discovery fields deliberately contain bytes which the core must discard. */
	memset(&query, 0xa5, sizeof(query));
	query.version = GPU_ABI_VERSION;
	query.size = sizeof(query);
	query.index = 0U;
	query.reserved = 0U;
	error = cdev_file_ops.ioctl(&reader->file, GPU_DISPLAY_QUERY, (uintptr_t)&query);
	expect_error(error, 0);
	assert(query.version == GPU_ABI_VERSION);
	assert(query.size == sizeof(query));
	assert(query.display_id == 1U && query.generation == 7U);
	assert(query.name[sizeof(query.name) - 1U] == 0);

	/* Reserved input and ABI size failures cannot invoke any discovery callback. */
	before = display_state.queries;
	query.reserved = 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_QUERY, (uintptr_t)&query);
	expect_error(error, EINVAL);
	query.reserved = 0U;
	query.size--;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_QUERY, (uintptr_t)&query);
	expect_error(error, EINVAL);
	assert(display_state.queries == before);
	query.size = sizeof(query);
	display_state.malformed_query = 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_QUERY, (uintptr_t)&query);
	expect_error(error, EIO);
	display_state.malformed_query = 0U;

	/* Count-only discovery supplies no per-output identity and remains a valid response. */
	query.index = GPU_DISPLAY_COUNT_ONLY;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_QUERY, (uintptr_t)&query);
	expect_error(error, 0);
	assert(query.count == 2U && query.display_id == 0U);

	/* Mode enumeration clears user geometry before asking the backend for output. */
	memset(&mode, 0, sizeof(mode));
	mode.version = GPU_ABI_VERSION;
	mode.size = sizeof(mode);
	mode.display_id = 1U;
	mode.generation = 7U;
	mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
	mode.width = UINT32_MAX;
	mode.height = UINT32_MAX;
	mode.refresh_millihz = UINT32_MAX;
	error = cdev_file_ops.ioctl(&reader->file, GPU_DISPLAY_MODE, (uintptr_t)&mode);
	expect_error(error, 0);
	assert(mode.width == 16U && mode.height == 16U);
	assert(display_state.owner == NULL);

	/* Validation uses defined geometry inputs but rejects unknown operation and output flags. */
	mode.count = 0U;
	mode.operation = GPU_DISPLAY_MODE_VALIDATE;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_MODE, (uintptr_t)&mode);
	expect_error(error, 0);
	before = display_state.modes;
	mode.operation = 99U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_MODE, (uintptr_t)&mode);
	expect_error(error, EINVAL);
	assert(display_state.modes == before);

	/* Succeeded: discovery changed no ownership and exposed no caller-supplied output state. */
	return;
}

/* Tests mutation rights, exclusive ownership and rollback of an unpublished lease. */
static uint64_t
display_test_ownership(
	struct test_file *opened,
	struct test_file *foreign,
	struct test_file *reader)
{
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	uint64_t lease;
	unsigned before;
	int error;

	/* Read-only mutation fails before copying arguments or entering any backend callback. */
	before = display_state.claims;
	error = cdev_file_ops.ioctl(&reader->file, GPU_DISPLAY_CLAIM, 0U);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&reader->file, GPU_DISPLAY_RELEASE, 0U);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&reader->file, GPU_DISPLAY_PRESENT, 0U);
	expect_error(error, EACCES);
	assert(display_state.claims == before);

	/* Failed publication must release the native lease that the caller never received. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = 1U;
	claim.generation = 7U;
	reject_copyout = 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_CLAIM, (uintptr_t)&claim);
	expect_error(error, EFAULT);
	assert(claim.lease == 0U);
	assert(display_state.owner == NULL);
	assert(display_state.releases == 1U);

	/* A later claim gets a new identity rather than reviving the unpublished lease. */
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_CLAIM, (uintptr_t)&claim);
	expect_error(error, 0);
	assert(claim.lease == 2U);
	lease = claim.lease;
	claim.lease = 0U;
	error = cdev_file_ops.ioctl(&foreign->file, GPU_DISPLAY_CLAIM, (uintptr_t)&claim);
	expect_error(error, EBUSY);

	/* Knowing another open's lease number never transfers its release authority. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = lease;
	error = cdev_file_ops.ioctl(&foreign->file, GPU_DISPLAY_RELEASE, (uintptr_t)&release);
	expect_error(error, EACCES);
	assert(display_state.lease == lease);

	/* Succeeded: the original open retains the sole live plane lease. */
	return lease;
}

/* Tests typed source ownership, complete pixel bounds and observable completed sequences. */
static void
display_test_frames(
	struct test_file *opened,
	struct test_file *foreign,
	uint64_t lease)
{
	struct gpu_resource_create storage;
	struct gpu_blob_create blob;
	struct gpu_display_present present;
	struct gpu_display_wait wait;
	struct gpu_display_release release;
	uint64_t storage_handle;
	unsigned before;
	int error;

	/* Both storage and blob objects are real session-owned allocations in the production GPU core. */
	prepare_create(&storage);
	error = cdev_file_ops.ioctl(&opened->file, GPU_RESOURCE_CREATE, (uintptr_t)&storage);
	expect_error(error, 0);
	storage_handle = storage.handle;
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = 4096U;
	blob.flags = GPU_BLOB_MAPPABLE;
	error = cdev_file_ops.ioctl(&opened->file, GPU_BLOB_CREATE, (uintptr_t)&blob);
	expect_error(error, 0);

	/* A complete image is valid only in ordinary storage owned by this exact open. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = lease;
	present.handle = storage_handle;
	present.frame = 101U;
	present.width = 16U;
	present.height = 16U;
	present.stride = 64U;
	present.format = GPU_PIXEL_RGBA8888;
	present.refresh_millihz = 50000U;
	present.flags = GPU_DISPLAY_PRESENT_FIFO;
	present.generation = 7U;
	before = display_state.presents;
	error = cdev_file_ops.ioctl(&foreign->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.handle = blob.handle;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	assert(display_state.presents == before);
	present.handle = storage_handle;

	/* Invalid row geometry, offset and output sequence never reach the native pixel reader. */
	present.width = UINT32_MAX;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.width = 16U;
	present.height = 65U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.height = 16U;
	present.stride = 63U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.stride = 64U;
	present.offset = 4095U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	present.offset = 0U;
	present.sequence = 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);
	assert(display_state.presents == before);

	/* A successfully completed image can be observed through its lease-scoped native sequence. */
	present.sequence = 0U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, 0);
	assert(present.sequence == 1U);
	memset(&wait, 0, sizeof(wait));
	wait.version = GPU_ABI_VERSION;
	wait.size = sizeof(wait);
	wait.lease = lease;
	wait.sequence = 1U;
	error = cdev_file_ops.ioctl(&foreign->file, GPU_DISPLAY_WAIT, (uintptr_t)&wait);
	expect_error(error, EACCES);
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_WAIT, (uintptr_t)&wait);
	expect_error(error, 0);
	assert(wait.completed_sequence == 1U);
	assert(wait.present_time_ns == 20000000U);
	assert(wait.generation == 7U);

	/* Reusing prior output fields is rejected before native completion observation. */
	before = display_state.waits;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_WAIT, (uintptr_t)&wait);
	expect_error(error, EINVAL);
	assert(display_state.waits == before);

	/* A present copyout fault cannot undo the real completed frame, which remains observable. */
	present.sequence = 0U;
	reject_copyout = 1U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EFAULT);
	wait.completed_sequence = 0U;
	wait.present_time_ns = 0U;
	wait.generation = 0U;
	wait.sequence = 2U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_WAIT, (uintptr_t)&wait);
	expect_error(error, 0);
	assert(wait.completed_sequence == 2U);

	/* Completed presentation retains no source reference, so an explicit resource destroy can succeed. */
	error = destroy_handle(opened, storage_handle);
	expect_error(error, 0);
	present.sequence = 0U;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_PRESENT, (uintptr_t)&present);
	expect_error(error, EINVAL);

	/* Releasing the plane consumes its lease independently of remaining ordinary GPU resources. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = lease;
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_RELEASE, (uintptr_t)&release);
	expect_error(error, 0);
	error = cdev_file_ops.ioctl(&opened->file, GPU_DISPLAY_RELEASE, (uintptr_t)&release);
	expect_error(error, EACCES);

	/* Succeeded: every invalid source remained outside native callbacks and every completed frame was observable. */
	return;
}
