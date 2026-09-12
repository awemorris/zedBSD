/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU registration, character-device sessions, and owned resource handles.
 */

#include <drivers/gpu.h>
#include <kern/cdev.h>
#include <kern/cred.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/uaccess.h>
#include <kern/page.h>
#include <kern/pmem.h>
#include <kern/vm-device.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GPU_DEVICE_BASE		0x00090000U
#define GPU_RESOURCE_STORAGE	1U
#define GPU_RESOURCE_BLOB	2U

/*
 * One registered GPU, retained by its owner and every cdev generation.
 *
 *  - The registry lock protects online/sessions.
 *  - The lifecycle gate protects the sorted device list and node publication.
 *  - Backend state stays borrowed until unregister succeeds, even when the
 *    node is already hidden.
 */
struct drv_gpu_device {
	const struct drv_gpu_ops *ops;
	void *private_data;
	struct cdev *node;
	struct drv_gpu_device *next;
	refcount_t references;
	unsigned number;
	unsigned online;
	unsigned sessions;
};

/*
 * One kernel snapshot of a selected display request; it retains no user pointers.
 */
union gpu_display_request {
	struct {
		uint32_t version;
		uint32_t size;
	} header;
	struct gpu_display_info query;
	struct gpu_display_mode mode;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_display_present present;
	struct gpu_display_wait wait;
};

/*
 * One dynamically allocated resource owned by its session until explicit release.
 */
struct gpu_resource {
	struct gpu_resource *next;
	uint64_t handle;
	uint64_t bytes;
	void *object;
	unsigned kind;
	volatile unsigned mapping_count;
	uint64_t mapping_offset;
	struct drv_gpu_mapping mapping;
};

/*
 * One open file description shared by dup/fork until its final close.
 *
 *  - Device session ownership keeps backend state alive.
 *  - Busy excludes simultaneous ioctls without holding a core lock across
 *    a backend callback.
 */
struct gpu_session {
	struct drv_gpu_device *device;
	void *backend;
	struct gpu_resource *resources;
	uint32_t resource_count;
	uint64_t next_mapping_offset;
	unsigned writable;
	unsigned readable;
	unsigned busy;
};

/*
 * Holds registered instances in ascending device-number order.
 *
 *  - The lifecycle gate protects linkage, withdrawn devices stay here while
 *    sessions retain backend ownership.
 *  - A successful unregister frees the ID.
 */
static struct drv_gpu_device *gpu_devices;

/*
 * Serializes registration and unregistration across callers and buses.
 * 
 * - Contending lifecycle callers receive EBUSY and may retry without spinning.
 */
static atomic_uint_t gpu_lifecycle;

/*
 * Assigns handles a globally unique generation, including across sessions.
 *
 * - The registry lock protects it, exhaustion refuses allocation with
 *   EOVERFLOW instead of wrapping into a previously issued generation.
 */
static uint64_t gpu_handle_generation;

/*
 * Protects session admission, offline transitions, and handle generations.
 *
 *  - Backend calls, cdev operations, allocations, and user copies run unlocked.
 */
static struct spinlock gpu_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "GPU registry", 0, 0
};

/*
 * Forward declaration.
 */
static int gpu_ops_validate(const struct drv_gpu_ops *ops);
static int gpu_publish_node(struct drv_gpu_device *device);
static void gpu_device_release(void *argument);
static int gpu_open(struct file *file);
static int gpu_close(struct file *file);
static int gpu_ioctl(struct file *file, unsigned long command, uintptr_t argument);
static int gpu_poll(struct file *file, short events, short *revents);
static void gpu_session_drop(struct gpu_session *session);
static int gpu_session_enter(struct gpu_session *session);
static void gpu_session_leave(struct gpu_session *session);
static int gpu_info_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_create_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_destroy_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_handle_allocate(uint64_t *handle);
static int gpu_resource_lookup(struct gpu_session *session, uint64_t handle, struct gpu_resource **result);
static int gpu_resource_reserve(struct gpu_session *session, uint64_t bytes, struct gpu_resource **result);
static int gpu_user_range(uint64_t address, uint32_t bytes, uintptr_t *pointer);
static int gpu_capset_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_blob_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_transfer_ioctl(struct gpu_session *session, uintptr_t argument, unsigned writing);
static int gpu_command_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_present_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_map_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_mmap(struct file *file, off_t offset, size_t bytes, uint32_t prot, struct vm_device_mapping **result);
static void gpu_mapping_release(void *owner);
static int gpu_display_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);

/*
 * Registers one initialized backend and publishes its character device.
 */
int
drv_gpu_register(
	const struct drv_gpu_ops *ops,
	void *private_data,
	struct drv_gpu_device **result)
{
	struct drv_gpu_device **position;
	struct drv_gpu_device *device;
	unsigned number;
	int acquired;
	int error;

	/* A successful registration must return an owner handle. */
	if (result == NULL)
		return EINVAL;

	/* Failed registration never transfers ownership to the caller. */
	*result = NULL;

	/* Rejects an incomplete dispatch contract before retaining backend data. */
	error = gpu_ops_validate(ops);
	if (error != 0)
		return error;

	/* Keeps numbering and publication atomic with respect to removal. */
	acquired = atomic_try_acquire_zero(&gpu_lifecycle);
	if (acquired == 0)
		return EBUSY;

	/* Reuses the lowest vacant number without a fixed-size device array. */
	number = 0;
	position = &gpu_devices;
	while (*position != NULL) {
		/* A gap in the sorted registry is available to this device. */
		device = *position;
		if (device->number != number)
			break;

		/* Device numbers must not wrap their encoded character-device ID. */
		if (number == UINT_MAX - GPU_DEVICE_BASE) {
			atomic_store_release(&gpu_lifecycle, 0);
			return EOVERFLOW;
		}

		/* Advances past an ID still owned by a live or withdrawing device. */
		number++;
		position = &device->next;
	}

	/* Allocates a wrapper that old inodes can retain after backend removal. */
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL) {
		atomic_store_release(&gpu_lifecycle, 0);
		return ENOMEM;
	}

	/* Initializes the complete backend view before any open can see it. */
	device->ops = ops;
	device->private_data = private_data;
	device->number = number;
	device->online = 1;
	refcount_init(&device->references, 1);

	/* Publishes immediately, independently of whether devfs is mounted. */
	error = gpu_publish_node(device);
	if (error != 0) {
		gpu_device_release(device);
		atomic_store_release(&gpu_lifecycle, 0);
		return error;
	}

	/* Keeps the registered instance until its owner completes unregister. */
	device->next = *position;
	*position = device;
	*result = device;
	atomic_store_release(&gpu_lifecycle, 0);

	/* Succeeded: the caller owns one registered GPU device. */
	return 0;
}

/*
 * Withdraws a device before releasing the owner's borrowed backend state.
 */
int
drv_gpu_unregister(
	struct drv_gpu_device *device)
{
	struct drv_gpu_device **position;
	struct cdev *node;
	unsigned sessions;
	unsigned long irq;
	int acquired;
	int error;

	/* Removal requires the handle returned by a successful registration. */
	if (device == NULL)
		return EINVAL;

	/* Keeps removal from racing publication or another removal attempt. */
	acquired = atomic_try_acquire_zero(&gpu_lifecycle);
	if (acquired == 0)
		return EBUSY;

	/* Finds the owned registration before inspecting its backend state. */
	position = &gpu_devices;
	while (*position != NULL) {
		/* Pointer identity selects the exact registered instance. */
		if (*position == device)
			break;

		/* Advances to the next independently owned device. */
		position = &(*position)->next;
	}

	/* A foreign handle has no registration that this core can release. */
	if (*position == NULL) {
		atomic_store_release(&gpu_lifecycle, 0);
		return ENOENT;
	}

	/* Stops new operations before removing the namespace entry. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->online = 0;
	sessions = device->sessions;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Old inode generations remain retained by the common cdev core. */
	node = device->node;
	if (node != NULL) {
		error = cdev_unregister(node);
		if (error != 0 && error != ENOENT) {
			atomic_store_release(&gpu_lifecycle, 0);
			return error;
		}

		/* Relinquishes the node reference after namespace withdrawal. */
		device->node = NULL;
		cdev_release(node);
	}

	/* Wakes retained files so poll can report device removal. */
	poll_notify();

	/* The owner must retain backend state until every close has finished. */
	if (sessions != 0) {
		atomic_store_release(&gpu_lifecycle, 0);
		return EBUSY;
	}

	/* Releases registration ownership and makes this number reusable. */
	*position = device->next;
	device->next = NULL;
	gpu_device_release(device);
	atomic_store_release(&gpu_lifecycle, 0);

	/* Succeeded: the owner may free its operations and private state. */
	return 0;
}

/* Validates callback pairs and capabilities before device registration. */
static int
gpu_ops_validate(
	const struct drv_gpu_ops *ops)
{
	/* A missing or incompatible table cannot be dispatched safely. */
	if (ops == NULL)
		return EINVAL;

	/* Rejects old or unrelated internal layouts before reading appended callbacks. */
	if (ops->version != DRV_GPU_INTERFACE_VERSION)
		return EOPNOTSUPP;

	/* Size and reserved fields prevent interpreting an unrelated layout. */
	if (ops->size != sizeof(*ops) || ops->reserved != 0)
		return EINVAL;

	/* Session creation and cleanup are an inseparable pair. */
	if (ops->open == NULL || ops->close == NULL)
		return EINVAL;

	/* Every backend must describe its storage limits and identity. */
	if (ops->get_info == NULL)
		return EINVAL;

	/* Rejects bits that have no defined framework operation. */
	if ((ops->capabilities & ~(GPU_CAP_RESOURCE | GPU_CAP_CAPSET |
	    GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_COMMAND |
	    GPU_CAP_PRESENT | GPU_CAP_MAPPING | GPU_CAP_DISPLAY)) != 0)
		return EOPNOTSUPP;

	/* Storage allocation must agree with its advertised capability. */
	if ((ops->capabilities & GPU_CAP_RESOURCE) != 0) {
		/* Storage support needs an allocator before any session can use it. */
		if (ops->resource_create == NULL)
			return EINVAL;
	} else {
		/* Hidden allocators would contradict the userspace capability reply. */
		if (ops->resource_create != NULL)
			return EINVAL;
	}

	/* Blob allocation is independently optional from ordinary storage. */
	if ((ops->capabilities & GPU_CAP_BLOB) != 0) {
		/* Blob support needs its protocol-aware allocator. */
		if (ops->blob_create == NULL)
			return EINVAL;
	} else {
		/* Unadvertised blob creation is not reachable through this contract. */
		if (ops->blob_create != NULL)
			return EINVAL;
	}

	/* Every resource type shares one infallible terminal cleanup operation. */
	if ((ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_BLOB)) != 0) {
		/* Every accepted allocation must have a matching final release. */
		if (ops->resource_destroy == NULL)
			return EINVAL;
	} else {
		/* A backend without resource allocation cannot own resource cleanup. */
		if (ops->resource_destroy != NULL)
			return EINVAL;
	}

	/* Capability queries must be present exactly when advertised. */
	if ((ops->capabilities & GPU_CAP_CAPSET) != 0) {
		/* Published capsets need a callback to fill the bounded response. */
		if (ops->get_capset == NULL)
			return EINVAL;
	} else {
		/* Suppresses an unreachable capability query operation. */
		if (ops->get_capset != NULL)
			return EINVAL;
	}

	/* Transfer support always provides both copy directions. */
	if ((ops->capabilities & GPU_CAP_TRANSFER) != 0) {
		/* A partial copy pair would leave the advertised transfer contract incomplete. */
		if (ops->resource_read == NULL || ops->resource_write == NULL)
			return EINVAL;
	} else {
		/* Unadvertised transfers cannot gain access to session resources. */
		if (ops->resource_read != NULL || ops->resource_write != NULL)
			return EINVAL;
	}

	/* Command streams are independently optional from ordinary storage. */
	if ((ops->capabilities & GPU_CAP_COMMAND) != 0) {
		/* Advertised command receipt requires a backend dispatcher. */
		if (ops->command == NULL)
			return EINVAL;
	} else {
		/* Hidden command submission would bypass capability negotiation. */
		if (ops->command != NULL)
			return EINVAL;
	}

	/* Presentation requires an explicit backend ownership operation. */
	if ((ops->capabilities & GPU_CAP_PRESENT) != 0) {
		/* A presentation callback arbitrates scanout ownership across sessions. */
		if (ops->present == NULL)
			return EINVAL;
	} else {
		/* No presentation callback is retained for a non-display backend. */
		if (ops->present != NULL)
			return EINVAL;
	}

	/* CPU mappings require a resource allocator and a complete stable view. */
	if ((ops->capabilities & GPU_CAP_MAPPING) != 0) {
		/* A CPU view requires owned resource storage for its entire lifetime. */
		if (ops->resource_map == NULL ||
		    (ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_BLOB)) == 0)
			return EINVAL;
	} else if (ops->resource_map != NULL) {
		return EINVAL;
	}

	/* Display discovery, ownership and presentation form one complete contract. */
	if ((ops->capabilities & GPU_CAP_DISPLAY) != 0) {
		/* Advertising display support requires the complete optional operation table. */
		if (ops->display == NULL)
			return EINVAL;

		/* Discovery, lease ownership and completion must all remain available together. */
		if (ops->display->query == NULL ||
		    ops->display->mode == NULL ||
		    ops->display->claim == NULL ||
		    ops->display->release == NULL ||
		    ops->display->present == NULL ||
		    ops->display->wait == NULL)
			return EINVAL;

		/* Whole-frame presentation consumes storage allocated by this same backend. */
		if ((ops->capabilities & GPU_CAP_RESOURCE) == 0)
			return EINVAL;
	} else if (ops->display != NULL) {
		return EINVAL;
	}

	/* Succeeded: every advertised operation has its required lifecycle. */
	return 0;
}

/* Publishes a generation whose finalizer retains the GPU wrapper. */
static int
gpu_publish_node(
	struct drv_gpu_device *device)
{
	/* Immutable dispatch shared by every published cdev generation. */
	static const struct cdev_ops gpu_file_operations = {
		gpu_open,
		gpu_close,
		NULL,
		NULL,
		gpu_ioctl,
		gpu_poll,
		gpu_mmap
	};
	struct cdev *node;
	char name[32];
	int error;

	/* Formats a bounded name from a framework-controlled slot number. */
	snprintf(name, sizeof(name), "gpu%u", device->number);

	/* A cdev generation keeps its wrapper alive even after backend removal. */
	refcount_get(&device->references);
	error = cdev_register_managed(name,
	    (dev_t)(GPU_DEVICE_BASE + device->number),
	    &gpu_file_operations,
	    device,
	    gpu_device_release,
	    &node);
	if (error != 0) {
		gpu_device_release(device);
		return error;
	}

	/* Retains the caller reference for the matching unregister transaction. */
	device->node = node;

	/* Succeeded: the namespace owns an independent cdev generation. */
	return 0;
}

/* Frees the wrapper after registration and stale inode ownership end. */
static void
gpu_device_release(
	void *argument)
{
	struct drv_gpu_device *device;
	int last;

	/* Releases one wrapper reference without touching borrowed backend data. */
	device = argument;
	last = refcount_put(&device->references);
	if (last != 0)
		kern_free(device);

	/* Succeeded: this reference no longer retains the GPU wrapper. */
	return;
}

/* Captures an authorized open and keeps its backend alive until close. */
static int
gpu_open(
	struct file *file)
{
	struct drv_gpu_device *device;
	struct gpu_session *session;
	struct ucred *credential;
	unsigned flags;
	unsigned long irq;
	int root;
	int error;

	/* Rejects absent credentials as well as non-root users in ABI version 1. */
	device = file->f_data;
	file->f_data = NULL;
	credential = cred_current_ref();
	if (credential == NULL)
		return EACCES;

	/* Copies authority rather than borrowing mutable process credentials. */
	root = cred_is_superuser(credential);
	cred_release(credential);
	if (root == 0)
		return EACCES;

	/* Allocates the open description before claiming backend ownership. */
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* Captures write authority from the original open mode. */
	flags = file_status_flags_get(file);
	session->device = device;
	if ((flags & O_ACCMODE) != O_RDONLY)
		session->writable = 1;

	/* Readback obeys the original descriptor's independent read authority. */
	if ((flags & O_ACCMODE) != O_WRONLY)
		session->readable = 1;

	/* Session ownership excludes hardware detach, including during open. */
	error = 0;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (device->online == 0) {
		error = ENODEV;
	} else if (device->sessions == UINT_MAX) {
		error = EOVERFLOW;
	} else {
		/* The count remains held until backend cleanup has returned. */
		device->sessions++;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* A failed admission has never entered the backend. */
	if (error != 0) {
		kern_free(session);
		return error;
	}

	/* Opens backend state without any core lock held. */
	error = device->ops->open(device->private_data, &session->backend);
	if (error != 0) {
		gpu_session_drop(session);
		return error;
	}

	/* Checks whether a removal raced with backend initialization. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	error = 0;
	if (device->online == 0)
		error = ENODEV;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Completes backend cleanup before allowing the withdrawing owner to retry. */
	if (error != 0) {
		device->ops->close(device->private_data, session->backend);
		gpu_session_drop(session);
		return error;
	}

	/* Publishes this session only after backend open has succeeded. */
	file->f_data = session;

	/* Succeeded: the file description owns one backend session. */
	return 0;
}

/* Releases resources before the session's backend context disappears. */
static int
gpu_close(
	struct file *file)
{
	struct gpu_session *session;
	struct drv_gpu_device *device;
	struct gpu_resource *resource;

	/* An unsuccessful open leaves no session for final-close cleanup. */
	session = file->f_data;
	if (session == NULL)
		return 0;

	/* File reference ownership excludes concurrent ioctls at final close. */
	file->f_data = NULL;
	device = session->device;

	/* Destroys all owned objects while their backend session remains valid. */
	while (session->resources != NULL) {
		resource = session->resources;
		session->resources = resource->next;
		device->ops->resource_destroy(
			device->private_data,
			session->backend,
			resource->object);
		kern_free(resource);

		/* Final close consumes the session count after each retained object retires. */
		session->resource_count--;
	}

	/* Finishes backend cleanup before releasing the device removal barrier. */
	device->ops->close(device->private_data, session->backend);
	gpu_session_drop(session);

	/* Succeeded: no resource is retained by this open description. */
	return 0;
}

/* Drops a completed or failed session without accessing backend state again. */
static void
gpu_session_drop(
	struct gpu_session *session)
{
	struct drv_gpu_device *device;
	unsigned long irq;

	/* Frees per-open memory before allowing the final hardware teardown. */
	device = session->device;
	kern_free(session);

	/* Zero tells the owner that every backend close callback has finished. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->sessions--;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: this session no longer prevents backend removal. */
	return;
}

/* Reserves one ioctl without holding a lock through driver execution. */
static int
gpu_session_enter(
	struct gpu_session *session)
{
	unsigned long irq;
	int error;

	/* Refuses operations after withdrawal and serializes a shared fd. */
	error = 0;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (session->device->online == 0) {
		error = ENODEV;
	} else if (session->busy != 0) {
		error = EBUSY;
	} else {
		/* Busy protects this session's handle table through the user copy. */
		session->busy = 1;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Reports a removal or another ioctl retaining the session. */
	if (error != 0)
		return error;

	/* Succeeded: this operation exclusively owns the session handle table. */
	return 0;
}

/* Releases the ioctl reservation after all backend and user-copy work. */
static void
gpu_session_leave(
	struct gpu_session *session)
{
	unsigned long irq;

	/* Makes the handle table available to the next operation on this fd. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	session->busy = 0;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: a later ioctl may enter this session. */
	return;
}

/* Dispatches only fixed-size, versioned requests into the GPU core. */
static int
gpu_ioctl(
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	struct gpu_session *session;
	int error;

	/* A failed open cannot supply backend authority. */
	session = file->f_data;
	if (session == NULL)
		return ENODEV;

	/* Holds the session table across validation, callbacks, and copyout. */
	error = gpu_session_enter(session);
	if (error != 0)
		return error;

	/* Every command owns its input checks and failure cleanup. */
	switch (command) {
	case GPU_GET_INFO:
		/* Returns the backend identity and supported framework limits. */
		error = gpu_info_ioctl(session, argument);
		break;
	case GPU_RESOURCE_CREATE:
		/* Allocates storage owned by this open description. */
		error = gpu_create_ioctl(session, argument);
		break;
	case GPU_RESOURCE_DESTROY:
		/* Releases a live resource belonging to this session. */
		error = gpu_destroy_ioctl(session, argument);
		break;
	case GPU_GET_CAPSET:
		/* Returns a bounded backend protocol description. */
		error = gpu_capset_ioctl(session, argument);
		break;
	case GPU_BLOB_CREATE:
		/* Allocates a protocol blob owned by this open description. */
		error = gpu_blob_ioctl(session, argument);
		break;
	case GPU_RESOURCE_READ:
		/* Copies bytes out of a retained resource after checking read authority. */
		error = gpu_transfer_ioctl(session, argument, 0);
		break;
	case GPU_RESOURCE_WRITE:
		/* Copies validated userspace bytes into a retained resource. */
		error = gpu_transfer_ioctl(session, argument, 1);
		break;
	case GPU_COMMAND:
		/* Submits an independent kernel copy of the backend command stream. */
		error = gpu_command_ioctl(session, argument);
		break;
	case GPU_RESOURCE_MAP:
		/* Returns a session-private mmap token without exposing physical addresses. */
		error = gpu_map_ioctl(session, argument);
		break;
	case GPU_DISPLAY_QUERY:
	case GPU_DISPLAY_MODE:
	case GPU_DISPLAY_CLAIM:
	case GPU_DISPLAY_RELEASE:
	case GPU_DISPLAY_PRESENT:
	case GPU_DISPLAY_WAIT:
		/* Native display requests share validation and access admission. */
		error = gpu_display_ioctl(session, command, argument);
		break;
	case GPU_PRESENT:
		/* Presents only a complete bounded image in ordinary owned storage. */
		error = gpu_present_ioctl(session, argument);
		break;
	default:
		/* No backend callback receives an unknown request. */
		error = EOPNOTSUPP;
		break;
	}

	/* Frees dispatch ownership on both success and failure. */
	gpu_session_leave(session);

	/* Reports malformed input, unsupported operations, or backend errors. */
	if (error != 0)
		return error;

	/* Succeeded: the requested framework operation is complete. */
	return 0;
}

/* Reports removal without pretending that rendering events are implemented. */
static int
gpu_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct gpu_session *session;
	unsigned long irq;

	/* Removal is reported regardless of the caller's requested event mask. */
	(void)events;

	/* A poll result must have storage owned by the VFS caller. */
	if (revents == NULL)
		return EINVAL;

	/* Samples online state while the file keeps its session alive. */
	*revents = 0;
	session = file->f_data;
	if (session == NULL) {
		*revents = POLLERR | POLLHUP;
		return 0;
	}

	/* Removed GPUs wake their retained descriptors with an error and hangup. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (session->device->online == 0)
		*revents = POLLERR | POLLHUP;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: only removal readiness is exposed by this framework. */
	return 0;
}

/* Returns backend identity with capabilities constrained by the core ABI. */
static int
gpu_info_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_info request;
	struct gpu_info information;
	struct drv_gpu_device *device;
	int error;

	/* Copies the fixed header before selecting its interpretation. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses incompatible layouts without invoking any backend operation. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* Zeroes the complete reply so unwritten fields never expose memory. */
	memset(&information, 0, sizeof(information));
	device = session->device;
	error = device->ops->get_info(device->private_data,
				      session->backend,
				      &information);
	if (error != 0)
		return error;

	/* The core, rather than the backend, declares this ioctl contract. */
	information.version = GPU_ABI_VERSION;
	information.size = sizeof(information);
	information.capabilities = device->ops->capabilities;
	information.driver_name[sizeof(information.driver_name) - 1U] = '\0';

	/* Returns the initialized snapshot to the requesting process. */
	error = copyout(&information, argument, sizeof(information));
	if (error != 0)
		return error;

	/* Succeeded: userspace has the capability snapshot. */
	return 0;
}

/* Creates an owned backend resource and rolls back an unreturned handle. */
static int
gpu_create_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_resource_create request;
	struct drv_gpu_device *device;
	void *object;
	struct gpu_resource *resource;
	int error;

	/* The original open mode, not later credential changes, grants mutation. */
	if (session->writable == 0)
		return EACCES;

	/* Checks capability before dereferencing optional callbacks. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_RESOURCE) == 0)
		return EOPNOTSUPP;

	/* Copies a fixed request with no embedded userspace pointers. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects unknown layouts before interpreting allocation inputs. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* Only storage resources with no optional flags exist in this ABI. */
	if (request.usage != GPU_RESOURCE_USAGE_STORAGE || request.flags != 0)
		return EINVAL;

	/* A new object cannot carry an existing or fabricated handle. */
	if (request.bytes == 0 || request.handle != 0)
		return EINVAL;

	/* Reserves bounded session ownership before any backend allocation. */
	error = gpu_resource_reserve(session, request.bytes, &resource);
	if (error != 0)
		return error;

	/* Carries the reserved public identity through the backend allocation transaction. */
	request.handle = resource->handle;

	/* Backend allocation receives a kernel copy of the validated request. */
	object = NULL;
	error = device->ops->resource_create(
		device->private_data,
		session->backend,
		&request,
		&object);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* A successful callback must supply an object that can be destroyed. */
	if (object == NULL) {
		kern_free(resource);
		return EIO;
	}

	/* Publishes the handle to the caller before committing the table entry. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		device->ops->resource_destroy(
			device->private_data,
			session->backend,
			object);
		kern_free(resource);
		return error;
	}

	/* Session admission excludes other lookups while ownership is published. */
	resource->object = object;
	resource->kind = GPU_RESOURCE_STORAGE;
	resource->next = session->resources;
	session->resources = resource;

	/* Published ownership now counts against the backend's per-session capacity. */
	session->resource_count++;

	/* Succeeded: the caller owns one typed resource handle in this session. */
	return 0;
}

/* Destroys only a matching live handle from this open session. */
static int
gpu_destroy_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_resource_destroy request;
	struct drv_gpu_device *device;
	struct gpu_resource *resource;
	struct gpu_resource **link;
	int error;
	unsigned mapping_count;

	/* A read-only description cannot mutate resources through ioctl. */
	if (session->writable == 0)
		return EACCES;

	/* Keeps optional cleanup callbacks paired with their advertised allocator. */
	device = session->device;
	if ((device->ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_BLOB)) == 0)
		return EOPNOTSUPP;

	/* Copies the versioned release request without user pointer retention. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects incompatible request layouts before handle lookup. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* Resolves the exact generation without accepting a foreign session handle. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* Regions and transient pins retain storage independently of this ioctl. */
	mapping_count = atomic_raw_load_acquire(&resource->mapping_count);
	if (mapping_count != 0)
		return EBUSY;

	/* Session admission preserves the resolved resource and its list link. */
	link = &session->resources;
	while (*link != resource)
		link = &(*link)->next;

	/* Completes backend cleanup before relinquishing session ownership. */
	device->ops->resource_destroy(
		device->private_data,
		session->backend,
		resource->object);
	*link = resource->next;

	/* The destroyed object no longer consumes the backend's per-session capacity. */
	session->resource_count--;
	kern_free(resource);

	/* Succeeded: the released handle can never identify a future object. */
	return 0;
}

/* Assigns an opaque resource identity that cannot wrap or alias. */
static int
gpu_handle_allocate(
	uint64_t *handle)
{
	unsigned long irq;
	int error;

	/* Allocates one generation independently of session and device reuse. */
	error = 0;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (gpu_handle_generation == UINT64_MAX) {
		error = EOVERFLOW;
	} else {
		/* Zero remains invalid; every attempted allocation consumes a stamp. */
		gpu_handle_generation++;
		*handle = gpu_handle_generation;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Refuses exhaustion instead of reviving a stale handle. */
	if (error != 0)
		return error;

	/* Succeeded: the stamp is unique for the lifetime of the kernel. */
	return 0;
}

/* Resolves an exact identity only within its owning open session. */
static int
gpu_resource_lookup(
	struct gpu_session *session,
	uint64_t handle,
	struct gpu_resource **result)
{
	struct gpu_resource *resource;

	/* Zero never identifies a successfully allocated resource. */
	if (handle == 0)
		return EINVAL;

	/* Searches only this open's ownership list for the globally unique handle. */
	resource = session->resources;
	for (;
	     resource != NULL;
	     resource = resource->next) {
		/* A matching identity belongs to this admitted session until the ioctl finishes. */
		if (resource->handle == handle)
			break;
	}

	/* A stale or foreign identity cannot reach any backend callback. */
	if (resource == NULL)
		return EINVAL;

	/* Transfers a borrowed object whose ownership is protected by session admission. */
	*result = resource;

	/* Succeeded: only this open can use the resolved backend resource. */
	return 0;
}

/* Reserves dynamic ownership after checking this backend's reported limits. */
static int
gpu_resource_reserve(
	struct gpu_session *session,
	uint64_t bytes,
	struct gpu_resource **result)
{
	struct drv_gpu_device *device;
	struct gpu_info information;
	struct gpu_resource *resource;
	int error;

	/* Queries the instance's capacity without holding a framework spinlock. */
	device = session->device;
	memset(&information, 0, sizeof(information));
	error = device->ops->get_info(
		device->private_data,
		session->backend,
		&information);
	if (error != 0)
		return error;

	/* Empty resources and sizes above the reported byte limit are invalid. */
	if (bytes == 0 || bytes > information.max_resource_bytes)
		return EINVAL;

	/* Backend capacity is independent of the number of registered GPU devices. */
	if (session->resource_count >= information.max_resources)
		return ENOSPC;

	/* Reserves metadata before acquiring a backend object that needs cleanup. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* Even a later failure consumes this non-reusable handle identity. */
	error = gpu_handle_allocate(&resource->handle);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* The caller publishes ownership only after backend and copyout succeed. */
	resource->bytes = bytes;
	*result = resource;

	/* Succeeded: failure paths now own one metadata record to release. */
	return 0;
}

/* Validates an encoded userspace pointer and its complete copy range. */
static int
gpu_user_range(
	uint64_t address,
	uint32_t bytes,
	uintptr_t *pointer)
{
	uintptr_t narrowed;

	/* Neither an empty transfer nor address zero identifies a copy buffer. */
	if (address == 0 || bytes == 0)
		return EFAULT;

	/* Refuses truncation when a wider userspace field targets a 32-bit kernel. */
	narrowed = (uintptr_t)address;
	if ((uint64_t)narrowed != address)
		return EFAULT;

	/* The last copied byte must remain representable in the native address type. */
	if ((uintptr_t)(bytes - 1U) > UINTPTR_MAX - narrowed)
		return EFAULT;

	/* Leaves actual userspace mapping and permission checks to copyin/copyout. */
	*pointer = narrowed;

	/* Succeeded: narrowing and range arithmetic preserve the original address. */
	return 0;
}

/* Copies a bounded backend capability set through an initialized inline reply. */
static int
gpu_capset_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct drv_gpu_device *device;
	struct gpu_capset request;
	struct gpu_capset response;
	int error;

	/* An absent optional capability cannot dispatch a callback. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_CAPSET) == 0)
		return EOPNOTSUPP;

	/* Copies the complete fixed layout before reading its payload bounds. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Unknown layouts cannot select backend-specific capabilities. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* The caller supplies capacity and leaves the actual byte count to the driver. */
	if (request.capacity == 0 || request.capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* An output byte count cannot carry a prior response into this query. */
	if (request.bytes != 0)
		return EINVAL;

	/* Zeroes unused payload bytes before the backend fills its capability data. */
	memset(&response, 0, sizeof(response));
	response.version = GPU_ABI_VERSION;
	response.size = sizeof(response);
	response.capset_id = request.capset_id;
	response.capset_version = request.capset_version;
	response.capacity = request.capacity;
	error = device->ops->get_capset(
		device->private_data,
		session->backend,
		&response);
	if (error != 0)
		return error;

	/* A backend cannot expand the caller's capacity or the inline buffer. */
	if (response.bytes > request.capacity)
		return EIO;

	/* Keeps protocol selectors and framework headers owned by the validated request. */
	response.version = GPU_ABI_VERSION;
	response.size = sizeof(response);
	response.capset_id = request.capset_id;
	response.capset_version = request.capset_version;
	response.capacity = request.capacity;

	/* Publishes only initialized bytes after callback success. */
	error = copyout(&response, argument, sizeof(response));
	if (error != 0)
		return error;

	/* Succeeded: the caller receives one bounded backend capability payload. */
	return 0;
}

/* Creates a session-owned protocol blob with rollback on failed handle copyout. */
static int
gpu_blob_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct drv_gpu_device *device;
	struct gpu_blob_create request;
	void *object;
	uint32_t resource_id;
	struct gpu_resource *resource;
	int error;

	/* Creation obeys authority captured when this description was opened. */
	if (session->writable == 0)
		return EACCES;

	/* A storage-only driver does not receive a blob request. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_BLOB) == 0)
		return EOPNOTSUPP;

	/* Copies the fixed request independently of any mutable caller buffer. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses unknown layouts before reading blob allocation parameters. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* Mapping is the only supported flag in the first blob contract. */
	if ((request.flags & ~GPU_BLOB_MAPPABLE) != 0)
		return EINVAL;

	/* Allocation outputs must not contain stale handles or backend identities. */
	if (request.handle != 0 || request.resource_id != 0)
		return EINVAL;

	/* Reserves bounded ownership before asking the driver to create a blob. */
	error = gpu_resource_reserve(session, request.bytes, &resource);
	if (error != 0)
		return error;

	/* Carries the reserved public identity through the backend allocation transaction. */
	request.handle = resource->handle;

	/* Receives only backend-owned identity, never a kernel or userspace pointer. */
	object = NULL;
	resource_id = 0;
	error = device->ops->blob_create(
		device->private_data,
		session->backend,
		&request,
		&object,
		&resource_id);
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* A successful allocation must return an object that can be released. */
	if (object == NULL) {
		kern_free(resource);
		return EIO;
	}

	/* A missing protocol identity cannot produce a usable userspace blob. */
	if (resource_id == 0) {
		device->ops->resource_destroy(device->private_data, session->backend, object);
		kern_free(resource);
		return EIO;
	}

	/* Rolls the allocation back when userspace cannot receive its ownership token. */
	request.resource_id = resource_id;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		device->ops->resource_destroy(device->private_data, session->backend, object);
		kern_free(resource);
		return error;
	}

	/* Publishes the type and extent while session admission excludes lookups. */
	resource->object = object;
	resource->kind = GPU_RESOURCE_BLOB;
	resource->next = session->resources;
	session->resources = resource;

	/* Published ownership now counts against the backend's per-session capacity. */
	session->resource_count++;

	/* Succeeded: this description owns the blob and its protocol resource identity. */
	return 0;
}

/* Copies one bounded resource range without exposing userspace pointers to drivers. */
static int
gpu_transfer_ioctl(
	struct gpu_session *session,
	uintptr_t argument,
	unsigned writing)
{
	struct drv_gpu_device *device;
	struct gpu_transfer request;
	struct gpu_resource *resource;
	uintptr_t pointer;
	void *buffer;
	int error;

	/* Enforces the original descriptor mode independently for each copy direction. */
	if (writing != 0) {
		/* A read-only description cannot change backend resource contents. */
		if (session->writable == 0)
			return EACCES;
	} else {
		/* A write-only description cannot disclose resource contents. */
		if (session->readable == 0)
			return EACCES;
	}

	/* Both copy callbacks were validated together at registration. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_TRANSFER) == 0)
		return EOPNOTSUPP;

	/* Copies the fixed descriptor before interpreting its encoded pointer. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Unknown layouts and reserved fields cannot reach a backend callback. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* No transfer modifiers are defined by the current ABI. */
	if (request.reserved != 0)
		return EINVAL;

	/* Bounds allocation and user-copy work for one ioctl independently of resource size. */
	if (request.bytes == 0 || request.bytes > GPU_COPY_MAX)
		return EINVAL;

	/* Resolves only a live allocation belonging to this open description. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* An offset past the allocation cannot be subtracted without underflow. */
	if (request.offset > resource->bytes)
		return EINVAL;

	/* The complete requested copy must fit the immutable allocated extent. */
	if (request.bytes > resource->bytes - request.offset)
		return EINVAL;

	/* Rejects encoded pointer truncation and wrap before allocating copy storage. */
	error = gpu_user_range(request.address, request.bytes, &pointer);
	if (error != 0)
		return error;

	/* A zeroed staging buffer also prevents a partial backend fill leaking memory. */
	buffer = kern_calloc(1, request.bytes);
	if (buffer == NULL)
		return ENOMEM;

	/* Completes the user copy before writes or after successful backend reads. */
	if (writing != 0) {
		/* Takes a private input snapshot before any backend state is changed. */
		error = copyin(pointer, buffer, request.bytes);
		if (error != 0) {
			kern_free(buffer);
			return error;
		}

		/* Dispatches only a validated object, offset and kernel-owned buffer. */
		error = device->ops->resource_write(
			device->private_data,
			session->backend,
			resource->object,
			request.offset,
			buffer,
			request.bytes);
		if (error != 0) {
			kern_free(buffer);
			return error;
		}
	} else {
		/* Obtains the complete resource range before exposing it to userspace. */
		error = device->ops->resource_read(
			device->private_data,
			session->backend,
			resource->object,
			request.offset,
			buffer,
			request.bytes);
		if (error != 0) {
			kern_free(buffer);
			return error;
		}

		/* Copies backend bytes out without transferring ownership of staging memory. */
		error = copyout(buffer, pointer, request.bytes);
		if (error != 0) {
			kern_free(buffer);
			return error;
		}
	}

	/* The staging allocation never survives synchronous dispatch. */
	kern_free(buffer);

	/* Succeeded: exactly the requested bounded range has been transferred. */
	return 0;
}

/* Submits one copied command stream while leaving completion to its protocol. */
static int
gpu_command_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct drv_gpu_device *device;
	struct gpu_command request;
	uintptr_t pointer;
	void *buffer;
	int error;

	/* Command submission can change GPU state and needs original write authority. */
	if (session->writable == 0)
		return EACCES;

	/* Backends without a command protocol cannot accept arbitrary streams. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_COMMAND) == 0)
		return EOPNOTSUPP;

	/* Copies and validates the descriptor before accessing command bytes. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Requires the fixed ABI layout and its currently empty flag set. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* No submission modifiers may be smuggled into the backend protocol. */
	if (request.flags != 0)
		return EINVAL;

	/* Bounds one copied stream and preserves its 32-bit protocol-word alignment. */
	if (request.bytes == 0 || request.bytes > GPU_COMMAND_MAX)
		return EINVAL;

	/* An incomplete word cannot be dispatched as a command stream. */
	if ((request.bytes & 3U) != 0)
		return EINVAL;

	/* Refuses encoded-pointer truncation or range wrap before user access. */
	error = gpu_user_range(request.address, request.bytes, &pointer);
	if (error != 0)
		return error;

	/* Allocates one bounded kernel snapshot of the command payload. */
	buffer = kern_malloc(request.bytes);
	if (buffer == NULL)
		return ENOMEM;

	/* A failed input copy never reaches a backend callback. */
	error = copyin(pointer, buffer, request.bytes);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* The backend consumes the private snapshot before its storage is released. */
	error = device->ops->command(
		device->private_data,
		session->backend,
		buffer,
		request.bytes);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/* Releases the copied stream after synchronous backend receipt finishes. */
	kern_free(buffer);

	/* Succeeded: the backend received the stream, not necessarily its execution result. */
	return 0;
}

/* Validates a complete packed-pixel image before backend display arbitration. */
static int
gpu_present_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct drv_gpu_device *device;
	struct gpu_present request;
	struct gpu_resource *resource;
	uint64_t image_bytes;
	uint32_t row_bytes;
	int error;

	/* Presentation changes shared display state and requires write authority. */
	if (session->writable == 0)
		return EACCES;

	/* Display ownership remains an optional backend responsibility. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_PRESENT) == 0)
		return EOPNOTSUPP;

	/* Copies the request before interpreting image dimensions or resource identity. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects unknown layouts before resolving any backend object. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* The initial display contract limits both dimensions before multiplication. */
	if (request.width == 0 || request.width > 4096U)
		return EINVAL;

	/* Empty or oversized rows cannot name a supported image. */
	if (request.height == 0 || request.height > 4096U)
		return EINVAL;

	/* Only the two defined packed four-byte pixel formats may be presented. */
	if (request.format != GPU_PIXEL_BGRA8888 &&
	    request.format != GPU_PIXEL_RGBA8888)
		return EINVAL;

	/* A complete row must fit its stride without overlapping the next row. */
	row_bytes = request.width * 4U;
	if (request.stride < row_bytes)
		return EINVAL;

	/* Resolves exact session ownership before backend display access. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* Host blobs must first be copied into ordinary scanout storage. */
	if (resource->kind != GPU_RESOURCE_STORAGE)
		return EINVAL;

	/* An out-of-range starting offset cannot be used in a remaining-size check. */
	if (request.offset > resource->bytes)
		return EINVAL;

	/* Widening both factors covers every complete displayed row without overflow. */
	image_bytes = (uint64_t)request.stride * request.height;
	if (image_bytes > resource->bytes - request.offset)
		return EINVAL;

	/* The driver arbitrates display ownership using only the validated resource. */
	error = device->ops->present(
		device->private_data,
		session->backend,
		resource->object,
		&request);
	if (error != 0)
		return error;

	/* Succeeded: the backend accepted this complete image for presentation. */
	return 0;
}

/* Issues a never-reused offset token for one immutable resource mapping. */
static int
gpu_map_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_resource_map request;
	struct gpu_resource *resource;
	struct drv_gpu_device *device;
	struct drv_gpu_mapping view;
	uint64_t offset;
	int error;

	/* Mapping discovery requires read authority and advertised support. */
	if (!session->readable)
		return EACCES;

	/* Unsupported backends cannot expose an opaque token without a stable mapping. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_MAPPING) == 0)
		return EOPNOTSUPP;

	/* Copies the complete request before interpreting any user-supplied field. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Output-only mapping fields must start empty in the selected ABI version. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.offset != 0 ||
	    request.bytes != 0)
		return EINVAL;

	/* A mapping query may borrow only storage owned by this admitted session. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* The first query validates and caches the resource's lifetime-stable view. */
	if (resource->mapping_offset == 0) {
		memset(&view, 0, sizeof(view));
		error = device->ops->resource_map(
			device->private_data,
			session->backend,
			resource->object,
			&view);
		if (error != 0)
			return error;

		/* A backend view must cover the resource and use only known storage attributes. */
		if (view.address == NULL ||
		    view.bytes == 0 ||
		    view.bytes < resource->bytes ||
		    view.bytes > SIZE_MAX ||
		    (view.attributes & ~DRV_GPU_MAPPING_DEVICE) != 0)
			return EIO;

		/* Every alias and extent must represent whole pages for the VM insertion. */
		if ((view.physical & (KERN_PAGE_SIZE - 1U)) != 0 ||
		    ((uintptr_t)view.address & (KERN_PAGE_SIZE - 1U)) != 0 ||
		    (view.bytes & (KERN_PAGE_SIZE - 1U)) != 0)
			return EIO;

		/* Reject truncated or wrapped aliases before caching a reusable mapping token. */
		if (view.physical > UINTPTR_MAX ||
		    view.bytes - 1U > UINTPTR_MAX - view.physical ||
		    view.bytes - 1U > UINTPTR_MAX - (uintptr_t)view.address)
			return EOVERFLOW;

		/* Keep every token within the signed mmap offset domain, never reusing it. */
		offset = session->next_mapping_offset;
		if (offset == 0)
			offset = KERN_PAGE_SIZE;

		/* The complete token extent must remain representable by signed mmap offsets. */
		if (offset > INT64_MAX || view.bytes > INT64_MAX - offset)
			return EOVERFLOW;

		/* Advancing this session watermark prevents stale tokens from naming future resources. */
		resource->mapping = view;
		resource->mapping_offset = offset;
		session->next_mapping_offset = offset + view.bytes;
	}

	/* A failed copyout can be retried with the same live resource handle. */
	request.offset = resource->mapping_offset;
	request.bytes = resource->mapping.bytes;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the caller received a complete initialized response for its owned object. */
	return 0;
}

/* Resolves one session-owned token into a retained, shared VM backing. */
static int
gpu_mmap(
	struct file *file,
	off_t offset,
	size_t bytes,
	uint32_t prot,
	struct vm_device_mapping **result)
{
	struct gpu_session *session;
	struct gpu_resource *resource;
	uint64_t displacement;
	uint32_t maximum;
	uint32_t attributes;
	unsigned mapping_count;
	int error;

	/* No failed request transfers ownership or accepts partial pages. */
	if (result == NULL)
		return EINVAL;
	*result = NULL;

	/* Token offsets and mapped sizes are complete nonempty page extents. */
	if (offset < 0 ||
	    bytes == 0 ||
	    ((uint64_t)offset & (KERN_PAGE_SIZE - 1U)) != 0 ||
	    (bytes & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* A failed or completed open cannot supply session-owned resource storage. */
	session = file->f_data;
	if (session == NULL)
		return ENODEV;

	/* All device mappings require the read authority used to discover their token. */
	if (!session->readable)
		return EACCES;

	/* Preserve the original open rights as the immutable ceiling for future mprotect. */
	maximum = KERN_PROT_READ;
	if (session->writable)
		maximum |= KERN_PROT_WRITE;

	/* Neither initial protection nor later VM changes may exceed that ceiling. */
	if ((prot & ~maximum) != 0)
		return EACCES;

	/* Admission excludes resource destruction until its mapping hold is acquired. */
	error = gpu_session_enter(session);
	if (error != 0)
		return error;

	/* Search only resources owned by this open file description. */
	resource = session->resources;
	while (resource != NULL) {
		/* A token names any whole-page subrange within one already discovered view. */
		if (resource->mapping_offset != 0 &&
		    (uint64_t)offset >= resource->mapping_offset &&
		    (uint64_t)offset - resource->mapping_offset < resource->mapping.bytes)
			break;

		/* Continue within this session; foreign resource tokens cannot be resolved here. */
		resource = resource->next;
	}

	/* Unknown or stale tokens must leave session admission without a storage hold. */
	if (resource == NULL) {
		gpu_session_leave(session);
		return EINVAL;
	}

	/* Bound the requested pages within the token's retained resource view. */
	displacement = (uint64_t)offset - resource->mapping_offset;
	if (bytes > resource->mapping.bytes - displacement) {
		gpu_session_leave(session);
		return EINVAL;
	}

	/* Refuse reference exhaustion before a new VM owner could wrap the counter. */
	mapping_count = atomic_raw_load_acquire(&resource->mapping_count);
	if (mapping_count == UINT_MAX) {
		gpu_session_leave(session);
		return EOVERFLOW;
	}

	/* The VM owner retains the file; its final callback drops this resource hold. */
	(void)atomic_raw_fetch_add_relaxed(&resource->mapping_count, 1U);
	attributes = 0U;
	if ((resource->mapping.attributes & DRV_GPU_MAPPING_DEVICE) != 0)
		attributes = VM_DEVICE_MMIO;

	/* A successful VM owner consumes the release callback and independently retains the file. */
	error = vm_device_create(
		file,
		resource->mapping.physical + displacement,
		(uint8_t *)resource->mapping.address + (size_t)displacement,
		bytes,
		attributes,
		maximum,
		gpu_mapping_release,
		resource,
		result);
	if (error != 0)
		gpu_mapping_release(resource);

	/* Resource creation and destruction may resume after the VM hold has been settled. */
	gpu_session_leave(session);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns a VM backing with immutable access rights and storage. */
	return 0;
}

/* Finishes all backing access before making the resource destroyable again. */
static void
gpu_mapping_release(
	void *owner)
{
	struct gpu_resource *resource;

	/* No field may be accessed after the final decrement permits destruction. */
	resource = owner;
	(void)atomic_raw_fetch_add_release(&resource->mapping_count, UINT_MAX);

	/* Succeeded: a zero mapping count now permits the owning session to destroy storage. */
	return;
}

/* Validates native display requests before invoking the backend's lease owner. */
static int
gpu_display_ioctl(
	struct gpu_session *session,
	unsigned long command,
	uintptr_t argument)
{
	union gpu_display_request request;
	struct drv_gpu_device *device;
	const struct drv_gpu_display_ops *ops;
	struct gpu_display_release rollback;
	struct gpu_resource *resource;
	uint64_t image_bytes;
	size_t bytes;
	uint32_t index;
	int error;
	int writing;

	/* Discovery and observation need read authority; ownership changes need write. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_DISPLAY) == 0)
		return EOPNOTSUPP;

	/* A lease change or presentation mutates the display and requires a writable open. */
	writing = 0;
	if (command == GPU_DISPLAY_CLAIM ||
	    command == GPU_DISPLAY_RELEASE ||
	    command == GPU_DISPLAY_PRESENT)
		writing = 1;

	/* Read-only opens can enumerate displays and observe owned completion state. */
	if ((writing && !session->writable) ||
	    (!writing && !session->readable))
		return EACCES;

	/* Registration validated all callbacks in this immutable optional operation table. */
	ops = device->ops->display;

	/* Copy exactly the selected ABI, so a short request cannot overread userspace. */
	switch (command) {
	case GPU_DISPLAY_QUERY:
		bytes = sizeof(request.query);
		break;
	case GPU_DISPLAY_MODE:
		bytes = sizeof(request.mode);
		break;
	case GPU_DISPLAY_CLAIM:
		bytes = sizeof(request.claim);
		break;
	case GPU_DISPLAY_RELEASE:
		bytes = sizeof(request.release);
		break;
	case GPU_DISPLAY_PRESENT:
		bytes = sizeof(request.present);
		break;
	case GPU_DISPLAY_WAIT:
		bytes = sizeof(request.wait);
		break;
	default:
		return EOPNOTSUPP;
	}

	/* Copies one bounded ABI payload without retaining its original userspace address. */
	memset(&request, 0, sizeof(request));
	error = copyin(argument, &request, bytes);
	if (error != 0)
		return error;

	/* Requires this command's exact record size before any backend callback can mutate state. */
	if (request.header.version != GPU_ABI_VERSION || request.header.size != bytes)
		return EINVAL;

	/* Each operation owns its semantic input and side-effect rollback checks. */
	switch (command) {
	case GPU_DISPLAY_QUERY:
		/* Only the ordinal is an input; never echo caller-supplied output fields. */
		if (request.query.reserved != 0)
			return EINVAL;

		/* Carries only the ordinal into the backend's newly zeroed output snapshot. */
		index = request.query.index;
		memset(&request.query, 0, sizeof(request.query));
		request.query.index = index;
		error = ops->query(device->private_data, session->backend, &request.query);
		if (error != 0)
			return error;

		/* A returned display must have a stable identity and a valid ordinal in its snapshot. */
		request.query.name[sizeof(request.query.name) - 1U] = '\0';
		if (index != GPU_DISPLAY_COUNT_ONLY &&
		    (request.query.display_id == 0 ||
		     request.query.generation == 0 ||
		     request.query.index >= request.query.count))
			return EIO;
		break;
	case GPU_DISPLAY_MODE:
		/* Validation is side-effect-free and does not select the active mode. */
		if (request.mode.display_id == 0 ||
		    request.mode.generation == 0 ||
		    request.mode.count != 0 ||
		    request.mode.flags != 0)
			return EINVAL;

		/* Only discovery and side-effect-free mode validation are defined in this ABI. */
		if (request.mode.operation != GPU_DISPLAY_MODE_ENUMERATE &&
		    request.mode.operation != GPU_DISPLAY_MODE_VALIDATE)
			return EINVAL;

		/* Enumeration provides geometry as output, while validation requires a complete input mode. */
		if (request.mode.operation == GPU_DISPLAY_MODE_ENUMERATE) {
			request.mode.width = 0;
			request.mode.height = 0;
			request.mode.refresh_millihz = 0;
		} else if (request.mode.index != 0 ||
		    request.mode.width == 0 ||
		    request.mode.height == 0 ||
		    request.mode.refresh_millihz == 0) {
			return EINVAL;
		}

		/* Lets the backend check the complete mode against this open's current display generation. */
		error = ops->mode(device->private_data, session->backend, &request.mode);
		if (error != 0)
			return error;
		break;
	case GPU_DISPLAY_CLAIM:
		/* A failed copyout must not leave a reservation the caller cannot release. */
		if (request.claim.display_id == 0 ||
		    request.claim.generation == 0 ||
		    request.claim.lease != 0)
			return EINVAL;

		/* Acquires exclusive native ownership before exposing the new lease identity. */
		error = ops->claim(device->private_data, session->backend, &request.claim);
		if (error != 0)
			return error;

		/* A successful ownership transition must return a usable nonzero release identity. */
		if (request.claim.lease == 0)
			return EIO;

		/* Publishes the acquired lease with the core's trusted ABI header. */
		request.claim.version = GPU_ABI_VERSION;
		request.claim.size = sizeof(request.claim);
		error = copyout(&request.claim, argument, sizeof(request.claim));
		if (error != 0) {
			/* Copy failure retains no user-visible lease, so return ownership to this backend. */
			memset(&rollback, 0, sizeof(rollback));
			rollback.version = GPU_ABI_VERSION;
			rollback.size = sizeof(rollback);
			rollback.lease = request.claim.lease;
			(void)ops->release(device->private_data, session->backend, &rollback);
			return error;
		}

		/* Succeeded: this exact open now owns the returned native lease. */
		return 0;
	case GPU_DISPLAY_RELEASE:
		/* Backend arbitration checks the lease belongs to this exact open. */
		if (request.release.lease == 0)
			return EINVAL;

		/* Consumes native ownership only after backend arbitration verifies the open and lease. */
		error = ops->release(device->private_data, session->backend, &request.release);
		if (error != 0)
			return error;

		/* Succeeded: the native display no longer retains the released lease. */
		return 0;
	case GPU_DISPLAY_PRESENT:
		/* Packed rows must occupy only live ordinary storage in this session. */
		if (request.present.lease == 0 ||
		    request.present.generation == 0 ||
		    request.present.sequence != 0 ||
		    request.present.flags != GPU_DISPLAY_PRESENT_FIFO)
			return EINVAL;

		/* Bounds packed row arithmetic before checking any resource extent. */
		if (request.present.width == 0 ||
		    request.present.width > UINT32_MAX / 4U ||
		    request.present.height == 0 ||
		    request.present.refresh_millihz == 0)
			return EINVAL;

		/* Accepts only the two packed channel layouts defined by this display ABI. */
		if (request.present.format != GPU_PIXEL_BGRA8888 &&
		    request.present.format != GPU_PIXEL_RGBA8888)
			return EINVAL;

		/* Requires a complete aligned packed row, including any explicit trailing pitch. */
		if (request.present.stride < request.present.width * 4U ||
		    (request.present.stride & 3U) != 0)
			return EINVAL;

		/* Resolves source storage within the same open that owns the presentation request. */
		error = gpu_resource_lookup(session, request.present.handle, &resource);
		if (error != 0)
			return error;

		/* Native presentation may read only live ordinary storage with a valid starting offset. */
		if (resource->kind != GPU_RESOURCE_STORAGE ||
		    request.present.offset > resource->bytes)
			return EINVAL;

		/* Bounds the whole image before the backend can read even its first pixel. */
		image_bytes = (uint64_t)request.present.stride * request.present.height;
		if (image_bytes > resource->bytes - request.present.offset)
			return EINVAL;

		/* Transfers only the resolved kernel resource and a validated immutable request snapshot. */
		error = ops->present(
			device->private_data,
			session->backend,
			resource->object,
			&request.present);
		if (error != 0)
			return error;

		/* Successful completion must identify a frame in this native lease's sequence. */
		if (request.present.sequence == 0)
			return EIO;
		break;
	case GPU_DISPLAY_WAIT:
		/* Observation cannot import stale output values into a backend response. */
		if (request.wait.lease == 0 ||
		    request.wait.completed_sequence != 0 ||
		    request.wait.present_time_ns != 0 ||
		    request.wait.generation != 0)
			return EINVAL;

		/* Observes completion only through the backend's own lease and timeout arbitration. */
		error = ops->wait(device->private_data, session->backend, &request.wait);
		if (error != 0)
			return error;
		break;
	default:
		return EOPNOTSUPP;
	}

	/* The core publishes the ABI header after the backend fills its bounded reply. */
	request.header.version = GPU_ABI_VERSION;
	request.header.size = (uint32_t)bytes;
	error = copyout(&request, argument, bytes);
	if (error != 0)
		return error;

	/* Succeeded: the caller received a complete initialized response for its owned object. */
	return 0;
}
