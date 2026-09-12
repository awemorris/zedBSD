/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

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

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GPU_DEVICE_BASE		0x00090000U
#define GPU_HANDLE_SLOT_BITS	8U

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
 * One resource owned by a session, a zero handle denotes an unused slot.
 */
struct gpu_resource {
	uint64_t handle;
	void *object;
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
	struct gpu_resource resources[GPU_SESSION_RESOURCE_MAX];
	unsigned writable;
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
static int gpu_handle_allocate(unsigned slot, uint64_t *handle);

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

	/* Only the complete first-generation contract is implemented here. */
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

	/* Rendering, mapping, and arbitrary capability bits remain unsupported. */
	if ((ops->capabilities & ~GPU_CAP_RESOURCE) != 0)
		return EOPNOTSUPP;

	/* A capability promises both allocation and infallible cleanup. */
	if ((ops->capabilities & GPU_CAP_RESOURCE) != 0) {
		/* Rejects storage support without its complete ownership pair. */
		if (ops->resource_create == NULL ||
		    ops->resource_destroy == NULL)
			return EINVAL;
	} else {
		/* Unadvertised callbacks would give callers conflicting contracts. */
		if (ops->resource_create != NULL ||
		    ops->resource_destroy != NULL)
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
		gpu_poll
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
	unsigned index;

	/* An unsuccessful open leaves no session for final-close cleanup. */
	session = file->f_data;
	if (session == NULL)
		return 0;

	/* File reference ownership excludes concurrent ioctls at final close. */
	file->f_data = NULL;
	device = session->device;

	/* Destroys all remaining objects while their backend session is valid. */
	for (index = 0; index < GPU_SESSION_RESOURCE_MAX; index++) {
		/* Only occupied slots own objects requiring backend cleanup. */
		if (session->resources[index].handle != 0) {
			device->ops->resource_destroy(device->private_data,
			    session->backend,
			    session->resources[index].object);
		}
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
	information.max_resources = GPU_SESSION_RESOURCE_MAX;
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
	struct gpu_info information;
	struct drv_gpu_device *device;
	void *object;
	unsigned slot;
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

	/* Enforces backend allocation limits before asking it to allocate. */
	memset(&information, 0, sizeof(information));
	error = device->ops->get_info(device->private_data,
				      session->backend,
				      &information);
	if (error != 0)
		return error;

	/* A zero limit advertises no allocatable storage. */
	if (request.bytes > information.max_resource_bytes)
		return EINVAL;

	/* Finds a session-local slot before allocating backend resources. */
	for (slot = 0; slot < GPU_SESSION_RESOURCE_MAX; slot++) {
		/* A zero handle means the slot owns no backend object. */
		if (session->resources[slot].handle == 0)
			break;
	}

	/* Resource accounting remains bounded independently for each session. */
	if (slot == GPU_SESSION_RESOURCE_MAX)
		return ENOSPC;

	/* Burns a unique generation even if later allocation or copyout fails. */
	error = gpu_handle_allocate(slot, &request.handle);
	if (error != 0)
		return error;

	/* Backend allocation receives a kernel copy of the validated request. */
	object = NULL;
	error = device->ops->resource_create(device->private_data,
					     session->backend,
					     &request,
					     &object);
	if (error != 0)
		return error;

	/* A successful callback must supply an object that can be destroyed. */
	if (object == NULL)
		return EIO;

	/* Publishes the handle to the caller before committing the table entry. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		device->ops->resource_destroy(device->private_data,
		    session->backend,
		    object);
		return error;
	}

	/* The busy reservation excludes all other accesses until commit ends. */
	session->resources[slot].object = object;
	session->resources[slot].handle = request.handle;

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
	unsigned slot;
	int error;

	/* A read-only description cannot mutate resources through ioctl. */
	if (session->writable == 0)
		return EACCES;

	/* Keeps optional cleanup callbacks paired with their advertised allocator. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_RESOURCE) == 0)
		return EOPNOTSUPP;

	/* Copies the versioned release request without user pointer retention. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects incompatible request layouts before handle lookup. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* The low byte identifies a resource slot and never encodes zero. */
	slot = (unsigned)(request.handle & 0xffU);
	if (slot == 0 || slot > GPU_SESSION_RESOURCE_MAX)
		return EINVAL;

	/* A matching global generation prevents stale and cross-session aliases. */
	resource = &session->resources[slot - 1U];
	if (resource->handle != request.handle)
		return EINVAL;

	/* Completes backend cleanup before making the slot reusable. */
	device->ops->resource_destroy(device->private_data,
				      session->backend,
				      resource->object);
	resource->object = NULL;
	resource->handle = 0;

	/* Succeeded: the released handle can never identify a future object. */
	return 0;
}

/* Stamps a resource slot with a generation that cannot wrap or alias. */
static int
gpu_handle_allocate(
	unsigned slot,
	uint64_t *handle)
{
	unsigned long irq;
	int error;

	/* Allocates one generation independently of session and device reuse. */
	error = 0;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (gpu_handle_generation == (UINT64_MAX >> GPU_HANDLE_SLOT_BITS)) {
		error = EOVERFLOW;
	} else {
		/* Zero remains invalid; every attempted allocation consumes a stamp. */
		gpu_handle_generation++;
		*handle = (gpu_handle_generation << GPU_HANDLE_SLOT_BITS) | (slot + 1U);
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Refuses exhaustion instead of reviving a stale handle. */
	if (error != 0)
		return error;

	/* Succeeded: the stamp is unique for the lifetime of the kernel. */
	return 0;
}
