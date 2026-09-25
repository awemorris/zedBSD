/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU registration, character-device sessions, and owned resource handles.
 */

#include <kern/klog.h>
#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-scanout.h>
#include <uapi/gpu-allocation.h>
#include <uapi/gpu-fence.h>
#include <uapi/gpu-job.h>
#include <drivers/gpu/gpu-fence.h>
#include <kern/cdev.h>
#include <kern/cred.h>
#include <kern/file.h>
#include <kern/filedesc.h>
#include <kern/fd-object.h>
#include <kern/handle.h>
#include <kern/process.h>
#include <kern/thread.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/uaccess.h>
#include <kern/page.h>
#include <kern/pmem.h>
#include <kern/vm-device.h>
#include <kern/clock.h>
#include <hal/hal.h>
#include <kern/sched.h>
#include <kern/waitq.h>
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <limits.h>

#define GPU_DEVICE_BASE		0x00090000U
#define GPU_RESOURCE_STORAGE	1U
#define GPU_RESOURCE_BLOB	2U
#define GPU_RESOURCE_SCANOUT	3U

#ifndef CONFIG_GPU_JOB_RESERVATION_MS
#define CONFIG_GPU_JOB_RESERVATION_MS 10000U
#endif
#ifndef CONFIG_GPU_JOB_EXECUTION_MS
#define CONFIG_GPU_JOB_EXECUTION_MS 60000U
#endif
#ifndef CONFIG_GPU_JOB_STOP_MS
#define CONFIG_GPU_JOB_STOP_MS 10000U
#endif

#if CONFIG_GPU_JOB_RESERVATION_MS < 1 || CONFIG_GPU_JOB_EXECUTION_MS < 1 || CONFIG_GPU_JOB_STOP_MS < 1
#error GPU supervision intervals must be finite positive milliseconds
#endif

/* One supervised job advances once from a producer reservation to terminal observation. */
enum gpu_job_state {
	GPU_JOB_NONE,
	GPU_JOB_RESERVED,
	GPU_JOB_COMMITTED,
	GPU_JOB_FINISHED
};

/* One failed session advances from requested stop to confirmed retirement. */
enum gpu_stop_state {
	GPU_STOP_NONE,
	GPU_STOP_REQUESTED,
	GPU_STOP_PENDING,
	GPU_STOP_FINISHED,
	GPU_STOP_QUARANTINED
};

/*
 * One registered GPU, retained by its owner and every cdev generation.
 *
 *  - The registry lock protects online, sessions and exported capability counts.
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
	unsigned shares;
	int error;
	uint64_t fault_epoch;
	unsigned fault_epoch_overflow;
	struct gpu_session *open_sessions;
	uint64_t identity;
	uint64_t capacity_sequence;
	unsigned capacity_overflow;

	/* Contexts isolated since the last checked reset; nonzero lets a sole fresh open reset a healthy device. */
	unsigned quarantined;
	unsigned recovering;
	struct wait_queue capacity_waitq;
	struct wait_queue monitor_waitq;
	struct mutex monitor_lock;
	struct thread *monitor;
	unsigned monitor_stopping;
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
	struct kernel_handle *scanout_source;
};

/*
 * One capability retaining an allocation independently from its exporting open.
 * A kernel handle owns this record; its final release drops the backend share
 * before releasing the device's withdrawal barrier.
 */
struct gpu_shared_resource {
	struct drv_gpu_device *device;
	void *object;
	struct gpu_image_descriptor image;
	struct gpu_allocation_descriptor allocation;
};

/*
 * One bounded completion slot embedded in its owning open description.
 * The registry lock protects state; drain ends backend callbacks before close.
 * Observers retain a slot until copyout finishes, even after another consumes it.
 */
struct drv_gpu_completion {
	struct gpu_session *session;
	uint64_t sequence;
	unsigned listed;
	unsigned completed;
	unsigned observers;
	unsigned consuming;
	unsigned job_action;
	enum gpu_job_state job_state;
	void *job_reservation;

	/* The backend still holds an unpublished reservation token the core must withdraw through cancel. */
	unsigned reservation_pending;
	uint64_t job_deadline;
	unsigned backend_owned;
	unsigned unpublished;
	int error;
};

/* One retained pending producer binding; the registry lock protects every field. */
struct gpu_fence_binding {
	struct kernel_handle *handle;
	uint64_t generation;
	uint64_t sequence;
	unsigned job;
};

/*
 * One open file description shared by dup/fork until its final close.
 *
 *  - Device session ownership keeps backend state alive.
 *  - Busy serializes ordinary ioctls through an interruptible reservation.
 *  - Completion observers sleep independently, permitting same-fd submission.
 */
struct gpu_session {
	struct drv_gpu_device *device;
	void *backend;
	struct gpu_resource *resources;
	uint32_t resource_count;
	uint64_t next_mapping_offset;
	unsigned writable;
	unsigned readable;

	/* Set before any stream may reach the GPU; zero proves the context never needed a native stop. */
	unsigned native_submitted;
	unsigned busy;
	struct thread *busy_owner;
	struct gpu_session *next_open;
	int error;
	unsigned closing;
	unsigned monitor_pins;
	enum gpu_stop_state stop_state;
	uint64_t stop_deadline;
	uint64_t stop_poll_deadline;
	struct wait_queue admission_waitq;
	struct wait_queue completion_waitq;
	struct drv_gpu_completion completions[GPU_SUBMIT_MAX];
	struct gpu_fence_binding fences[GPU_SUBMIT_MAX];

	/* The registry lock protects per-open observation and successful acknowledgement. */
	uint64_t topology_observed;
	uint64_t topology_acknowledged;
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
static int gpu_events_snapshot(struct gpu_session *session, uint64_t *sequence);
static int gpu_events_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_create_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_destroy_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_handle_allocate(uint64_t *handle);
static int gpu_resource_lookup(struct gpu_session *session, uint64_t handle, struct gpu_resource **result);
static int gpu_resource_reserve(struct gpu_session *session, uint64_t bytes, struct gpu_resource **result);
static int gpu_user_range(uint64_t address, uint32_t bytes, uintptr_t *pointer);
static int gpu_capset_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_blob_ioctl(struct gpu_session *session, uintptr_t argument, unsigned placed);
static int gpu_transfer_ioctl(struct gpu_session *session, uintptr_t argument, unsigned writing);
static int gpu_command_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_submit_ioctl(struct gpu_session *session, uintptr_t argument, uint64_t *sequence);
static int gpu_job_reserve_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_capacity_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_policy_ioctl(struct gpu_session *session, uintptr_t argument);
static void gpu_capacity_changed_locked(struct drv_gpu_device *device);
static int gpu_session_error_locked(struct gpu_session *session);
static void gpu_session_fail_locked(struct gpu_session *session, int error, unsigned hard);
static uint64_t gpu_monitor_deadline(uint64_t milliseconds);
static int gpu_monitor_start(struct drv_gpu_device *device);
static int gpu_monitor_stop(struct drv_gpu_device *device);
static void gpu_monitor(void *argument);
static uint64_t gpu_monitor_step(struct drv_gpu_device *device);
static void gpu_monitor_close(struct gpu_session *session);
static void gpu_session_reclaim(struct drv_gpu_device *device, struct gpu_session *session);
static int gpu_recover_open(struct drv_gpu_device *device);
static int gpu_job_action_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);
static int gpu_job_bind(struct gpu_session *session, struct drv_gpu_completion *completion, struct kernel_handle *handle, uint64_t generation);
static void gpu_job_unbind(struct gpu_session *session, uint64_t sequence);
static int gpu_job_admit(struct gpu_session *session, uint64_t sequence);
static int gpu_job_observe(struct gpu_session *session, uint64_t sequence, struct drv_gpu_completion **result);
static int gpu_wait_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_fence_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);
static int gpu_fence_create_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_fence_bind_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_fence_state_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);
static int gpu_fence_get(struct gpu_session *session, int fd, unsigned foreign, struct kernel_handle **result);
static int gpu_fence_binding_add(struct gpu_session *session, struct kernel_handle *handle, uint64_t generation, uint64_t sequence);
static int gpu_fence_binding_end(struct gpu_session *session, struct kernel_handle *handle, uint64_t generation, unsigned release, int status);
static void gpu_fence_retire(struct gpu_session *session);
static int gpu_dependency_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);
static int gpu_dependency_wait(struct gpu_session *session, int fd, uint64_t generation, unsigned foreign);

static int gpu_completion_reserve(struct gpu_session *session, uint64_t sequence, struct drv_gpu_completion **result);
static void gpu_completion_discard(struct drv_gpu_completion *completion);
static int gpu_completion_observe(struct gpu_session *session, const struct gpu_command_wait *request, struct drv_gpu_completion **result);
static void gpu_completion_observer_leave(struct drv_gpu_completion *completion, unsigned consume, unsigned reserved);
static int gpu_completion_deadline(uint64_t timeout, uint64_t *deadline);
static int gpu_present_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_map_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_mmap(struct file *file, off_t offset, size_t bytes, uint32_t prot, struct vm_device_mapping **result);
static void gpu_mapping_release(void *owner);
static int gpu_display_ioctl(struct gpu_session *session, unsigned long command, uintptr_t argument);
static int gpu_export_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_import_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_allocation_export_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_allocation_import_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_device_query_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_constraints_ioctl(struct gpu_session *session, uintptr_t argument);
static int gpu_scanout_import(struct gpu_session *session, struct gpu_shared_resource *shared, void **object);
static int gpu_allocation_validate(struct gpu_resource *resource, const struct gpu_allocation_descriptor *allocation);
static int gpu_image_validate(struct gpu_resource *resource, const struct gpu_image_descriptor *image);
static void gpu_shared_release(void *object);
static int gpu_shared_hold_device(struct drv_gpu_device *device);
static int gpu_export_install(struct kernel_handle *handle, void *request, size_t bytes, uint32_t policy, int32_t *fd, uintptr_t argument);
static const struct kernel_handle_ops *gpu_shared_operations(void);

/*
 * Retains a protected GPU wrapper independently from device registration.
 */
void
drv_gpu_retain(
	struct drv_gpu_device *device)
{
	/* An absent backend publication carries no wrapper reference. */
	if (device == NULL)
		return;

	/* Existing ownership excludes resurrection while IRQ-side snapshots take a hold. */
	refcount_get(&device->references);

	/* Succeeded: this observer may outlive namespace withdrawal. */
	return;
}

/*
 * Releases a retained wrapper without touching borrowed backend state.
 */
void
drv_gpu_release(
	struct drv_gpu_device *device)
{
	/* Empty publication snapshots need no release. */
	if (device == NULL)
		return;

	/* The final wrapper release is independent from hardware teardown. */
	gpu_device_release(device);

	/* Succeeded: this reference no longer retains the wrapper. */
	return;
}

/*
 * Publishes one backend retirement without overwriting an earlier logical error.
 */
void
drv_gpu_complete(
	struct drv_gpu_completion *completion,
	int error)
{
	struct gpu_fence_binding *binding;
	struct kernel_handle *retired[GPU_SUBMIT_MAX];
	unsigned retired_count;
	unsigned index;
	unsigned long irq;

	/* A refused backend submission has no completion obligation. */
	if (completion == NULL)
		return;

	/* Terminal observation and actual backend retirement have distinct ownership. */
	retired_count = 0U;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* Only the first terminal result may define this generation's public status. */
	if (completion->completed == 0U) {
		/* A reservation without admission cannot prove successful GPU execution. */
		if (completion->job_state == GPU_JOB_RESERVED && error == 0)
			error = EIO;

		/* Ordinary notifications still distinguish receipt from GPU job success. */
		completion->error = error;
		completion->completed = 1U;

		/* A supervised generation cannot return to reservation after terminal publication. */
		if (completion->job_state != GPU_JOB_NONE)
			completion->job_state = GPU_JOB_FINISHED;
	}

	/* A late success cannot erase a timeout, while actual callback retirement releases its pin. */
	completion->backend_owned = 0U;
	completion->job_deadline = 0U;

	/* Exact sequence ownership joins shared fence lifetime to native completion. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		binding = &completion->session->fences[index];
		if (binding->handle == NULL || binding->sequence != completion->sequence)
			continue;

		/* Legacy notifications propagate failures; supervised jobs can also prove success. */
		if (binding->job != 0U || completion->error != 0) {
			(void)drv_gpu_fence_signal_deferred(
				binding->handle,
				binding->generation,
				completion->session,
				completion->error);
		}

		/* Logical error retained this hold until the backend stopped using its completion. */
		if (binding->job != 0U) {
			retired[retired_count] = binding->handle;
			retired_count++;
			kern_memset(binding, 0, sizeof(*binding));
		}
	}

	/* Consumed logical errors can reclaim their record only after native callback retirement. */
	if (completion->listed == 0U && completion->observers == 0U)
		completion->sequence = 0U;

	/* Reclamation waiters wake even when they must consume a newly terminal record first. */
	waitq_wake_all(&completion->session->completion_waitq);
	gpu_capacity_changed_locked(completion->session->device);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Payload finalizers run only after their binding pointers have left the registry. */
	for (index = 0U; index < retired_count; index++)
		handle_put(retired[index]);

	/* Descriptor and imported-fence observers receive the same terminal state. */
	poll_notify();

	/* Succeeded: actual backend retirement ended this callback's ownership. */
	return;
}

/*
 * Publishes device loss or checked recovery to retained GPU descriptors.
 */
void
drv_gpu_report_error(
	struct drv_gpu_device *device,
	int error)
{
	struct gpu_session *session;
	struct gpu_fence_binding *binding;
	unsigned index;
	unsigned long irq;

	/* A transport may fail before its GPU node is published. */
	if (device == NULL)
		return;

	/* Wakes both admission and observation when device availability changes. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* Every new nonzero report is distinct, even when its errno equals the previous fault. */
	if (error != 0) {
		/* Saturation stays explicit because a reused epoch cannot protect checked reset. */
		if (device->fault_epoch == UINT64_MAX) {
			device->fault_epoch_overflow = 1U;
		} else {
			device->fault_epoch++;
		}
	}

	/* Publish the new device state before waking capacity and every retained open. */
	device->error = error;
	gpu_capacity_changed_locked(device);

	/* Each open keeps its own sticky loss and exact producer-generation ownership. */
	session = device->open_sessions;
	while (session != NULL) {
		/* Device loss terminates every retained producer generation, including pre-submit reservations. */
		if (error != 0) {
			gpu_session_fail_locked(session, error, 1U);
			session->stop_state = GPU_STOP_FINISHED;

			/* Existing shared aliases observe the same retained error generation. */
			for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
				binding = &session->fences[index];
				if (binding->handle != NULL)
					(void)drv_gpu_fence_signal_deferred(binding->handle, binding->generation, session, error);
			}
		}

		/* Wake this open before advancing to the next retained session. */
		waitq_wake_all(&session->admission_waitq);
		waitq_wake_all(&session->completion_waitq);
		session = session->next_open;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Poll observes loss independently of its requested readiness mask. */
	poll_notify();

	/* Succeeded: old descriptors see failure and a fresh recovery clears it. */
	return;
}

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
	device->capacity_sequence = 1U;
	waitq_init(&device->capacity_waitq, "GPU submission capacity");
	waitq_init(&device->monitor_waitq, "GPU job supervision");
	refcount_init(&device->references, 1);

	/* A failed monitor gate cannot publish a partially initialized device. */
	error = mutex_init(&device->monitor_lock, LOCK_RANK_DEVICE, "GPU monitor lifetime");
	if (error != 0) {
		gpu_device_release(device);
		atomic_store_release(&gpu_lifecycle, 0);
		return error;
	}

	/* Device identities share the non-reusable generation domain with resource handles. */
	error = gpu_handle_allocate(&device->identity);
	if (error != 0) {
		gpu_device_release(device);
		atomic_store_release(&gpu_lifecycle, 0);
		return error;
	}

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
	unsigned shares;
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
	shares = device->shares;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Withdrawal arms DMA retention before publishing permission for global-error teardown. */
	if (device->ops->recovery != NULL)
		device->ops->recovery->fault(device->private_data, ENODEV);

	/* Wakes admissions and observers after the backend has established safe quarantine. */
	drv_gpu_report_error(device, ENODEV);

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
	if (sessions != 0 || shares != 0) {
		atomic_store_release(&gpu_lifecycle, 0);
		return EBUSY;
	}

	/* Ends the common worker before borrowed backend state can disappear. */
	error = gpu_monitor_stop(device);
	if (error != 0) {
		atomic_store_release(&gpu_lifecycle, 0);
		return error;
	}

	/* Releases registration ownership and makes this number reusable. */
	*position = device->next;
	device->next = NULL;
	gpu_device_release(device);
	atomic_store_release(&gpu_lifecycle, 0);

	/* Succeeded: the owner may free its operations and private state. */
	return 0;
}

/*
 * Publishes one session's loss while independent contexts remain available.
 */
void
drv_gpu_report_session_error(
	struct drv_gpu_device *device,
	void *backend,
	int error)
{
	struct gpu_session *session;
	unsigned long irq;

	/* A missing publication or success cannot create a sticky local failure. */
	if (device == NULL || error == 0)
		return;

	/* Backend lifetime ownership makes its private pointer an exact live-session key. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	session = device->open_sessions;
	while (session != NULL) {
		/* Only the originating context loses new submission authority. */
		if (session->backend == backend) {
			gpu_session_fail_locked(session, error, 1U);
			break;
		}

		/* Each linked session is retained until final backend teardown completes. */
		session = session->next_open;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* External fence aliases and this GPU descriptor observe the same local loss. */
	poll_notify();

	/* Succeeded: logical error is visible without claiming that DMA has stopped. */
	return;
}

/*
 * Announces backend capacity after the final descriptor or reservation release.
 */
void
drv_gpu_capacity_changed(
	struct drv_gpu_device *device)
{
	unsigned long irq;

	/* Early transport initialization may precede framework registration. */
	if (device == NULL)
		return;

	/* The common condition joins backend release with per-open record retirement. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	gpu_capacity_changed_locked(device);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: capacity waiters may recheck both independently owned limits. */
	return;
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
	    GPU_CAP_PRESENT | GPU_CAP_MAPPING | GPU_CAP_DISPLAY | GPU_CAP_SHARE |
	    GPU_CAP_NOTIFICATION | GPU_CAP_ALLOCATION_SHARE | GPU_CAP_FENCE |
	    GPU_CAP_DISPLAY_EVENTS | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY)) != 0)
		return EOPNOTSUPP;

	/* Supervised jobs require reservation, publication, cancellation and callback drain together. */
	if ((ops->capabilities & GPU_CAP_JOB) != 0U) {
		/* Completion records remain observable through the existing notification interface. */
		if ((ops->capabilities & GPU_CAP_NOTIFICATION) == 0U || ops->jobs == NULL)
			return EINVAL;

		/* Partial job operations cannot guarantee every reservation reaches a terminal outcome. */
		if (ops->jobs->reserve == NULL ||
		    ops->jobs->commit == NULL ||
		    ops->jobs->cancel == NULL)
			return EINVAL;
	} else {
		/* Unadvertised callbacks must not manufacture hidden submission authority. */
		if (ops->jobs != NULL)
			return EINVAL;
	}

	/* Capacity snapshots must accompany an advertised supervised job interface. */
	if ((ops->capabilities & GPU_CAP_JOB_CAPACITY) != 0U) {
		/* Both layers must describe actual reservable storage without sleeping. */
		if (ops->jobs == NULL || ops->jobs->capacity == NULL)
			return EINVAL;
	}

	/* Every supervised backend can quarantine work whose safe stop is unknown. */
	if ((ops->capabilities & GPU_CAP_JOB) != 0U) {
		/* A missing fault operation cannot implement finite supervision safely. */
		if (ops->recovery == NULL || ops->recovery->fault == NULL)
			return EINVAL;
	}

	/* Optional local stopping requires both nonblocking stages. */
	if (ops->recovery != NULL) {
		/* Raw-command close also needs quarantine when supervision or local stop fails. */
		if (ops->recovery->fault == NULL)
			return EINVAL;

		/* A begin without a confirmation operation cannot prove retirement. */
		if (ops->recovery->stop_begin != NULL && ops->recovery->stop_poll == NULL)
			return EINVAL;

		/* A confirmation without stopping new native work is insufficient. */
		if (ops->recovery->stop_poll != NULL && ops->recovery->stop_begin == NULL)
			return EINVAL;

		/* Isolation is only meaningful after a local stop handshake has been attempted. */
		if (ops->recovery->isolate != NULL && ops->recovery->stop_begin == NULL)
			return EINVAL;
	}

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

	/* A placed allocator extends an existing blob namespace and its ordinary cleanup. */
	if (ops->blob_create_placed != NULL && (ops->capabilities & GPU_CAP_BLOB) == 0U)
		return EINVAL;

	/* Every resource type shares one infallible terminal cleanup operation. */
	if ((ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_BLOB)) != 0 ||
	    (ops->scanout != NULL && ops->scanout->import_image != NULL)) {
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

	/* Queued completion requires submission and a final-close callback barrier. */
	if ((ops->capabilities & GPU_CAP_NOTIFICATION) != 0U) {
		/* An incomplete optional table cannot retain asynchronous callbacks safely. */
		if (ops->commands == NULL)
			return EINVAL;

		/* Submission and drain together bound every accepted request lifetime. */
		if (ops->commands->submit == NULL || ops->commands->drain == NULL)
			return EINVAL;
	} else if (ops->commands != NULL) {
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

		/* A display-only backend may consume imported allocations without creating render storage. */
		if ((ops->capabilities & GPU_CAP_RESOURCE) == 0 &&
		    (ops->scanout == NULL || ops->scanout->import_image == NULL))
			return EINVAL;
	} else if (ops->display != NULL) {
		return EINVAL;
	}

	/* Display event readiness requires an explicitly advertised nonblocking snapshot. */
	if ((ops->capabilities & GPU_CAP_DISPLAY_EVENTS) != 0U) {
		/* Events cannot exist without an ordinary display inventory to re-query. */
		if (ops->display == NULL)
			return EINVAL;

		/* The snapshot must remain usable while ordinary presentation is waiting. */
		if (ops->display->events == NULL)
			return EINVAL;
	} else if (ops->display != NULL) {
		/* Hidden callbacks must not contradict the published capability mask. */
		if (ops->display->events != NULL)
			return EINVAL;
	}

	/* Exported allocations need independent release and receiver-context import together. */
	if ((ops->capabilities & (GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE)) != 0) {
		/* Shared resources must originate in a supported blob namespace. */
		if ((ops->capabilities & GPU_CAP_BLOB) == 0)
			return EINVAL;

		/* A missing share table cannot preserve exported ownership. */
		if (ops->share == NULL)
			return EINVAL;

		/* Every successful export has both terminal cleanup and a defined import operation. */
		if (ops->share->export_resource == NULL ||
		    ops->share->release == NULL ||
		    ops->share->import_resource == NULL)
			return EINVAL;
	} else if (ops->share != NULL) {
		return EINVAL;
	}

	/* Optional scanout import still requires a complete display lifetime and constraints query. */
	if (ops->scanout != NULL) {
		if (ops->scanout->query_device == NULL)
			return EINVAL;
		if (ops->scanout->import_image != NULL &&
		    (!(ops->capabilities & GPU_CAP_DISPLAY) || ops->scanout->constraints == NULL))
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
		.open = gpu_open,
		.close = gpu_close,
		.ioctl = gpu_ioctl,
		.poll = gpu_poll,
		.mmap = gpu_mmap
	};
	struct cdev *node;
	char name[32];
	int error;

	/* Formats a bounded name from a framework-controlled slot number. */
	kern_snprintf(name, sizeof(name), "gpu%u", device->number);

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

	/* Initializes both independent wait channels before publishing the open. */
	waitq_init(&session->admission_waitq, "GPU session admission");
	waitq_init(&session->completion_waitq, "GPU command completion");

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

	/* A checked recovery may rebuild hardware only after old owners have retired. */
	error = gpu_recover_open(device);
	if (error != 0) {
		gpu_session_drop(session);
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

	/* Links the initialized open so removal and failure wake all its waiters. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	session->next_open = device->open_sessions;
	device->open_sessions = session;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

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
	unsigned long irq;
	int error;

	/* An unsuccessful open leaves no session for final-close cleanup. */
	session = file->f_data;
	if (session == NULL)
		return 0;

	/* File reference ownership excludes concurrent ioctls at final close. */
	file->f_data = NULL;
	device = session->device;

	/*
	 * A graceful producer close stops new admission and keeps every committed
	 * job supervised, so its real completion still signals success rather than
	 * the former unconditional device-loss error.  Reserved and ordinary
	 * command generations the departed producer can no longer publish end now.
	 */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	gpu_session_fail_locked(session, ENODEV, 0U);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Descriptor pollers observe local loss without waiting for native retirement. */
	poll_notify();

	/* Raw native commands also require a supervisor before asynchronous stop begins. */
	if (device->ops->recovery != NULL) {
		error = gpu_monitor_start(device);
		if (error != 0) {
			/* Failed supervisor allocation cannot authorize uncertain resource destruction. */
			device->ops->recovery->fault(device->private_data, error);
			drv_gpu_report_error(device, error);
		}
	}

	/* Keep resources and embedded completion storage alive until every native callback has retired. */
	if (device->ops->commands != NULL)
		device->ops->commands->drain(device->private_data, session->backend);

	/* A supervised committed job signaled its real result during drain; end any leftover binding now. */
	gpu_fence_retire(session);

	/* No monitor callback may inspect the backend while terminal objects are destroyed. */
	gpu_monitor_close(session);

	/* Destroys all owned objects while their backend session remains valid. */
	while (session->resources != NULL) {
		resource = session->resources;
		session->resources = resource->next;
		device->ops->resource_destroy(
			device->private_data,
			session->backend,
			resource->object);
		if (resource->scanout_source != NULL)
			handle_put(resource->scanout_source);
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
	struct gpu_session **position;
	unsigned long irq;

	/* Removes this open before any failure notification can walk its wait queues. */
	device = session->device;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	position = &device->open_sessions;
	while (*position != NULL) {
		/* Failed backend opens were never linked into the published-open list. */
		if (*position == session) {
			*position = session->next_open;
			break;
		}

		/* Advances only over retained opens protected by the registry lock. */
		position = &(*position)->next_open;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Frees per-open memory while its device still has a withdrawal barrier. */
	kern_free(session);

	/* Zero tells the owner that every backend close callback has finished. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->sessions--;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: this session no longer prevents backend removal. */
	return;
}

/* Serializes ordinary operations without holding a lock through driver execution. */
static int
gpu_session_enter(
	struct gpu_session *session)
{
	struct thread *caller;
	uint64_t observed;
	unsigned long irq;
	int error;

	/* Waits for the shared handle table while completion observations remain independent. */
	error = 0;
	caller = thread_current();
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* A recursive kernel callback cannot sleep waiting for its own outer operation. */
	if (session->busy != 0U && session->busy_owner == caller) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EBUSY;
	}

	while (session->busy != 0U &&
	    session->device->online != 0U &&
	    session->device->error == 0 &&
	    session->error == 0) {
		observed = waitq_sequence(&session->admission_waitq);
		error = waitq_sleep(
			&session->admission_waitq,
			&gpu_registry_lock,
			observed,
			0U,
			WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN)
			break;
	}

	/* Admission rechecks withdrawal after every wakeup and before reserving the table. */
	if (error == EAGAIN)
		error = 0;

	/* Device withdrawal always refuses new resource-table operations. */
	if (error == 0 && session->device->online == 0U)
		error = ENODEV;

	/* Local device loss rejects new work without contaminating independent opens. */
	if (error == 0)
		error = gpu_session_error_locked(session);

	/* Busy now belongs to this operation through backend work and user copyout. */
	if (error == 0) {
		session->busy = 1U;
		session->busy_owner = caller;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Reports interruption or withdrawal without taking the reservation. */
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
	session->busy_owner = NULL;
	waitq_wake_all(&session->admission_waitq);

	/* Only a stopping session's admission release advances the monitor's handshake. */
	if (session->stop_state != GPU_STOP_NONE && session->stop_state != GPU_STOP_FINISHED)
		waitq_wake_all(&session->device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: a later ioctl may enter this session. */
	return;
}

/* Dispatches only fixed-size, versioned requests into the GPU core. */
#ifndef GPU_IOCTL_TRACE
#define GPU_IOCTL_TRACE 0
#endif

static int gpu_ioctl_body(struct file *file, unsigned long command, uintptr_t argument);

/*
 * WS031 E-127: with -DGPU_IOCTL_TRACE=1 every failing GPU ioctl is logged by its 'G' number,
 * for backend bring-up (the application only names the Vulkan call it was in).
 */
static int
gpu_ioctl(
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	int error = gpu_ioctl_body(file, command, argument);

	if (GPU_IOCTL_TRACE && error != 0)
		kern_logf("gpu: ioctl G%lu (cmd 0x%lx) -> error %d\n",
			command & 0xffUL, command, error);
	return error;
}

static int
gpu_ioctl_body(
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

	/* Capacity and policy observation never own ordinary resource-table admission. */
	if (command == GPU_JOB_CAPACITY) {
		error = gpu_capacity_ioctl(session, argument);
		if (error != 0)
			return error;

		/* Succeeded: the caller may reclaim and retry without an accepted job. */
		return 0;
	}

	/* Supervision policy remains inspectable independently from renderer health. */
	if (command == GPU_JOB_POLICY) {
		error = gpu_policy_ioctl(session, argument);
		if (error != 0)
			return error;

		/* Succeeded: effective finite policy intervals were returned. */
		return 0;
	}

	/* Job reservations leave the resource table available for their native submission. */
	if (command == GPU_JOB_RESERVE) {
		error = gpu_job_reserve_ioctl(session, argument);
		if (error != 0)
			return error;

		/* Succeeded: a supervised reservation precedes any native submission. */
		return 0;
	}

	/* Finalizing a reservation progresses independently from ordinary admission. */
	if (command == GPU_JOB_COMMIT || command == GPU_JOB_CANCEL) {
		error = gpu_job_action_ioctl(session, command, argument);
		if (error != 0)
			return error;

		/* Succeeded: the exact reservation was committed or explicitly canceled. */
		return 0;
	}

	/* A completion wait must allow another thread on this fd to submit work. */
	if (command == GPU_COMMAND_WAIT) {
		error = gpu_wait_ioctl(session, argument);
		if (error != 0)
			return error;

		/* Succeeded: the selected observation never retained table admission. */
		return 0;
	}

	/* Topology observation must remain responsive while this open is presenting. */
	if (command == GPU_DISPLAY_EVENTS) {
		error = gpu_events_ioctl(session, argument);
		if (error != 0)
			return error;

		/* Succeeded: observation or exact acknowledgement did not retain table admission. */
		return 0;
	}

	/* Shared completion state must progress while another thread waits before hardware submission. */
	if (command == GPU_FENCE_CREATE ||
	    command == GPU_FENCE_QUERY ||
	    command == GPU_FENCE_WAIT ||
	    command == GPU_FENCE_RESET ||
	    command == GPU_FENCE_SIGNAL ||
	    command == GPU_FENCE_BIND) {
		error = gpu_fence_ioctl(session, command, argument);
		if (error != 0)
			return error;

		/* Succeeded: the independent completion payload operation has finished. */
		return 0;
	}

	/* Dependencies are resolved before ordinary session admission can block their producer. */
	if (command == GPU_COMMAND_SUBMIT_SYNC || command == GPU_DISPLAY_PRESENT_SYNC) {
		error = gpu_dependency_ioctl(session, command, argument);
		if (error != 0)
			return error;

		/* Succeeded: the submitted operation retained its exact prerequisite and completion capabilities. */
		return 0;
	}

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
		error = gpu_blob_ioctl(session, argument, 0U);
		break;
	case GPU_BLOB_CREATE_PLACED:
		/* Physical conditions must be guaranteed before a blob ownership token is published. */
		error = gpu_blob_ioctl(session, argument, 1U);
		break;
	case GPU_RESOURCE_READ:
		/* Copies bytes out of a retained resource after checking read authority. */
		error = gpu_transfer_ioctl(session, argument, 0);
		break;
	case GPU_RESOURCE_WRITE:
		/* Copies validated userspace bytes into a retained resource. */
		error = gpu_transfer_ioctl(session, argument, 1);
		break;
	case GPU_COMMAND_SUBMIT:
		/* Retains bounded independent command bytes and a session completion identity. */
		error = gpu_submit_ioctl(session, argument, NULL);
		break;
	case GPU_COMMAND:
		/* Submits an independent kernel copy of the backend command stream. */
		error = gpu_command_ioctl(session, argument);
		break;
	case GPU_RESOURCE_EXPORT:
		/* Exports one allocation capability without granting session-wide authority. */
		error = gpu_export_ioctl(session, argument);
		break;
	case GPU_RESOURCE_IMPORT:
		/* Imports a typed capability into this independent GPU open. */
		error = gpu_import_ioctl(session, argument);
		break;
	case GPU_ALLOCATION_EXPORT:
		/* Exports allocation authority without imposing a linear image layout. */
		error = gpu_allocation_export_ioctl(session, argument);
		break;
	case GPU_ALLOCATION_IMPORT:
		/* Attaches an allocation and returns its immutable userspace protocol metadata. */
		error = gpu_allocation_import_ioctl(session, argument);
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
	case GPU_DEVICE_QUERY:
		/* Resolves stable node roles without requiring a renderer capset. */
		error = gpu_device_query_ioctl(session, argument);
		break;
	case GPU_DISPLAY_CONSTRAINTS:
		/* Display allocation requirements precede swapchain path selection. */
		error = gpu_constraints_ioctl(session, argument);
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

/* Reports independent command, display-change and device-loss readiness. */
static int
gpu_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct gpu_session *session;
	struct drv_gpu_completion *completion;
	unsigned index;
	unsigned long irq;
	uint64_t sequence;
	int error;

	/* A poll result must have storage owned by the VFS caller. */
	if (revents == NULL)
		return EINVAL;

	/* Samples readiness while the file retains its open description. */
	*revents = 0;
	session = file->f_data;
	if (session == NULL) {
		*revents = POLLERR | POLLHUP;
		return 0;
	}

	/* Device failure is independent of the caller's selected event mask. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (session->device->online == 0U)
		*revents |= POLLERR | POLLHUP;

	/* A failed but registered GPU can later recover after all old references retire. */
	if (session->device->error != 0 || session->error != 0)
		*revents |= POLLERR;

	/* Readability persists until every terminal record has been consumed. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		completion = &session->completions[index];
		if (completion->listed != 0U && completion->completed != 0U) {
			*revents |= events & (POLLIN | POLLRDNORM);
			break;
		}
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Only readable display-event opens perform the independent nonblocking snapshot. */
	if ((events & POLLPRI) != 0 && session->readable != 0U &&
	    (session->device->ops->capabilities & GPU_CAP_DISPLAY_EVENTS) != 0U) {
		error = gpu_events_snapshot(session, &sequence);
		if (error != 0) {
			*revents |= POLLERR;
		} else {
			/* A concurrent ACK may clear only the sequence that its own copyout confirmed. */
			irq = spin_lock_irqsave(&gpu_registry_lock);

			if (sequence > session->topology_acknowledged)
				*revents |= POLLPRI;

			spin_unlock_irqrestore(&gpu_registry_lock, irq);
		}
	}

	/* Succeeded: each readiness bit retains its own completion or topology meaning. */
	return 0;
}

/* Samples driver-owned event state while the retained file excludes backend destruction. */
static int
gpu_events_snapshot(
	struct gpu_session *session,
	uint64_t *sequence)
{
	struct drv_gpu_device *device;
	unsigned long irq;
	int error;

	/* Device errors are observed without admitting an ordinary ioctl or calling the backend. */
	device = session->device;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	error = gpu_session_error_locked(session);
	if (device->online == 0U)
		error = ENODEV;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* An offline or failed instance cannot yield an authoritative topology snapshot. */
	if (error != 0)
		return error;

	/* The immutable callback may take its short IRQ-safe event lock, never a controller mutex. */
	*sequence = 0U;
	error = device->ops->display->events(device->private_data, session->backend, sequence);
	if (error != 0)
		return error;

	/* Zero would let a fresh open mistake an unqueried inventory for acknowledged state. */
	if (*sequence == 0U)
		return EIO;

	/* Succeeded: the backend remains retained even if unregister raced this callback. */
	return 0;
}

/* Publishes an event snapshot before committing any exact acknowledgement to this open. */
static int
gpu_events_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_display_events request;
	uint64_t sequence;
	uint64_t acknowledged;
	unsigned long irq;
	int error;

	/* Event queries cannot expose an unadvertised callback or bypass read authority. */
	if ((session->device->ops->capabilities & GPU_CAP_DISPLAY_EVENTS) == 0U)
		return EOPNOTSUPP;

	/* Acknowledging this open's observation does not mutate hardware or require write access. */
	if (session->readable == 0U)
		return EACCES;

	/* Every output field is input zero, independent from any previous QUERY reply. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Versioned fixed-width framing must agree before any state is sampled. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request))
		return EINVAL;

	/* Unknown flags or reused output fields cannot acquire acknowledgement authority. */
	if ((request.flags & ~GPU_DISPLAY_EVENT_ACK) != 0U ||
	    request.events != 0U || request.sequence != 0U || request.reserved != 0U)
		return EINVAL;

	/* QUERY has no acknowledgement input; ACK always identifies one earlier snapshot. */
	if (request.flags == 0U) {
		/* A non-destructive query cannot carry an implicit acknowledgement. */
		if (request.ack_sequence != 0U)
			return EINVAL;
	} else if (request.ack_sequence == 0U) {
		return EINVAL;
	}

	/* Driver state is obtained outside the registry lock and ordinary session admission. */
	error = gpu_events_snapshot(session, &sequence);
	if (error != 0)
		return error;

	/* Only a snapshot already returned successfully to this open may be acknowledged. */
	error = 0;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	acknowledged = session->topology_acknowledged;
	if (request.ack_sequence > session->topology_observed || request.ack_sequence > sequence)
		error = EINVAL;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Rejection leaves both the observed sequence and readiness unchanged. */
	if (error != 0)
		return error;

	/* The response describes the snapshot after this requested acknowledgement, not future events. */
	if (request.ack_sequence > acknowledged)
		acknowledged = request.ack_sequence;

	/* Newer sequence values remain pending even when an older snapshot is acknowledged. */
	request.sequence = sequence;
	if (sequence > acknowledged)
		request.events = GPU_DISPLAY_EVENT_CHANGE;

	/* A failed user copy may have partial bytes but can never consume notification state. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Concurrent queries and acknowledgements monotonically advance their independent cursors. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (sequence > session->topology_observed)
		session->topology_observed = sequence;

	/* A racing newer event is never acknowledged through this older exact sequence. */
	if (request.ack_sequence > session->topology_acknowledged)
		session->topology_acknowledged = request.ack_sequence;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: only the successfully returned observation and requested ACK were committed. */
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
	kern_memset(&information, 0, sizeof(information));
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

	/* Imported scanout backing also owns an ordinary destroyable resource. */
	device = session->device;
	if ((device->ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_BLOB)) == 0) {
		if (device->ops->scanout == NULL)
			return EOPNOTSUPP;

		if (device->ops->scanout->import_image == NULL)
			return EOPNOTSUPP;
	}

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
	if (resource->scanout_source != NULL)
		handle_put(resource->scanout_source);
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
	kern_memset(&information, 0, sizeof(information));
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
	kern_memset(&response, 0, sizeof(response));
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
	uintptr_t argument,
	unsigned placed)
{
	struct drv_gpu_device *device;
	struct gpu_blob_create request;
	struct gpu_blob_create_placed extended;
	size_t bytes;
	unsigned constrained;
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

	/* Both layouts share the old prefix while only the new command reads placement conditions. */
	kern_memset(&extended, 0, sizeof(extended));
	bytes = sizeof(request);
	if (placed != 0U)
		bytes = sizeof(extended);

	/* Copying only the selected request preserves the legacy caller's exact buffer boundary. */
	error = copyin(argument, &extended, bytes);
	if (error != 0)
		return error;

	/* Refuses unknown layouts before normalizing the legacy backend request. */
	request = extended.blob;
	if (request.version != GPU_ABI_VERSION || request.size != bytes)
		return EINVAL;

	/* The ordinary allocator always receives its unchanged legacy request header. */
	request.size = sizeof(request);

	/* Unknown physical conditions or nonzero reserved fields cannot reach a backend. */
	if (extended.placement.reserved != 0U ||
	    (extended.placement.flags & ~(GPU_PLACEMENT_DMA32 | GPU_PLACEMENT_CONTIGUOUS | GPU_PLACEMENT_COHERENT)) != 0U)
		return EINVAL;

	/* A physical base alignment is meaningful only when it is a power of two. */
	if (extended.placement.alignment != 0U &&
	    (extended.placement.alignment & (extended.placement.alignment - 1U)) != 0U)
		return EINVAL;

	/* Any nontrivial requirement needs the backend that can inspect or arrange actual backing. */
	constrained = 0U;
	if (extended.placement.flags != 0U ||
	    extended.placement.max_dma_address != 0U || extended.placement.alignment > 1U)
		constrained = 1U;

	/* Missing physical support is rejected before allocating even a common resource wrapper. */
	if (constrained != 0U && device->ops->blob_create_placed == NULL)
		return ENOTSUP;

	/* Blob flags select mapping and independent sharing without changing their wire values. */
	if ((request.flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0)
		return EINVAL;

	/* A cross-device renderer export must also be explicitly shareable. */
	if ((request.flags & GPU_BLOB_CROSS_DEVICE) != 0 &&
	    (request.flags & GPU_BLOB_SHAREABLE) == 0)
		return EINVAL;

	/* Sharing flags require the complete backend capability. */
	if ((request.flags & (GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0 &&
	    (device->ops->capabilities & (GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE)) == 0)
		return EOPNOTSUPP;

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
	if (constrained != 0U) {
		/* The placed backend sees the reserved identity and every immutable condition together. */
		extended.blob.handle = request.handle;
		error = device->ops->blob_create_placed(
			device->private_data,
			session->backend,
			&extended,
			&object,
			&resource_id);
	} else {
		/* Unconstrained requests retain exactly the pre-existing allocation behavior. */
		error = device->ops->blob_create(
			device->private_data,
			session->backend,
			&request,
			&object,
			&resource_id);
	}

	/* Failed callbacks retain no backend object, including a rejected physical allocation. */
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
	extended.blob = request;
	extended.blob.size = (uint32_t)bytes;
	error = copyout(&extended, argument, bytes);
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
	unsigned long irq;
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

	/* A rejected or uncertain send still counts as native ownership for the final stop proof. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	session->native_submitted = 1U;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

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
		kern_memset(&view, 0, sizeof(view));
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
	kern_memset(&request, 0, sizeof(request));
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
		kern_memset(&request.query, 0, sizeof(request.query));
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

		/* Enumeration provides geometry; validation permits driver frequency selection with refresh zero. */
		if (request.mode.operation == GPU_DISPLAY_MODE_ENUMERATE) {
			request.mode.width = 0;
			request.mode.height = 0;
			request.mode.refresh_millihz = 0;
		} else if (request.mode.index != 0 ||
		    request.mode.width == 0 ||
		    request.mode.height == 0) {
			return EINVAL;
		}

		/* Lets the backend check the complete mode against this open's current display generation. */
		error = ops->mode(device->private_data, session->backend, &request.mode);
		if (error != 0)
			return error;

		/* A driver-selected validation result must resolve to a complete usable mode. */
		if (request.mode.operation == GPU_DISPLAY_MODE_VALIDATE &&
		    (request.mode.width == 0U ||
		     request.mode.height == 0U ||
		     request.mode.refresh_millihz == 0U))
			return EIO;
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
			kern_memset(&rollback, 0, sizeof(rollback));
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
		    (request.present.flags != GPU_DISPLAY_PRESENT_FIFO &&
		     request.present.flags != (GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB)))
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

		/* Blob presentation consumes only imported or exported allocation resources. */
		if ((request.present.flags & GPU_DISPLAY_PRESENT_BLOB) != 0) {
			/* Shared scanout requires allocation ownership in the blob namespace. */
			if (resource->kind != GPU_RESOURCE_BLOB && resource->kind != GPU_RESOURCE_SCANOUT)
				return EINVAL;
		} else {
			/* Legacy copied presentation keeps its ordinary storage contract. */
			if (resource->kind != GPU_RESOURCE_STORAGE)
				return EINVAL;
		}

		/* An offset is validated before subtracting it from the retained extent. */
		if (request.present.offset > resource->bytes)
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

/* Returns the immutable type-specific finalizer used by GPU capabilities. */
static const struct kernel_handle_ops *
gpu_shared_operations(void)
{
	static const struct kernel_handle_ops operations = {
		gpu_shared_release,
		NULL
	};

	/* Succeeded: every GPU capability uses the same verified payload contract. */
	return &operations;
}

/* Validates allocation authority without interpreting a userspace image or buffer protocol. */
static int
gpu_allocation_validate(
	struct gpu_resource *resource,
	const struct gpu_allocation_descriptor *allocation)
{
	uint32_t index;

	/* Only the exact versioned metadata envelope has a defined import representation. */
	if (allocation->version != GPU_ABI_VERSION || allocation->size != sizeof(*allocation))
		return EINVAL;

	/* The exported capability owns the whole resource; K supplies its device identity. */
	if (allocation->device_id != 0U ||
	    resource->kind != GPU_RESOURCE_BLOB ||
	    allocation->allocation_bytes != resource->bytes)
		return EINVAL;

	/* A nonzero userspace schema identifies the immutable metadata contract. */
	if (allocation->schema == 0U || allocation->metadata_bytes > GPU_ALLOCATION_METADATA_MAX)
		return EINVAL;

	/* Unused bytes cannot carry uninitialized or ambiguous protocol data across the fd. */
	for (index = allocation->metadata_bytes; index < GPU_ALLOCATION_METADATA_MAX; index++) {
		/* The entire returned envelope has a deterministic representation. */
		if (allocation->metadata[index] != 0U)
			return EINVAL;
	}

	/* Succeeded: the bounded opaque description grants exactly this allocation's ownership. */
	return 0;
}

/* Exports an allocation with userspace metadata independent from native scanout eligibility. */
static int
gpu_allocation_export_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_allocation_export request;
	struct gpu_shared_resource *shared;
	struct gpu_resource *resource;
	struct drv_gpu_device *device;
	struct kernel_handle *handle;
	const struct kernel_handle_ops *operations;
	int error;

	/* Export grants mutable allocation authority and requires a writable source open. */
	if (session->writable == 0U)
		return EACCES;

	/* Older image-only sharing backends do not receive a null image contract. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_ALLOCATION_SHARE) == 0U)
		return EOPNOTSUPP;

	/* Snapshot the complete envelope before resolving any resource or userspace metadata. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* A new export has no preexisting fd and accepts only explicit inheritance policies. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.fd != -1 ||
	    (request.flags & ~(GPU_HANDLE_CLOEXEC | GPU_HANDLE_CLOFORK)) != 0U)
		return EINVAL;

	/* Foreign resource identifiers cannot grant storage from another open. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* Validate the opaque envelope without imposing display geometry or linear tiling. */
	error = gpu_allocation_validate(resource, &request.allocation);
	if (error != 0)
		return error;

	/* The independent payload carries only the allocation authority and immutable metadata. */
	shared = kern_calloc(1U, sizeof(*shared));
	if (shared == NULL)
		return ENOMEM;

	/* Device withdrawal cannot retire backend state while this capability remains alive. */
	error = gpu_shared_hold_device(device);
	if (error != 0) {
		kern_free(shared);
		return error;
	}

	/* From here the payload finalizer owns the device reference on every failure path. */
	request.allocation.device_id = device->identity;
	shared->device = device;
	shared->allocation = request.allocation;
	error = device->ops->share->export_resource(
		device->private_data,
		session->backend,
		resource->object,
		NULL,
		&shared->object);
	if (error != 0) {
		gpu_shared_release(shared);
		return error;
	}

	/* Successful backend export must return exactly one releasable ownership reference. */
	if (shared->object == NULL) {
		gpu_shared_release(shared);
		return EIO;
	}

	/* The typed kernel handle consumes the payload only after wrapper allocation succeeds. */
	operations = gpu_shared_operations();
	error = handle_create(KERNEL_HANDLE_DRIVER, operations, shared, &handle);
	if (error != 0) {
		gpu_shared_release(shared);
		return error;
	}

	/* Copyout and fd reservation commit use the same ownership protocol as image export. */
	error = gpu_export_install(handle, &request, sizeof(request), request.flags, &request.fd, argument);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Succeeded: the installed fd retains allocation ownership independently of its source. */
	return 0;
}

/* Imports allocation authority while keeping its opaque userspace description immutable. */
static int
gpu_allocation_import_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_allocation_import request;
	struct gpu_allocation_descriptor empty;
	struct gpu_shared_resource *shared;
	struct gpu_resource *resource;
	struct drv_gpu_device *device;
	struct kernel_handle *handle;
	const struct kernel_handle_ops *operations;
	void *object;
	uint32_t resource_id;
	int different;
	int error;

	/* Imported allocation ownership requires a writable destination namespace. */
	if (session->writable == 0U)
		return EACCES;

	/* Only backends advertising allocation-only sharing can attach this capability. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_ALLOCATION_SHARE) == 0U)
		return EOPNOTSUPP;

	/* The input fd is the sole authority; all resource and metadata fields are outputs. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Reject stale outputs or reserved values before retaining a foreign kernel object. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.fd < 0 ||
	    request.flags != 0U ||
	    request.handle != 0U ||
	    request.resource_id != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* Receiver-supplied metadata can never replace the stored sender contract. */
	kern_memset(&empty, 0, sizeof(empty));
	different = kern_memcmp(&request.allocation, &empty, sizeof(empty));
	if (different != 0)
		return EINVAL;

	/* A strong typed reference remains valid across concurrent close of the input fd. */
	handle = handle_fd_get(request.fd, KERNEL_HANDLE_DRIVER);
	if (handle == NULL)
		return EINVAL;

	/* Verify the payload implementation before dereferencing its private state. */
	operations = gpu_shared_operations();
	if (handle->ops != operations) {
		handle_put(handle);
		return EINVAL;
	}

	/* Image capabilities and allocation-only capabilities have distinct immutable envelopes. */
	shared = handle->object;
	if (shared->allocation.version == 0U) {
		handle_put(handle);
		return EINVAL;
	}

	/* Renderer exportability does not imply cross-GPU import compatibility. */
	if (shared->device != device) {
		handle_put(handle);
		return EXDEV;
	}

	/* Reserve a fresh session-local identity before attaching host resource ownership. */
	error = gpu_resource_reserve(session, shared->allocation.allocation_bytes, &resource);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Backend import borrows the capability and returns a separately owned alias. */
	object = NULL;
	resource_id = 0U;
	error = device->ops->share->import_resource(
		device->private_data,
		session->backend,
		shared->object,
		&object,
		&resource_id);
	if (error != 0) {
		kern_free(resource);
		handle_put(handle);
		return error;
	}

	/* An incomplete success cannot publish a resource that lacks identity or cleanup ownership. */
	if (object == NULL || resource_id == 0U) {
		/* A malformed backend result may still have acquired an independently releasable alias. */
		if (object != NULL)
			device->ops->resource_destroy(device->private_data, session->backend, object);

		/* Neither the unpublished resource nor the lookup reference escapes this failure. */
		kern_free(resource);
		handle_put(handle);
		return EIO;
	}

	/* The new backend alias keeps the allocation alive after the lookup reference is released. */
	request.handle = resource->handle;
	request.resource_id = resource_id;
	request.allocation = shared->allocation;
	handle_put(handle);

	/* A failed output copy retires only this unpublished destination alias. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		device->ops->resource_destroy(device->private_data, session->backend, object);
		kern_free(resource);
		return error;
	}

	/* Publish the alias only after the caller has a complete immutable import result. */
	resource->object = object;
	resource->kind = GPU_RESOURCE_BLOB;
	resource->next = session->resources;
	session->resources = resource;

	/* The session retains each imported allocation until explicit destruction or final close. */
	session->resource_count++;

	/* Succeeded: this independent context owns the original allocation without a display contract. */
	return 0;
}

/* Queries node roles independently of rendering or display ownership. */
static int
gpu_device_query_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_device_info request;
	struct drv_gpu_device *device;
	int error;

	/* This read-only query accepts no stale output or private driver data. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Input fields identify the ABI; output fields must start empty. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.device_id != 0U ||
	    request.companion_id != 0U ||
	    request.roles != 0U ||
	    request.flags != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* Ordinary existing drivers derive roles from the operations they actually implement. */
	device = session->device;
	kern_memset(&request, 0, sizeof(request));
	if (device->ops->scanout != NULL) {
		error = device->ops->scanout->query_device(
			device->private_data,
			session->backend,
			&request);
		if (error != 0)
			return error;
	} else {
		if (device->ops->capabilities & GPU_CAP_COMMAND)
			request.roles |= GPU_DEVICE_RENDER;

		if (device->ops->capabilities & GPU_CAP_DISPLAY)
			request.roles |= GPU_DEVICE_DISPLAY;
	}

	/* K owns stable device identity; the companion is only a driver-provided selection hint. */
	if ((request.roles & ~(GPU_DEVICE_RENDER | GPU_DEVICE_DISPLAY)) != 0U ||
	    request.flags != 0U ||
	    request.reserved != 0U)
		return EIO;

	/* Publishes the backend roles with the core's immutable node identity. */
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.device_id = device->identity;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: display-only nodes participate in discovery without creating a fake Vulkan GPU. */
	return 0;
}

/* Returns display generation constraints before allocation and import route selection. */
static int
gpu_constraints_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_scanout_constraints request;
	struct drv_gpu_device *device;
	uint32_t identifier;
	uint64_t generation;
	int error;

	/* An unimplemented constraints callback cannot imply arbitrary DMA compatibility. */
	device = session->device;
	if (!(device->ops->capabilities & GPU_CAP_DISPLAY) ||
	    device->ops->scanout == NULL ||
	    device->ops->scanout->constraints == NULL)
		return EOPNOTSUPP;

	/* Accepts one existing display generation without caller-provided output. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Nonzero result fields would conceal an incomplete backend response. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.display_id == 0U ||
	    request.generation == 0U ||
	    request.flags != 0U ||
	    request.formats != 0U ||
	    request.stride_alignment != 0U ||
	    request.offset_alignment != 0U ||
	    request.placement != 0U ||
	    request.max_dma_address != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* The backend checks the requested generation and supplies only supported routes. */
	identifier = request.display_id;
	generation = request.generation;
	error = device->ops->scanout->constraints(
		device->private_data,
		session->backend,
		&request);
	if (error != 0)
		return error;

	/* A backend cannot change the requested generation or invent allocation flags. */
	if (request.display_id != identifier ||
	    request.generation != generation ||
	    request.reserved != 0U ||
	    (request.flags & ~(GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY | GPU_SCANOUT_FOREIGN)) != 0U ||
	    (request.placement & ~(GPU_PLACEMENT_DMA32 | GPU_PLACEMENT_CONTIGUOUS | GPU_PLACEMENT_COHERENT)) != 0U)
		return EIO;

	/* Packed formats are the only image descriptions accepted by the present ABI. */
	if ((request.formats & ~(GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888)) != 0U)
		return EIO;

	/* A copy route requires storage allocation and upload before ordinary presentation. */
	if (request.flags & GPU_SCANOUT_COPY) {
		if ((device->ops->capabilities & (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER)) !=
		    (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER))
			return EIO;
	}

	/* Nonzero alignments must describe arithmetic that applications can actually satisfy. */
	if ((request.stride_alignment & (request.stride_alignment - 1U)) != 0U || (request.offset_alignment & (request.offset_alignment - 1U)) != 0U)
		return EIO;

	/* Foreign sharing requires the driver to validate the original physical backing. */
	if ((request.flags & GPU_SCANOUT_FOREIGN) &&
	    (!(request.flags & GPU_SCANOUT_SHARED) ||
	     device->ops->scanout->import_image == NULL))
		return EIO;

	/* Publishes constraints only after validating the backend's complete contract. */
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: this generation's requirements are explicit, without promising every import works. */
	return 0;
}

/* Validates foreign physical backing through the destination display driver's native import. */
static int
gpu_scanout_import(
	struct gpu_session *session,
	struct gpu_shared_resource *shared,
	void **object)
{
	struct drv_gpu_scanout_backing backing;
	struct drv_gpu_device *device;
	int error;

	/* No renderer-local resource identifier can establish cross-device DMA reachability. */
	device = session->device;
	*object = NULL;
	if (!(device->ops->capabilities & GPU_CAP_DISPLAY) ||
	    device->ops->scanout == NULL ||
	    device->ops->scanout->import_image == NULL ||
	    shared->device->ops->share == NULL ||
	    shared->device->ops->share->get_scanout_backing == NULL)
		return EOPNOTSUPP;

	/* The lookup reference keeps the source anchor and its physical page array alive. */
	kern_memset(&backing, 0, sizeof(backing));
	error = shared->device->ops->share->get_scanout_backing(
		shared->device->private_data,
		shared->object,
		&backing);
	if (error != 0)
		return error;

	/* Rejects incomplete or overflowing page extents before the driver examines them. */
	if (backing.pages == NULL ||
	    backing.page_count == 0U ||
	    backing.page_bytes == 0U ||
	    (backing.page_bytes & (backing.page_bytes - 1U)) != 0U ||
	    backing.bytes < shared->image.allocation_bytes ||
	    backing.page_count > UINT64_MAX / backing.page_bytes ||
	    backing.bytes > backing.page_count * backing.page_bytes)
		return EIO;

	/* Destination DMA/address/cache/format checks decide whether this exact allocation imports. */
	error = device->ops->scanout->import_image(
		device->private_data,
		session->backend,
		&shared->image,
		&backing,
		object);
	if (error != 0)
		return error;

	/* Succeeded: the core retains the source handle until after native resource destruction. */
	return 0;
}

/* Validates the complete linear image extent before an export reaches the backend. */
static int
gpu_image_validate(
	struct gpu_resource *resource,
	const struct gpu_image_descriptor *image)
{
	uint64_t bytes;

	/* The first sharing contract has one fixed public metadata layout. */
	if (image->version != GPU_ABI_VERSION || image->size != sizeof(*image))
		return EINVAL;

	/* K supplies the device identity and accepts no reserved metadata. */
	if (image->device_id != 0U || image->reserved != 0U)
		return EINVAL;

	/* Shared allocations describe exactly the resource being exported. */
	if (resource->kind != GPU_RESOURCE_BLOB || image->allocation_bytes != resource->bytes)
		return EINVAL;

	/* The initial scanout contract accepts complete linear packed-color images. */
	if (image->tiling != GPU_IMAGE_LINEAR || image->memory_type >= 32U)
		return EOPNOTSUPP;

	/* Vulkan 1.0 image usages form the supported nonempty description domain. */
	if (image->usage == 0U || (image->usage & ~255U) != 0U)
		return EINVAL;

	/* Width multiplication must fit before the row pitch is compared. */
	if (image->width == 0U ||
	    image->width > UINT32_MAX / 4U ||
	    image->height == 0U)
		return EINVAL;

	/* Only the two published four-byte channel orders have a defined scanout encoding. */
	if (image->format != GPU_PIXEL_RGBA8888 && image->format != GPU_PIXEL_BGRA8888)
		return EOPNOTSUPP;

	/* Every visible row and its padding belong to the same aligned allocation. */
	if (image->stride < image->width * 4U || (image->stride & 3U) != 0U)
		return EINVAL;

	/* The native single-plane scanout offset is representable in 32 bits. */
	if (image->offset > UINT32_MAX || image->offset > resource->bytes)
		return EINVAL;

	/* Bounds the full image only after the offset subtraction is safe. */
	bytes = (uint64_t)image->stride * image->height;
	if (bytes > resource->bytes - image->offset)
		return EINVAL;

	/* Succeeded: no described pixel can address storage outside this allocation. */
	return 0;
}

/* Retains backend withdrawal independently from any open session. */
static int
gpu_shared_hold_device(
	struct drv_gpu_device *device)
{
	unsigned long irq;

	/* New capabilities cannot escape after device withdrawal starts. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (device->online == 0U) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENODEV;
	}

	/* Counter exhaustion must not turn live capabilities into a removable device. */
	if (device->shares == UINT_MAX) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EOVERFLOW;
	}

	/* Each capability payload keeps the borrowed backend valid through its finalizer. */
	device->shares++;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: unregister must wait for this exported ownership lifetime. */
	return 0;
}

/* Releases the backend allocation before dropping the device withdrawal barrier. */
static void
gpu_shared_release(
	void *object)
{
	struct gpu_shared_resource *shared;
	struct drv_gpu_device *device;
	unsigned long irq;

	/* The handle finalizer owns its payload and may run without an exporting process. */
	shared = object;
	device = shared->device;

	/* Failed export acquisition owns no backend reference to release. */
	if (shared->object != NULL)
		device->ops->share->release(device->private_data, shared->object);

	/* No payload pointer remains live once the device can be detached. */
	kern_free(shared);

	/* Zero permits unregister only after the backend finalizer has completely returned. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->shares--;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: neither this capability nor its callback borrows the device. */
	return;
}

/* Publishes the fd number before committing its reserved table slot. */
static int
gpu_export_install(
	struct kernel_handle *handle,
	void *request,
	size_t bytes,
	uint32_t policy,
	int32_t *fd,
	uintptr_t argument)
{
	struct filedesc_reservation reservation;
	struct fd_object object;
	struct thread *thread;
	unsigned flags;
	int descriptor;
	int error;

	/* A syscall must have a process-owned descriptor table. */
	thread = thread_current();
	if (thread == NULL ||
	    thread->proc == NULL ||
	    thread->proc->fd == NULL)
		return ESRCH;

	/* Public export flags map explicitly to descriptor-local inheritance policy. */
	flags = 0U;
	if ((policy & GPU_HANDLE_CLOEXEC) != 0U)
		flags |= FILEDESC_CLOEXEC;

	/* Fork inheritance remains independent from close-on-exec behavior. */
	if ((policy & GPU_HANDLE_CLOFORK) != 0U)
		flags |= FILEDESC_CLOFORK;

	/* Reservation prevents another thread from closing and reusing a failed export slot. */
	kern_memset(&reservation, 0, sizeof(reservation));
	error = filedesc_reserve_many(thread->proc->fd, 1U, flags, &reservation);
	if (error != 0)
		return error;

	/* Copy failure withdraws only the reservation, never a later unrelated descriptor. */
	*fd = reservation.slots[0];
	error = copyout(request, argument, bytes);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		return error;
	}

	/* Successful commit consumes the caller's one owned handle reference. */
	kern_memset(&object, 0, sizeof(object));
	object.type = FD_OBJECT_HANDLE;
	object.data.handle = handle;
	error = filedesc_commit_objects(&reservation, &object, &descriptor);
	if (error != 0) {
		filedesc_abort_reserved(&reservation);
		return error;
	}

	/* Succeeded: the published fd owns the capability after a complete output copy. */
	return 0;
}

/* Exports one allocation without retaining or exposing its generating GPU session. */
static int
gpu_export_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_resource_export request;
	struct gpu_shared_resource *shared;
	struct gpu_resource *resource;
	struct drv_gpu_device *device;
	struct kernel_handle *handle;
	const struct kernel_handle_ops *operations;
	int error;

	/* Export grants mutation-capable allocation authority and requires a writable source. */
	if (session->writable == 0U)
		return EACCES;

	/* No unimplemented sharing callback receives an export request. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_SHARE) == 0U)
		return EOPNOTSUPP;

	/* Snapshot the complete request before resolving its resource identity. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Output fd and public flag bits have a single unambiguous initial form. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.fd != -1 ||
	    (request.flags & ~(GPU_HANDLE_CLOEXEC | GPU_HANDLE_CLOFORK)) != 0U)
		return EINVAL;

	/* A globally unique but foreign handle does not grant source ownership. */
	error = gpu_resource_lookup(session, request.handle, &resource);
	if (error != 0)
		return error;

	/* Validate and stamp immutable metadata before the backend retains it. */
	error = gpu_image_validate(resource, &request.image);
	if (error != 0)
		return error;

	/* Allocation metadata is independent from the process-local source handle. */
	request.image.device_id = device->identity;
	shared = kern_calloc(1U, sizeof(*shared));
	if (shared == NULL)
		return ENOMEM;

	/* Device withdrawal must wait even after the exporting open is gone. */
	error = gpu_shared_hold_device(device);
	if (error != 0) {
		kern_free(shared);
		return error;
	}

	/* The payload finalizer now owns the device hold on every remaining path. */
	shared->device = device;
	shared->image = request.image;
	error = device->ops->share->export_resource(
		device->private_data,
		session->backend,
		resource->object,
		&request.image,
		&shared->object);
	if (error != 0) {
		gpu_shared_release(shared);
		return error;
	}

	/* Successful backend acquisition must provide a terminally releasable object. */
	if (shared->object == NULL) {
		gpu_shared_release(shared);
		return EIO;
	}

	/* A generic typed handle takes the payload only after wrapper allocation succeeds. */
	operations = gpu_shared_operations();
	error = handle_create(KERNEL_HANDLE_DRIVER, operations, shared, &handle);
	if (error != 0) {
		gpu_shared_release(shared);
		return error;
	}

	/* Installation consumes ownership only after complete copyout and reservation commit. */
	error = gpu_export_install(handle, &request, sizeof(request), request.flags, &request.fd, argument);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Succeeded: userspace owns an independently transferable allocation fd. */
	return 0;
}

/* Imports an allocation capability into a separately owned GPU context. */
static int
gpu_import_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_resource_import request;
	struct gpu_image_descriptor empty;
	struct gpu_shared_resource *shared;
	struct gpu_resource *resource;
	struct drv_gpu_device *device;
	struct kernel_handle *handle;
	const struct kernel_handle_ops *operations;
	void *object;
	uint32_t resource_id;
	int different;
	int foreign;
	int error;

	/* Imported resources belong to a writable destination open. */
	if (session->writable == 0U)
		return EACCES;

	/* The destination must implement the whole independent sharing contract. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_SHARE) == 0U &&
	    (device->ops->scanout == NULL || device->ops->scanout->import_image == NULL))
		return EOPNOTSUPP;

	/* The descriptor is the only input beyond the versioned request header. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Reject stale outputs and unknown flags before acquiring a foreign capability. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.fd < 0 ||
	    (request.flags & ~GPU_IMPORT_SCANOUT) != 0U ||
	    request.handle != 0U ||
	    request.resource_id != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* Never trust receiver-supplied descriptions in place of the exported immutable metadata. */
	kern_memset(&empty, 0, sizeof(empty));
	different = kern_memcmp(&request.image, &empty, sizeof(empty));
	if (different != 0)
		return EINVAL;

	/* Type-checked lookup returns a strong reference independent from concurrent close. */
	handle = handle_fd_get(request.fd, KERNEL_HANDLE_DRIVER);
	if (handle == NULL)
		return EINVAL;

	/* The registered type's payload contract must match before any cast is used. */
	operations = gpu_shared_operations();
	if (handle->ops != operations) {
		handle_put(handle);
		return EINVAL;
	}

	/* A capability from another GPU cannot name resources in this renderer namespace. */
	shared = handle->object;

	/* An allocation-only capability cannot acquire a forged linear scanout description. */
	if (shared->image.version == 0U) {
		handle_put(handle);
		return EINVAL;
	}

	/* Device identity remains authoritative even when another process supplies the descriptor. */
	foreign = shared->device != device;
	if (foreign != 0 && !(request.flags & GPU_IMPORT_SCANOUT)) {
		handle_put(handle);
		return EXDEV;
	}

	/* Reserve a new session-local identity before acquiring the backend attachment. */
	error = gpu_resource_reserve(session, shared->image.allocation_bytes, &resource);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Import creates its own owned resource without consuming the transferable fd. */
	object = NULL;
	resource_id = 0U;
	if (foreign != 0) {
		/* Foreign scanout requires explicit backing and destination DMA validation. */
		error = gpu_scanout_import(session, shared, &object);
	} else if (device->ops->share != NULL) {
		/* An ordinary same-device alias remains usable by its native renderer. */
		error = device->ops->share->import_resource(
			device->private_data,
			session->backend,
			shared->object,
			&object,
			&resource_id);
	} else {
		error = EOPNOTSUPP;
	}
	if (error != 0) {
		kern_free(resource);
		handle_put(handle);
		return error;
	}

	/* A complete imported resource needs both cleanup ownership and a native identity. */
	if (object == NULL || (foreign == 0 && resource_id == 0U)) {
		/* Even malformed successful backend output must release any ownership it created. */
		if (object != NULL)
			device->ops->resource_destroy(device->private_data, session->backend, object);

		/* Neither the unpublished slot nor the borrowed capability escapes this failure. */
		kern_free(resource);
		handle_put(handle);
		return EIO;
	}

	/* The destination's new resource now retains its own allocation lifetime. */
	request.handle = resource->handle;
	request.resource_id = resource_id;
	request.image = shared->image;
	if (foreign != 0)
		resource->scanout_source = handle;
	else
		handle_put(handle);

	/* Failed output copy destroys only the unpublished destination attachment. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		device->ops->resource_destroy(device->private_data, session->backend, object);
		if (resource->scanout_source != NULL)
			handle_put(resource->scanout_source);
		kern_free(resource);
		return error;
	}

	/* Publication makes the new handle reachable only through this destination session. */
	resource->object = object;
	resource->kind = foreign != 0 ? GPU_RESOURCE_SCANOUT : GPU_RESOURCE_BLOB;
	resource->next = session->resources;
	session->resources = resource;

	/* This session's resource count includes each independently owned import. */
	session->resource_count++;

	/* Succeeded: the caller can bind or present the same allocation in its own context. */
	return 0;
}

/* Reserves every kernel owner before userspace can submit native GPU work. */
static int
gpu_job_reserve_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_job_reserve request;
	struct drv_gpu_completion *completion;
	struct drv_gpu_device *device;
	struct kernel_handle *handle;
	void *reservation;
	unsigned long irq;
	int error;

	/* A job changes device state and requires the backend's strict completion contract. */
	device = session->device;
	if (session->writable == 0U)
		return EACCES;

	/* The immutable registered capability includes all three reservation callbacks. */
	if ((device->ops->capabilities & GPU_CAP_JOB) == 0U)
		return EOPNOTSUPP;

	/* Decode fixed-width framing before resolving optional descriptor authority. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* The common header carries a backend-defined execution domain without hardware-specific limits. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.flags != 0U || request.reserved != 0U ||
	    request.sequence != 0U || request.fd < -1)
		return EINVAL;

	/* An omitted payload has no generation; a supplied payload must identify one. */
	if ((request.fd == -1 && request.generation != 0U) ||
	    (request.fd != -1 && request.generation == 0U))
		return EINVAL;

	/* Autonomous policy supervision must exist before native submission becomes possible. */
	error = gpu_monitor_start(device);
	if (error != 0)
		return error;

	/* A strong reference covers concurrent descriptor close and payload retirement. */
	handle = NULL;
	if (request.fd != -1) {
		error = gpu_fence_get(session, request.fd, 0U, &handle);
		if (error != 0)
			return error;
	}

	/* Identity allocation precedes all backend callback ownership. */
	error = gpu_handle_allocate(&request.sequence);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Completion storage is bounded independently from the backend queue's slots. */
	error = gpu_completion_reserve(session, request.sequence, &completion);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Setup pins the slot and excludes actions racing the early output identity. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	completion->observers++;
	completion->job_action = 1U;
	completion->job_state = GPU_JOB_RESERVED;
	completion->job_deadline = gpu_monitor_deadline(CONFIG_GPU_JOB_RESERVATION_MS);
	completion->backend_owned = 1U;
	waitq_wake_all(&device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Fence ownership is established before a watchdog could complete the reservation. */
	error = gpu_job_bind(session, completion, handle, request.generation);
	if (error == 0)
		error = copyout(&request, argument, sizeof(request));

	/* No undisclosed identity is allowed to retain a backend callback. */
	reservation = NULL;
	if (error == 0) {
		error = device->ops->jobs->reserve(
			device->private_data, session->backend, request.timeline,
			completion, &reservation);
	}

	/* Rejection guarantees that the backend retains no future callback obligation. */
	if (error != 0) {
		gpu_job_unbind(session, request.sequence);
		gpu_completion_discard(completion);
	} else {
		/* Immediate expiry may already have made the record terminal; preserve that state. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		completion->job_reservation = reservation;
		completion->reservation_pending = 1U;
		completion->unpublished = 0U;

		spin_unlock_irqrestore(&gpu_registry_lock, irq);
	}

	/* Release setup serialization before dropping the pin that prevents slot reuse. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	completion->job_action = 0U;
	waitq_wake_all(&session->device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Setup cleanup keeps the original failure visible after releasing both independent holds. */
	gpu_completion_observer_leave(completion, 0U, 0U);
	handle_put(handle);
	if (error != 0)
		return error;

	/* Succeeded: supervision already covers the producer's native submission interval. */
	return 0;
}

/* Commits native acceptance or cancels definite nonacceptance without losing ownership. */
static int
gpu_job_action_ioctl(
	struct gpu_session *session,
	unsigned long command,
	uintptr_t argument)
{
	struct gpu_job_action request;
	struct drv_gpu_completion *completion;
	struct drv_gpu_device *device;
	void *reservation;
	enum gpu_job_state state;
	unsigned fault;
	unsigned lost;
	unsigned long irq;
	int cancel_error;
	int error;

	/* Only the writable originating open may act on its private sequence. */
	device = session->device;
	if (session->writable == 0U)
		return EACCES;

	/* Capability validation at registration guarantees the complete callback set. */
	if ((device->ops->capabilities & GPU_CAP_JOB) == 0U)
		return EOPNOTSUPP;

	/* Neither action returns a new identity or transfers descriptor ownership. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Unknown flags cannot silently turn native uncertainty into rollback. */
	if (request.version != GPU_ABI_VERSION || request.size != sizeof(request) ||
	    request.sequence == 0U || request.reserved != 0U ||
	    (request.flags & ~GPU_JOB_CANCEL_FAULT) != 0U ||
	    (command == GPU_JOB_COMMIT && request.flags != 0U))
		return EINVAL;

	/* A concurrent terminal consume cannot recycle the slot during a backend action. */
	error = gpu_job_observe(session, request.sequence, &completion);
	if (error != 0)
		return error;

	/* Only this action may change the reservation; callbacks may still finish it. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	state = completion->job_state;
	reservation = completion->job_reservation;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Fault reporting may end committed work, while rollback requires an unpublished reservation. */
	fault = request.flags & GPU_JOB_CANCEL_FAULT;
	lost = 0U;
	error = 0;
	if (state != GPU_JOB_RESERVED &&
	    !(command == GPU_JOB_CANCEL && fault != 0U && state == GPU_JOB_COMMITTED))
		error = EALREADY;

	/* Admission precedes publication so even immediate host completion signals the exact generation. */
	if (error == 0 && command == GPU_JOB_COMMIT) {
		error = gpu_job_admit(session, request.sequence);
		if (error == 0) {
			error = device->ops->jobs->commit(
				device->private_data, session->backend, reservation, completion);
		}

		/* Native acceptance is already possible; a failed commit must never become ordinary rollback. */
		if (error != 0) {
			cancel_error = device->ops->jobs->cancel(
				device->private_data, session->backend, reservation, completion, 1U);
			if (cancel_error == 0)
				lost = 1U;
		}
	}

	/* Definite nonacceptance withdraws callback ownership before clearing the payload binding. */
	if (error == 0 && command == GPU_JOB_CANCEL) {
		error = device->ops->jobs->cancel(
			device->private_data, session->backend, reservation, completion, fault);
		if (error == 0 && fault == 0U) {
			gpu_job_unbind(session, request.sequence);
			gpu_completion_discard(completion);
			drv_gpu_capacity_changed(device);
		}

		/* A retained uncertain callback means this context can no longer prove its own work. */
		if (error == 0 && fault != 0U)
			lost = 1U;
	}

	/* Native uncertainty loses this context first; the common stop policy escalates from here. */
	if (lost != 0U) {
		irq = spin_lock_irqsave(&gpu_registry_lock);

		gpu_session_fail_locked(session, EIO, 1U);

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* Fence aliases and this descriptor observe the local loss at once. */
		poll_notify();
	}

	/* Release action serialization while the observer still prevents callback-slot reuse. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	completion->job_action = 0U;
	waitq_wake_all(&session->device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Dropping the action observer does not hide an unsuccessful commit or cancellation. */
	gpu_completion_observer_leave(completion, 0U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: native work remains supervised, or definite rejection released its reservation. */
	return 0;
}

/* Binds an optional shared payload before the backend owns a callback. */
static int
gpu_job_bind(
	struct gpu_session *session,
	struct drv_gpu_completion *completion,
	struct kernel_handle *handle,
	uint64_t generation)
{
	struct gpu_fence_binding *binding;
	unsigned index;
	unsigned long irq;
	int error;

	/* The registry serializes device health and exact producer-generation ownership. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (session->device->online == 0U ||
	    session->device->error != 0 ||
	    session->error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENODEV;
	}

	/* Sequence-only jobs still reserve real backend supervision. */
	if (handle == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return 0;
	}

	/* Payload producer slots are bounded independently from external descriptor aliases. */
	binding = NULL;
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* Only an empty producer slot may receive another independent generation. */
		if (session->fences[index].handle == NULL) {
			binding = &session->fences[index];
			break;
		}
	}

	/* Rejection occurs before changing the payload's generation or producer. */
	if (binding == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EAGAIN;
	}

	/* A reserved generation is not yet eligible for an in-kernel GPU dependency. */
	error = drv_gpu_fence_bind(handle, generation, session);
	if (error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return error;
	}

	/* The callback owns this reference even if every exported descriptor is closed. */
	handle_get(handle);
	binding->handle = handle;
	binding->generation = generation;
	binding->sequence = completion->sequence;
	binding->job = 1U;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: the exact reservation owns the optional payload's terminal transition. */
	return 0;
}

/* Rolls back producer ownership only after the backend rejected or canceled its callback. */
static void
gpu_job_unbind(
	struct gpu_session *session,
	uint64_t sequence)
{
	struct gpu_fence_binding *binding;
	struct kernel_handle *handle;
	unsigned index;
	unsigned long irq;

	/* A prior global fault may already have terminated the payload without releasing this metadata. */
	handle = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* Select only the optional payload owned by this exact job identity. */
		binding = &session->fences[index];
		if (binding->handle == NULL || binding->job == 0U || binding->sequence != sequence)
			continue;

		/* Preserve pending state after definite nonacceptance; terminal errors stay terminal. */
		handle = binding->handle;
		(void)drv_gpu_fence_unbind(handle, binding->generation, session);
		kern_memset(binding, 0, sizeof(*binding));
		break;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Last-reference cleanup and descriptor notification run outside the registry lock. */
	handle_put(handle);
	poll_notify();

	/* Succeeded: no discarded reservation retains producer authority. */
	return;
}

/* Marks the supervised generation as accepted before its prepared marker is published. */
static int
gpu_job_admit(
	struct gpu_session *session,
	uint64_t sequence)
{
	struct drv_gpu_completion *completion;
	struct gpu_fence_binding *binding;
	unsigned index;
	unsigned long irq;
	int error;

	/* The caller's observer pins the exact record throughout this admission transition. */
	completion = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* The retained observer excludes identity reuse during admission. */
		if (session->completions[index].sequence == sequence) {
			completion = &session->completions[index];
			break;
		}
	}

	/* Expiry or another terminal callback cannot be reclassified as accepted success. */
	if (completion == NULL ||
	    completion->completed != 0U ||
	    completion->job_state != GPU_JOB_RESERVED ||
	    session->device->error != 0 ||
	    session->error != 0 ||
	    session->device->online == 0U) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENODEV;
	}

	/* Optional sequence-only jobs require no descriptor payload transition. */
	error = 0;
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* Select only the optional payload owned by this exact job identity. */
		binding = &session->fences[index];
		if (binding->handle == NULL || binding->job == 0U || binding->sequence != sequence)
			continue;

		/* Only this owner and generation may become an in-kernel GPU dependency. */
		error = drv_gpu_fence_admit(binding->handle, binding->generation, session);
		break;
	}

	/* Admission is irreversible except through a terminal error once native work may exist. */
	if (error == 0) {
		completion->job_state = GPU_JOB_COMMITTED;
		completion->reservation_pending = 0U;
		completion->job_deadline = gpu_monitor_deadline(CONFIG_GPU_JOB_EXECUTION_MS);
		session->native_submitted = 1U;
		waitq_wake_all(&session->device->monitor_waitq);
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* A refused payload transition must never publish the prepared marker. */
	if (error != 0)
		return error;

	/* Succeeded: the same watchdog now supervises an accepted job's completion. */
	return 0;
}

/* Pins one job action independently from ordinary session resource-table admission. */
static int
gpu_job_observe(
	struct gpu_session *session,
	uint64_t sequence,
	struct drv_gpu_completion **result)
{
	struct drv_gpu_completion *completion;
	unsigned index;
	unsigned long irq;
	int error;

	/* Foreign, discarded and consumed sequences provide no action authority. */
	*result = NULL;
	completion = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* Ordinary notifications and consumed jobs provide no reservation authority. */
		if (session->completions[index].listed != 0U &&
		    session->completions[index].sequence == sequence &&
		    session->completions[index].job_state != GPU_JOB_NONE) {
			completion = &session->completions[index];
			break;
		}
	}

	/* Lookup failure changes neither pending work nor the completion ledger. */
	if (completion == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENOENT;
	}

	/* Setup and another action must finish before this caller may change the token. */
	if (completion->job_action != 0U) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EBUSY;
	}

	/* Completed sequences remain observable through WAIT, not actionable as fresh reservations. */
	if (completion->completed != 0U) {
		error = completion->error != 0 ? completion->error : EALREADY;
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return error;
	}

	/* A successful lookup holds the slot until all backend pointer use has ended. */
	if (completion->observers == UINT_MAX) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EOVERFLOW;
	}

	/* This action owns one slot pin until its final backend pointer use ends. */
	completion->observers++;
	completion->job_action = 1U;
	*result = completion;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: callback completion and consumption cannot recycle this action's token. */
	return 0;
}

/* Retains one command before exposing its sequence or entering the backend. */
static int
gpu_submit_ioctl(
	struct gpu_session *session,
	uintptr_t argument,
	uint64_t *sequence)
{
	struct gpu_command_submit request;
	struct drv_gpu_completion *completion;
	struct drv_gpu_device *device;
	void *command;
	uintptr_t pointer;
	unsigned long irq;
	int error;

	/* Queued command submission requires write authority and advertised support. */
	device = session->device;
	if (session->writable == 0U)
		return EACCES;

	/* Old backends retain their existing synchronous command contract. */
	if ((device->ops->capabilities & GPU_CAP_NOTIFICATION) == 0U)
		return EOPNOTSUPP;

	/* Copies the complete fixed-width request before resolving user memory. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects incompatible framing and caller-supplied output identities. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.reserved != 0U ||
	    request.sequence != 0U)
		return EINVAL;

	/* Only explicit context fencing gives the timeline field a meaning. */
	if ((request.flags & ~GPU_COMMAND_CONTEXT_FENCE) != 0U)
		return EINVAL;

	/* Without an explicit completion-domain flag, the unused selector remains zero. */
	if (request.flags == 0U && request.timeline != 0U)
		return EINVAL;

	/* Empty byte streams only insert a selected context marker. */
	if (request.bytes == 0U && request.flags == 0U)
		return EINVAL;

	/* Empty markers retain no unused user pointer. */
	if (request.bytes == 0U && request.address != 0U)
		return EINVAL;

	/* Bounds every independently retained command copy. */
	if (request.bytes > GPU_COMMAND_MAX)
		return E2BIG;

	/* Allocates command storage before any completion record becomes visible. */
	command = NULL;
	if (request.bytes != 0U) {
		error = gpu_user_range(request.address, request.bytes, &pointer);
		if (error != 0)
			return error;

		/* The backend never receives a mutable userspace command address. */
		command = kern_malloc(request.bytes);
		if (command == NULL)
			return ENOMEM;

		/* Copies the entire bounded stream before publishing any work. */
		error = copyin(pointer, command, request.bytes);
		if (error != 0) {
			kern_free(command);
			return error;
		}
	}

	/* Global identities prevent a foreign open's sequence from aliasing this one. */
	error = gpu_handle_allocate(&request.sequence);
	if (error != 0) {
		kern_free(command);
		return error;
	}

	/* Bounded embedded records retain their open through final backend drain. */
	error = gpu_completion_reserve(session, request.sequence, &completion);
	if (error != 0) {
		kern_free(command);
		return error;
	}

	/* A dependency wrapper retains the completion even if another thread consumes it immediately. */
	if (sequence != NULL) {
		irq = spin_lock_irqsave(&gpu_registry_lock);

		completion->observers++;

		spin_unlock_irqrestore(&gpu_registry_lock, irq);
	}

	/* A failed output copy must never submit an operation without its observable identity. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		gpu_completion_discard(completion);

		/* The dependency wrapper retains its own observer independently from the public record. */
		if (sequence != NULL)
			gpu_completion_observer_leave(completion, 0U, 0U);

		/* No native owner received the staged command bytes. */
		kern_free(command);
		return error;
	}

	/* Pins publication against a concurrent consumer and a local stop request. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	error = gpu_session_error_locked(session);
	if (error == 0) {
		completion->backend_owned = 1U;
		completion->unpublished = 0U;
		completion->job_action = 1U;
		completion->observers++;
		session->native_submitted = 1U;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* A local failure preceding publication must not submit untracked work afterward. */
	if (error != 0) {
		gpu_completion_discard(completion);

		/* The dependency wrapper retains its own observer independently from the public record. */
		if (sequence != NULL)
			gpu_completion_observer_leave(completion, 0U, 0U);

		/* No native owner received the staged command bytes. */
		kern_free(command);
		return error;
	}

	/* Success transfers exactly one terminal callback obligation to the backend. */
	error = device->ops->commands->submit(
		device->private_data,
		session->backend,
		command,
		request.bytes,
		request.flags,
		request.timeline,
		completion);
	kern_free(command);

	/* Local stop waits until the native submission callback has returned. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	completion->job_action = 0U;
	waitq_wake_all(&device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Refusal retains no callback and withdraws the publication pin separately. */
	if (error != 0) {
		gpu_completion_discard(completion);
		gpu_completion_observer_leave(completion, 0U, 0U);

		/* A dependency caller may own one additional setup observer. */
		if (sequence != NULL)
			gpu_completion_observer_leave(completion, 0U, 0U);

		/* Failure retains no callback or publication observer. */
		return error;
	}

	/* The accepted backend or result ledger now retains the publication independently. */
	gpu_completion_observer_leave(completion, 0U, 0U);

	/* Internal dependency wrappers use the trusted accepted identity, never a second user-memory read. */
	if (sequence != NULL)
		*sequence = request.sequence;

	/* Succeeded: the independent command and its selected notification are pending. */
	return 0;
}

/* Observes one retained completion without taking ordinary session admission. */
static int
gpu_wait_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_command_wait request;
	struct drv_gpu_completion *completion;
	unsigned consume;
	int error;

	/* Completion observation follows the open description's original read authority. */
	if (session->readable == 0U)
		return EACCES;

	/* Backends without queued commands cannot own observable sequence records. */
	if ((session->device->ops->capabilities & GPU_CAP_NOTIFICATION) == 0U)
		return EOPNOTSUPP;

	/* Copies the fixed-width input before lookup or any possible sleep. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Unknown observation flags cannot silently discard a completion. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.sequence == 0U ||
	    request.status != 0U ||
	    (request.flags & ~GPU_WAIT_CONSUME) != 0U)
		return EINVAL;

	/* Pins the record through a terminal observation and its following user copy. */
	error = gpu_completion_observe(session, &request, &completion);
	if (error != 0)
		return error;

	/* A transport outcome is separate from ioctl validation and Vulkan execution results. */
	request.status = (uint32_t)completion->error;
	error = copyout(&request, argument, sizeof(request));
	consume = 0U;
	if (error == 0 && (request.flags & GPU_WAIT_CONSUME) != 0U)
		consume = 1U;

	/* Failed output copy leaves the terminal result readable for a later retry. */
	gpu_completion_observer_leave(completion, consume, request.flags & GPU_WAIT_CONSUME);
	if (error != 0)
		return error;

	/* Succeeded: userspace received the exact terminal transport status. */
	return 0;
}

/* Reserves a bounded slot without recycling an identity still held by an observer. */
static int
gpu_completion_reserve(
	struct gpu_session *session,
	uint64_t sequence,
	struct drv_gpu_completion **result)
{
	struct drv_gpu_completion *completion;
	unsigned index;
	unsigned long irq;
	int error;

	/* Failure leaves no backend callback owner. */
	*result = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* Failed opens cannot acquire a new completion owner while stop is pending. */
	error = gpu_session_error_locked(session);
	if (error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return error;
	}

	/* Finds only slots whose prior operation and all observers have retired. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		completion = &session->completions[index];
		if (completion->sequence != 0U)
			continue;

		/* Publishes the owning session before a backend may complete immediately. */
		kern_memset(completion, 0, sizeof(*completion));
		completion->session = session;
		completion->sequence = sequence;
		completion->listed = 1U;
		completion->unpublished = 1U;
		*result = completion;
		break;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Userspace can consume completed records before retrying bounded saturation. */
	if (*result == NULL)
		return EAGAIN;

	/* Succeeded: this immutable sequence owns one bounded completion slot. */
	return 0;
}

/* Withdraws a record when the backend never accepted its callback obligation. */
static void
gpu_completion_discard(
	struct drv_gpu_completion *completion)
{
	unsigned long irq;

	/* Early output publication can race another thread observing the same sequence. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	completion->listed = 0U;
	completion->unpublished = 1U;
	completion->backend_owned = 0U;
	completion->job_deadline = 0U;
	completion->error = ECANCELED;
	completion->completed = 1U;
	waitq_wake_all(&completion->session->completion_waitq);

	/* Existing observers retain the embedded slot until their lookup/copy finishes. */
	if (completion->observers == 0U)
		completion->sequence = 0U;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: no future lookup can confuse rejection with accepted GPU work. */
	return;
}

/* Waits for a selected record while another thread may submit through the same fd. */
static int
gpu_completion_observe(
	struct gpu_session *session,
	const struct gpu_command_wait *request,
	struct drv_gpu_completion **result)
{
	struct drv_gpu_completion *completion;
	uint64_t deadline;
	uint64_t observed;
	unsigned index;
	unsigned long irq;
	int error;

	/* Converts timeout before taking the short record lock. */
	*result = NULL;
	error = gpu_completion_deadline(request->timeout_ns, &deadline);
	if (error != 0)
		return error;

	/* Resolves exactly one still-listed identity within this open. */
	completion = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		/* Consumed or rejected records cannot be re-observed by identity. */
		if (session->completions[index].listed == 0U)
			continue;

		/* Sequence identities are immutable until every observer has left. */
		if (session->completions[index].sequence == request->sequence) {
			completion = &session->completions[index];
			break;
		}
	}

	/* A foreign or already consumed sequence names no pending operation. */
	if (completion == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENOENT;
	}

	/* Prevents observer-count overflow before retaining a recyclable slot. */
	if (completion->observers == UINT_MAX) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EOVERFLOW;
	}

	/* Observers pin the record independently from ordinary ioctl admission. */
	completion->observers++;
	error = 0;
	while (completion->completed == 0U) {
		/* Zero timeout observes readiness without changing request ownership. */
		if (request->timeout_ns == 0U) {
			error = EAGAIN;
			break;
		}

		/* Withdrawal ends observation even if hardware cleanup is still retained. */
		if (session->device->online == 0U) {
			error = ENODEV;
			break;
		}

		/* Sequence-based sleeping closes the completion-before-registration race. */
		observed = waitq_sequence(&session->completion_waitq);
		error = waitq_sleep(
			&session->completion_waitq,
			&gpu_registry_lock,
			observed,
			deadline,
			WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN)
			break;
	}

	/* A wakeup racing registration requires only another terminal-state check. */
	if (error == EAGAIN && completion->completed != 0U)
		error = 0;

	/* Reserves terminal consumption through copyout without blocking other submitters. */
	if (error == 0 && (request->flags & GPU_WAIT_CONSUME) != 0U) {
		/* A parallel consumer must not retire this result twice. */
		if (completion->consuming != 0U) {
			error = EAGAIN;
		} else {
			completion->consuming = 1U;
		}
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Interruption and timeout keep the request itself intact. */
	if (error != 0) {
		gpu_completion_observer_leave(completion, 0U, 0U);
		return error;
	}

	/* Succeeded: the immutable terminal result stays pinned for user copyout. */
	*result = completion;
	return 0;
}

/* Releases an observer and retires a successfully copied terminal result when requested. */
static void
gpu_completion_observer_leave(
	struct drv_gpu_completion *completion,
	unsigned consume,
	unsigned reserved)
{
	unsigned long irq;

	/* Consuming removes lookup authority before the last observer releases the slot. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (consume != 0U)
		completion->listed = 0U;

	/* All observers end their copyout before the sequence can be reused. */
	if (reserved != 0U)
		completion->consuming = 0U;

	/* Every observer retains its own slot hold independently from consumption. */
	completion->observers--;
	if (completion->listed == 0U &&
	    completion->observers == 0U &&
	    completion->backend_owned == 0U) {
		completion->sequence = 0U;

		/* Failed try-reserve rollback must not wake its own retry on every refusal. */
		if (completion->unpublished == 0U)
			gpu_capacity_changed_locked(completion->session->device);
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Readiness may have changed after the final terminal record was consumed. */
	poll_notify();

	/* Succeeded: remaining observers or the terminal queue retain the old record. */
	return;
}

/* Converts a relative userspace interval to a checked guest-clock deadline. */
static int
gpu_completion_deadline(
	uint64_t timeout,
	uint64_t *deadline)
{
	uint64_t ticks;
	uint64_t now;
	uint64_t quantum;

	/* Zero is query-only and the maximum value requests an unbounded observation. */
	*deadline = 0U;
	if (timeout == 0U || timeout == UINT64_MAX)
		return 0;

	/* Rounds upward so a finite wait never expires before its requested interval. */
	quantum = KERN_NSEC_PER_SEC / KERN_CLOCK_HZ;
	ticks = timeout / quantum;
	if (timeout % quantum != 0U)
		ticks++;

	/* Deadline addition must not wrap into an unrelated early tick. */
	now = sched_ticks();
	if (ticks > UINT64_MAX - now)
		return EOVERFLOW;

	/* Succeeded: a nonzero absolute deadline bounds the requested observation. */
	*deadline = now + ticks;
	return 0;
}

/* Dispatches pure payload operations without reserving a GPU resource table. */
static int
gpu_fence_ioctl(
	struct gpu_session *session,
	unsigned long command,
	uintptr_t argument)
{
	int error;

	/* Optional support is authoritative at the registered device boundary. */
	if ((session->device->ops->capabilities & GPU_CAP_FENCE) == 0U)
		return EOPNOTSUPP;

	/* Observers need read access; ownership and state changes need write access. */
	if (command == GPU_FENCE_QUERY || command == GPU_FENCE_WAIT) {
		if (session->readable == 0U)
			return EACCES;
	} else if (session->writable == 0U) {
		return EACCES;
	}

	/* Each fixed-width request owns its complete validation and failure cleanup. */
	if (command == GPU_FENCE_CREATE)
		error = gpu_fence_create_ioctl(session, argument);
	else if (command == GPU_FENCE_BIND)
		error = gpu_fence_bind_ioctl(session, argument);
	else
		error = gpu_fence_state_ioctl(session, command, argument);

	/* A rejected payload operation preserves its specific authority or state error. */
	if (error != 0)
		return error;

	/* Succeeded: the selected independent payload operation has finished. */
	return 0;
}

/* Creates a generation-one fence and installs its descriptor transactionally. */
static int
gpu_fence_create_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_fence_create request;
	struct kernel_handle *handle;
	int error;

	/* Copy the exact creation ABI before allocating any independent ownership. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Unused and output fields cannot encode hidden policy or an existing descriptor. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.fd != -1 ||
	    request.signaled > 1U ||
	    request.reserved != 0U ||
	    request.generation != 0U ||
	    (request.flags & ~(GPU_HANDLE_CLOEXEC | GPU_HANDLE_CLOFORK)) != 0U)
		return EINVAL;

	/* The immutable identity outlives the exporting open without retaining its session. */
	error = drv_gpu_fence_create(session->device->identity, request.signaled, &handle);
	if (error != 0)
		return error;

	/* Reservation and successful copyout precede descriptor publication. */
	request.generation = 1U;
	error = gpu_export_install(handle, &request, sizeof(request), request.flags, &request.fd, argument);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Succeeded: the descriptor table owns the initial payload reference. */
	return 0;
}

/* Resolves exact typed authority and optionally enforces renderer device identity. */
static int
gpu_fence_get(
	struct gpu_session *session,
	int fd,
	unsigned foreign,
	struct kernel_handle **result)
{
	struct kernel_handle *handle;
	int error;

	/* Descriptor lookup pins the payload across concurrent close and fd reuse. */
	*result = NULL;
	handle = handle_fd_get(fd, KERNEL_HANDLE_DRIVER);
	if (handle == NULL)
		return EBADF;

	/* Vulkan OPAQUE_FD state access never imports an incompatible GPU's payload. */
	if (foreign == 0U) {
		error = drv_gpu_fence_device(handle, session->device->identity);
		if (error != 0) {
			handle_put(handle);
			return error;
		}
	}

	*result = handle;

	/* Succeeded: the caller owns one independent capability reference. */
	return 0;
}

/* Reads, waits, resets or signals one exact payload generation. */
static int
gpu_fence_state_ioctl(
	struct gpu_session *session,
	unsigned long command,
	uintptr_t argument)
{
	struct gpu_fence_state request;
	struct drv_gpu_fence_state state;
	struct kernel_handle *handle;
	uint64_t deadline;
	unsigned immediate;
	int error;

	/* Fixed-width framing is checked before descriptor lookup. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Only QUERY may select the current generation by supplying zero. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.flags != 0U ||
	    request.state != 0U ||
	    request.error < 0)
		return EINVAL;

	/* State mutation and waiting must name an exact work generation. */
	if (command != GPU_FENCE_QUERY && request.generation == 0U)
		return EINVAL;

	/* Only WAIT uses a timeout and only SIGNAL supplies an input error status. */
	if (command != GPU_FENCE_WAIT && request.timeout_ns != 0U)
		return EINVAL;

	/* Error observations are output-only for all operations except producer signal. */
	if (command != GPU_FENCE_SIGNAL && request.error != 0)
		return EINVAL;

	/* Validate finite deadline arithmetic before resolving payload ownership. */
	error = gpu_completion_deadline(request.timeout_ns, &deadline);
	if (error != 0)
		return error;

	/* The kernel is authoritative for same-device OPAQUE_FD compatibility. */
	error = gpu_fence_get(session, request.fd, 0U, &handle);
	if (error != 0)
		return error;

	/* Zero timeout selects a nonblocking observation of the same retained generation. */
	immediate = 0U;
	if (request.timeout_ns == 0U)
		immediate = 1U;

	/* State operations lock only the independent payload, never a renderer controller. */
	if (command == GPU_FENCE_QUERY) {
		error = drv_gpu_fence_query(handle, request.generation, &state);
	} else if (command == GPU_FENCE_WAIT) {
		error = drv_gpu_fence_wait(handle, request.generation, deadline, immediate, &state);
	} else if (command == GPU_FENCE_RESET) {
		error = drv_gpu_fence_reset(handle, request.generation, &state);
	} else {
		/* Possession of an imported fd does not acquire the submitting open's authority. */
		error = gpu_fence_binding_end(session, handle, request.generation, 0U, request.error);
		if (error == 0)
			error = drv_gpu_fence_query(handle, request.generation, &state);
	}

	/* Payload cleanup cannot invoke a destructor while holding the registry or resource-table lock. */
	handle_put(handle);

	/* Timeout, interruption and stale generation never consume descriptor ownership. */
	if (error != 0)
		return error;

	/* Return an initialized immutable observation rather than echoing output fields. */
	request.generation = state.generation;
	request.state = state.state;
	request.error = state.error;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: every alias observes the same generation and completion state. */
	return 0;
}

/* Acquires or rolls back one producer reservation before native work submission. */
static int
gpu_fence_bind_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_fence_bind request;
	struct kernel_handle *handle;
	int error;

	/* Copy the bounded binding ABI before resolving any shared payload. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* A rollback never changes the sequence association of accepted work. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.generation == 0U ||
	    (request.flags & ~GPU_FENCE_BIND_RELEASE) != 0U)
		return EINVAL;

	/* RELEASE applies only to a pre-submit reservation and supplies no sequence. */
	if (request.flags != 0U && request.sequence != 0U)
		return EINVAL;

	/* Binding authority is restricted to the device namespace of this open. */
	error = gpu_fence_get(session, request.fd, 0U, &handle);
	if (error != 0)
		return error;

	/* The session retains an additional reference only after ownership validation succeeds. */
	if (request.flags == GPU_FENCE_BIND_RELEASE)
		error = gpu_fence_binding_end(session, handle, request.generation, 1U, 0);
	else
		error = gpu_fence_binding_add(session, handle, request.generation, request.sequence);

	/* Payload cleanup cannot invoke a destructor while holding the registry or resource-table lock. */
	handle_put(handle);

	/* A refused producer reservation never changes the caller's fd ownership. */
	if (error != 0)
		return error;

	/* Succeeded: the requested producer reservation or rollback is complete. */
	return 0;
}

/* Reserves bounded session ownership under the same lock used by terminal notification. */
static int
gpu_fence_binding_add(
	struct gpu_session *session,
	struct kernel_handle *handle,
	uint64_t generation,
	uint64_t sequence)
{
	struct gpu_fence_binding *binding;
	struct drv_gpu_completion *completion;
	unsigned index;
	unsigned long irq;
	int error;

	/* Every session owns bounded independent slots for pending shared completion payloads. */
	binding = NULL;
	completion = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* Failed or withdrawn devices cannot acquire new producer authority. */
	if (session->device->online == 0U || session->device->error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENODEV;
	}

	/* A notification sequence belongs only to this exact open description. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		if (session->fences[index].handle == NULL && binding == NULL)
			binding = &session->fences[index];

		/* Identity is globally unique; only retained listed records may be associated. */
		if (sequence != 0U &&
		    session->completions[index].listed != 0U &&
		    session->completions[index].sequence == sequence)
			completion = &session->completions[index];
	}

	/* Unknown or consumed work cannot be used as error propagation authority. */
	if (sequence != 0U && completion == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return ENOENT;
	}

	/* Saturation is explicit and leaves the payload unbound. */
	if (binding == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EAGAIN;
	}

	/* Lock order is registry then independent fence; global poll notification runs after both are released. */
	error = drv_gpu_fence_bind(handle, generation, session);
	if (error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return error;
	}

	/* The extra reference retains the payload after the producer closes its exported fd. */
	handle_get(handle);
	binding->handle = handle;
	binding->generation = generation;
	binding->sequence = sequence;
	binding->job = 0U;

	/* An already failed transport record is immediately terminal without claiming GPU success. */
	if (completion != NULL &&
	    completion->completed != 0U &&
	    completion->error != 0)
		(void)drv_gpu_fence_signal_deferred(handle, generation, session, completion->error);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Descriptor readiness notification follows release of the GPU registry lock. */
	poll_notify();

	/* Succeeded: final session close owns any unfinished generation's terminal error. */
	return 0;
}

/* Ends only this open's exact producer reservation and drops its independent payload reference. */
static int
gpu_fence_binding_end(
	struct gpu_session *session,
	struct kernel_handle *handle,
	uint64_t generation,
	unsigned release,
	int status)
{
	struct gpu_fence_binding *binding;
	unsigned index;
	unsigned long irq;
	int error;

	/* Shared aliases cannot signal another open's pending work. */
	binding = NULL;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		if (session->fences[index].handle == handle && session->fences[index].generation == generation) {
			binding = &session->fences[index];
			break;
		}
	}

	/* A stale or foreign owner has no authority even if its descriptor remains valid. */
	if (binding == NULL) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EPERM;
	}

	/* Userspace cannot forge completion or release an autonomous job's generation. */
	if (binding->job != 0U) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EPERM;
	}

	/* Explicit rollback is reserved for work that never acquired a notification sequence. */
	if (release != 0U && binding->sequence != 0U) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return EBUSY;
	}

	/* Rollback preserves pending state; verified completion publishes success or error. */
	if (release != 0U)
		error = drv_gpu_fence_unbind(handle, generation, session);
	else
		error = drv_gpu_fence_signal_deferred(handle, generation, session, status);

	/* Keep cleanup ownership when a mismatched or already terminal generation refuses mutation. */
	if (error != 0) {
		spin_unlock_irqrestore(&gpu_registry_lock, irq);
		return error;
	}

	binding->handle = NULL;
	binding->generation = 0U;
	binding->sequence = 0U;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Payload cleanup cannot invoke a destructor while holding the registry or resource-table lock. */
	handle_put(handle);

	/* Descriptor readiness notification follows release of the GPU registry lock. */
	poll_notify();

	/* Succeeded: the producer no longer retains or controls this generation. */
	return 0;
}

/* Ends unfinished producer authority while native callbacks and their session storage remain retained. */
static void
gpu_fence_retire(
	struct gpu_session *session)
{
	struct kernel_handle *handle;
	unsigned index;
	unsigned long irq;

	/* Final close excludes new ioctls; the registry lock serializes binding retirement with late callbacks. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		irq = spin_lock_irqsave(&gpu_registry_lock);

		handle = session->fences[index].handle;
		if (handle != NULL) {
			(void)drv_gpu_fence_signal_deferred(handle, session->fences[index].generation, session, ENODEV);
			session->fences[index].handle = NULL;
		}

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* The last metadata release runs without holding any GPU framework lock. */
		handle_put(handle);
	}

	/* Descriptor readiness notification follows release of the GPU registry lock. */
	poll_notify();

	/* Succeeded: descriptor aliases outlive this session with a terminal state. */
	return;
}

/* Waits on an independent capability before reserving any controller or resource-table ownership. */
static int
gpu_dependency_wait(
	struct gpu_session *session,
	int fd,
	uint64_t generation,
	unsigned foreign)
{
	struct kernel_handle *handle;
	struct drv_gpu_fence_state state;
	int error;

	/* An absent dependency carries neither descriptor nor work-generation authority. */
	if (fd == -1) {
		if (generation != 0U)
			return EINVAL;

		/* Succeeded: no producer dependency precedes this operation. */
		return 0;
	}

	/* Every real dependency identifies an exact generation. */
	if (fd < 0 || generation == 0U)
		return EINVAL;

	/* A native display may consume a renderer's fence from its approved companion GPU. */
	error = gpu_fence_get(session, fd, foreign, &handle);
	if (error != 0)
		return error;

	/* Only driver-admitted work may enter this interruptible producer-supervised wait. */
	error = drv_gpu_fence_wait_work(handle, generation, &state);
	/* Payload cleanup cannot invoke a destructor while holding the registry or resource-table lock. */
	handle_put(handle);
	if (error != 0)
		return error;

	/* Failed work is never presented or submitted as if the producer succeeded. */
	if (state.state == DRV_GPU_FENCE_ERROR)
		return state.error;

	/* Succeeded: the exact prerequisite generation completed successfully. */
	return 0;
}

/* Coordinates explicit wait and signal capabilities around ordinary GPU submission. */
static int
gpu_dependency_ioctl(
	struct gpu_session *session,
	unsigned long command,
	uintptr_t argument)
{
	struct gpu_command_submit_sync submit;
	struct gpu_display_present_sync present;
	struct kernel_handle *signal;
	struct gpu_fence_binding *binding;
	struct drv_gpu_completion *completion;
	uint64_t wait_generation;
	uint64_t signal_generation;
	uint64_t sequence;
	int wait_fd;
	int signal_fd;
	unsigned display;
	unsigned index;
	unsigned long irq;
	int error;
	int finish;

	/* Both dependency variants require write authority and the core fence contract. */
	if (session->writable == 0U)
		return EACCES;

	/* A backend opts into the standard shared completion capability explicitly. */
	if ((session->device->ops->capabilities & GPU_CAP_FENCE) == 0U)
		return EOPNOTSUPP;

	/* Snapshot the dependency metadata before any potentially blocking operation. */
	display = 0U;
	if (command == GPU_DISPLAY_PRESENT_SYNC)
		display = 1U;

	/* Decode the exact nested ABI selected by the ioctl number. */
	if (display != 0U) {
		error = copyin(argument, &present, sizeof(present));
		if (error != 0)
			return error;

		/* The nested display ABI remains unchanged. */
		if (present.present.version != GPU_ABI_VERSION || present.present.size != sizeof(present.present))
			return EINVAL;

		/* Only fixed-width capability identifiers survive the pending producer wait. */
		wait_fd = present.wait_fd;
		signal_fd = present.signal_fd;
		wait_generation = present.wait_generation;
		signal_generation = present.signal_generation;
	} else {
		error = copyin(argument, &submit, sizeof(submit));
		if (error != 0)
			return error;

		/* The nested command ABI retains its existing sequence and framing contract. */
		if (submit.command.version != GPU_ABI_VERSION || submit.command.size != sizeof(submit.command))
			return EINVAL;

		/* Driver callbacks never receive process-local fence descriptor numbers. */
		wait_fd = submit.wait_fd;
		signal_fd = submit.signal_fd;
		wait_generation = submit.wait_generation;
		signal_generation = submit.signal_generation;
	}

	/* Reject malformed signal authority before waiting on an unrelated producer. */
	if ((signal_fd == -1 && signal_generation != 0U) ||
	    signal_fd < -1 ||
	    (signal_fd >= 0 && signal_generation == 0U))
		return EINVAL;

	/* Pin the signal capability before waiting, so concurrent close and fd reuse cannot change its identity. */
	signal = NULL;
	if (signal_fd >= 0) {
		error = gpu_fence_get(session, signal_fd, 0U, &signal);
		if (error != 0)
			return error;
	}

	/* The wait holds neither the renderer controller nor the shared GPU fd admission gate. */
	error = gpu_dependency_wait(session, wait_fd, wait_generation, display);
	if (error != 0) {
		handle_put(signal);
		return error;
	}

	/* Retained payload identity remains stable even when its former descriptor number is reused. */
	if (signal != NULL) {
		/* Reserve before hardware submission so final close can always terminate accepted work. */
		error = gpu_fence_binding_add(session, signal, signal_generation, 0U);
		if (error != 0) {
			handle_put(signal);
			return error;
		}
	}

	/* Ordinary operations retain the same validated resource-table ownership as their unsynchronized ABI. */
	error = gpu_session_enter(session);
	if (error != 0) {
		if (signal != NULL)
			(void)gpu_fence_binding_end(session, signal, signal_generation, 1U, 0);

		/* Failure before submission leaves the payload unsignaled and unbound. */
		handle_put(signal);
		return error;
	}

	/* Each original operation still owns its framing, resource and backend validation. */
	sequence = 0U;
	if (display != 0U)
		error = gpu_display_ioctl(session, GPU_DISPLAY_PRESENT, argument);
	else if (signal != NULL)
		error = gpu_submit_ioctl(session, argument, &sequence);
	else
		error = gpu_submit_ioctl(session, argument, NULL);

	/* Other operations may resume after the backend has finished using this session resource table. */
	gpu_session_leave(session);

	/* Display success is the existing scanout-selection boundary, not physical panel vblank. */
	if (signal != NULL && display != 0U) {
		finish = gpu_fence_binding_end(session, signal, signal_generation, 0U, error);
		if (error == 0)
			error = finish;
	}

	/* A rejected command owns no future notification and may roll back its pre-submit reservation. */
	if (signal != NULL &&
	    display == 0U &&
	    error != 0)
		(void)gpu_fence_binding_end(session, signal, signal_generation, 1U, 0);

	/* Accepted asynchronous work retains only failure propagation until authoritative U signal. */
	if (signal != NULL &&
	    display == 0U &&
	    error == 0) {
		irq = spin_lock_irqsave(&gpu_registry_lock);

		/* Locate the exact retained producer binding and wrapper-pinned completion record. */
		binding = NULL;
		completion = NULL;
		for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
			/* Payload and generation together identify this operation's producer authority. */
			if (session->fences[index].handle == signal && session->fences[index].generation == signal_generation)
				binding = &session->fences[index];

			/* A fast callback may have already completed before the submitting syscall returned. */
			if (session->completions[index].sequence == sequence)
				completion = &session->completions[index];
		}

		/* Updating association is atomic with transport error notification. */
		if (binding != NULL) {
			binding->sequence = sequence;

			/* Immediate failure is propagated before the wrapper's pinned observation ends. */
			if (completion != NULL &&
			    completion->completed != 0U &&
			    completion->error != 0)
				(void)drv_gpu_fence_signal_deferred(signal, signal_generation, session, completion->error);
		}

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* The wrapper hold ends only after its failure association has observed the immutable record. */
		gpu_completion_observer_leave(completion, 0U, 0U);

		/* Any immediate error is visible to descriptor observers after registry release. */
		poll_notify();
	}

	/* The temporary syscall reference ends after accepted work has acquired its own hold. */
	handle_put(signal);

	/* Failed submission preserves its transport or prerequisite error for the caller. */
	if (error != 0)
		return error;

	/* Succeeded: accepted work retains its required completion ownership. */
	return 0;
}

/* Advances capacity observation without wrapping into an earlier caller's snapshot. */
static void
gpu_capacity_changed_locked(
	struct drv_gpu_device *device)
{
	/* Exhaustion stays explicit instead of creating a missed wake through aliasing. */
	if (device->capacity_sequence == UINT64_MAX) {
		device->capacity_overflow = 1U;
	} else {
		device->capacity_sequence++;
	}

	/* Capacity and graceful stop both depend on actual callback retirement. */
	waitq_wake_all(&device->capacity_waitq);
	waitq_wake_all(&device->monitor_waitq);

	/* Succeeded: no observer can mistake this transition for its old condition. */
	return;
}

/* Returns the current open's health under the common registry lock. */
static int
gpu_session_error_locked(
	struct gpu_session *session)
{
	/* Withdrawal invalidates every backend operation even on a healthy context. */
	if (session->device->online == 0U)
		return ENODEV;

	/* Transport loss affects all opens; local loss affects only this one. */
	if (session->device->error != 0)
		return session->device->error;

	/* A fresh open is the only way to obtain a new context after local loss. */
	if (session->error != 0)
		return session->error;

	/* Succeeded: this open may admit new operations. */
	return 0;
}

/*
 * Publishes local terminal errors while preserving every backend callback hold.
 *
 * A hard failure -- an expired deadline, an uncertain native fault, or device
 * loss -- ends every backend-owned generation at once.  A graceful producer
 * close (hard zero) keeps each committed job supervised so its real completion
 * still signals success, and ends only the reserved and ordinary command
 * generations the departed producer can no longer publish.  The stop interval
 * is armed later, when the monitor actually begins the stop handshake, so a job
 * running inside its own execution deadline is never escalated to a device fault.
 */
static void
gpu_session_fail_locked(
	struct gpu_session *session,
	int error,
	unsigned hard)
{
	struct drv_gpu_completion *completion;
	struct gpu_fence_binding *binding;
	unsigned kept;
	unsigned inner;
	unsigned index;

	/* The first failure fixes the context's public result and requests its stop. */
	if (session->error == 0) {
		session->error = error;
		session->stop_state = GPU_STOP_REQUESTED;
	}

	/* Logical completion cannot release storage still reachable by native callbacks. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		completion = &session->completions[index];
		if (completion->sequence == 0U ||
		    completion->completed != 0U ||
		    completion->backend_owned == 0U)
			continue;

		/* A graceful close leaves a committed job to complete with its real result. */
		if (hard == 0U && completion->job_state == GPU_JOB_COMMITTED)
			continue;

		/* The failed generation becomes observable even before native work retires. */
		completion->completed = 1U;
		completion->error = session->error;
		completion->job_deadline = 0U;

		/* Terminal supervision cannot be recommitted while native ownership is retained. */
		if (completion->job_state != GPU_JOB_NONE)
			completion->job_state = GPU_JOB_FINISHED;
	}

	/* Imported aliases end their producer authority unless a kept committed job still owns them. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		binding = &session->fences[index];
		if (binding->handle == NULL)
			continue;

		/* A graceful close leaves a still-running committed job to signal its own fence. */
		kept = 0U;
		if (hard == 0U) {
			for (inner = 0U; inner < GPU_SUBMIT_MAX; inner++) {
				completion = &session->completions[inner];
				if (completion->sequence == binding->sequence &&
				    completion->backend_owned != 0U &&
				    completion->completed == 0U &&
				    completion->job_state == GPU_JOB_COMMITTED)
					kept = 1U;
			}
		}

		/* A committed producer keeps its fence until real completion publishes the result. */
		if (kept != 0U)
			continue;

		/* Deferred publication avoids invoking the poll registry under this lock. */
		(void)drv_gpu_fence_signal_deferred(
			binding->handle,
			binding->generation,
			session,
			session->error);
	}

	/* Admission, record observers and reclamation loops all see the same error. */
	waitq_wake_all(&session->admission_waitq);
	waitq_wake_all(&session->completion_waitq);
	gpu_capacity_changed_locked(session->device);

	/* Succeeded: only backend retirement can now end the retained DMA ownership. */
	return;
}

/* Converts a finite administrator interval to a saturating monotonic deadline. */
static uint64_t
gpu_monitor_deadline(
	uint64_t milliseconds)
{
	uint64_t ticks;
	uint64_t now;

	/* Round upward so clock granularity cannot shorten the selected policy. */
	ticks = milliseconds * KERN_CLOCK_HZ / 1000U;
	if (milliseconds * KERN_CLOCK_HZ % 1000U != 0U)
		ticks++;

	/* Configuration always remains finite, including near clock exhaustion. */
	now = sched_ticks();
	if (now > UINT64_MAX - ticks)
		return UINT64_MAX;

	/* Succeeded: this absolute deadline is independent from userspace scheduling. */
	return now + ticks;
}

/* Starts the common supervisor before the first native reservation is returned. */
static int
gpu_monitor_start(
	struct drv_gpu_device *device)
{
	struct thread *worker;
	int error;

	/* Serializes worker creation with teardown without holding the registry lock. */
	mutex_lock(&device->monitor_lock);

	/* One worker supervises all sessions belonging to this registered device. */
	if (device->monitor != NULL) {
		mutex_unlock(&device->monitor_lock);
		return 0;
	}

	/* A withdrawn worker cannot be restarted against disappearing backend state. */
	if (device->monitor_stopping != 0U) {
		mutex_unlock(&device->monitor_lock);
		return ENODEV;
	}

	/* Native work cannot precede successful allocation of its deadline owner. */
	error = kthread_create(gpu_monitor, device, SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0) {
		mutex_unlock(&device->monitor_lock);
		return error;
	}

	/* Registration keeps the device alive until this worker is explicitly reaped. */
	device->monitor = worker;
	thread_start(worker);

	mutex_unlock(&device->monitor_lock);

	/* Succeeded: future GPU work has an autonomous finite supervisor. */
	return 0;
}

/* Reaps the common supervisor before unregister releases borrowed backend state. */
static int
gpu_monitor_stop(
	struct drv_gpu_device *device)
{
	struct thread *worker;
	uint64_t deadline;
	uint64_t now;
	unsigned long irq;
	int error;

	/* Creation cannot race the transition to permanent teardown. */
	mutex_lock(&device->monitor_lock);

	/* Every sleeping monitor sees withdrawal before its worker storage can retire. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->monitor_stopping = 1U;
	waitq_wake_all(&device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Devices which never submitted native work have no thread to reap. */
	worker = device->monitor;
	if (worker == NULL) {
		mutex_unlock(&device->monitor_lock);
		return 0;
	}

	/* A stalled owner remains retained instead of freeing its borrowed backend. */
	deadline = gpu_monitor_deadline(15000U);
	while (1) {
		error = thread_wait(worker, NULL);
		if (error == 0)
			break;

		/* Only a still-running worker permits a finite teardown retry. */
		if (error != EBUSY) {
			mutex_unlock(&device->monitor_lock);
			return error;
		}

		/* Failure leaves the registration and thread references available for retry. */
		now = sched_ticks();
		if (now >= deadline) {
			mutex_unlock(&device->monitor_lock);
			return ETIMEDOUT;
		}

		/* Lets the woken worker leave its final condition check. */
		sched_sleep(now + 1U);
	}

	/* Successful reap consumes the creator's retained thread reference. */
	device->monitor = NULL;

	mutex_unlock(&device->monitor_lock);

	/* Succeeded: no supervisor can call the withdrawn backend again. */
	return 0;
}

/* Sleeps until a job deadline, stop confirmation, or actual resource transition. */
static void
gpu_monitor(
	void *argument)
{
	struct drv_gpu_device *device;
	uint64_t observed;
	uint64_t deadline;
	unsigned stopping;
	unsigned long irq;

	/* Registration and the explicit reap barrier retain this device wrapper. */
	device = argument;
	while (1) {
		/* Snapshot precedes all unlocked callbacks so their wakeups cannot be lost. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		stopping = device->monitor_stopping;
		observed = waitq_sequence(&device->monitor_waitq);

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* Unregister owns the only transition which permanently stops the worker. */
		if (stopping != 0U)
			break;

		/* Driver stop operations execute without any common spinlock held. */
		deadline = gpu_monitor_step(device);

		/* Recheck the same sequence while registering the atomic sleep. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		if (device->monitor_stopping == 0U) {
			(void)waitq_sleep(
				&device->monitor_waitq,
				&gpu_registry_lock,
				observed,
				deadline,
				0U);
		}

		spin_unlock_irqrestore(&gpu_registry_lock, irq);
	}

	/* Succeeded: the thread trampoline publishes exit for the unregister reaper. */
	return;
}

/* Performs one nonblocking deadline or context-stop action under retained ownership. */
static uint64_t
gpu_monitor_step(
	struct drv_gpu_device *device)
{
	struct gpu_session *session;
	struct gpu_session *selected;
	struct drv_gpu_completion *completion;
	const struct drv_gpu_recovery_ops *recovery;
	uint64_t now;
	uint64_t deadline;
	uint64_t candidate;
	unsigned action;
	unsigned notified;
	unsigned busy;
	unsigned running;
	unsigned owned;
	unsigned escalate;
	unsigned isolated;
	unsigned index;
	unsigned long irq;
	int fault_error;
	int status;
	int error;

	/* No driver callback may retain an unpinned session outside this scan. */
	now = sched_ticks();
	deadline = 0U;
	selected = NULL;
	action = 0U;
	notified = 0U;
	recovery = device->ops->recovery;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* All sessions contribute their earliest independent policy deadline. */
	session = device->open_sessions;
	while (session != NULL) {
		/* Final resource teardown excludes new supervisor callbacks. */
		if (session->closing != 0U ||
		    device->error != 0 ||
		    session->monitor_pins != 0U) {
			session = session->next_open;
			continue;
		}

		/* Deadline publication never claims that a timed-out backend stopped accessing memory. */
		for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
			completion = &session->completions[index];
			candidate = completion->job_deadline;
			if (candidate == 0U || completion->completed != 0U)
				continue;

			/* First expiry fixes this session's error and starts its independent stop interval. */
			if (candidate <= now) {
				gpu_session_fail_locked(session, ETIMEDOUT, 1U);
				notified = 1U;
				break;
			}

			/* The nearest deadline controls the next idle wake. */
			if (deadline == 0U || candidate < deadline)
				deadline = candidate;
		}

		/* Healthy, quiescent and already isolated contexts require no stop polling. */
		if (session->stop_state == GPU_STOP_NONE ||
		    session->stop_state == GPU_STOP_FINISHED ||
		    session->stop_state == GPU_STOP_QUARANTINED) {
			session = session->next_open;
			continue;
		}

		/* A backend without a stop contract escalates straight to whole-device recovery. */
		if (recovery == NULL || recovery->stop_begin == NULL) {
			selected = session;
			action = 3U;
			break;
		}

		/* An armed stop interval that has elapsed escalates while retaining uncertain resources. */
		if (session->stop_state == GPU_STOP_PENDING && now >= session->stop_deadline) {
			selected = session;
			action = 3U;
			break;
		}

		/* A stop operation cannot race native publication or ordinary resource-table mutation. */
		busy = session->busy;
		for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
			if (session->completions[index].job_action != 0U)
				busy = 1U;
		}

		/*
		 * A committed job running inside its own execution deadline still owns
		 * the backend, so a graceful producer close waits for its real
		 * completion instead of forcing a stop while it legitimately runs.
		 */
		running = 0U;
		for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
			completion = &session->completions[index];
			if (completion->backend_owned != 0U &&
			    completion->completed == 0U &&
			    completion->job_deadline > now)
				running = 1U;
		}

		/* Admitted operations and running jobs wake the monitor as they retire. */
		if (busy != 0U ||
		    (session->stop_state == GPU_STOP_REQUESTED && running != 0U)) {
			session = session->next_open;
			continue;
		}

		/* A context that never reached the GPU and retains no backend callback is already quiescent. */
		if (session->stop_state == GPU_STOP_REQUESTED) {
			owned = 0U;
			for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
				if (session->completions[index].backend_owned != 0U)
					owned = 1U;
			}

			/* Only a context with possible native ownership needs the backend's stop proof. */
			if (session->native_submitted == 0U && owned == 0U) {
				session->stop_state = GPU_STOP_FINISHED;
				waitq_wake_all(&device->monitor_waitq);
				session = session->next_open;
				continue;
			}

			/* The first nonblocking stage prevents subsequent native acceptance. */
			selected = session;
			action = 1U;
			break;
		}

		/* An armed stop interval bounds this session's periodic retirement queries. */
		if (deadline == 0U || session->stop_deadline < deadline)
			deadline = session->stop_deadline;

		/* Stop queries are periodic only during a finite active retirement interval. */
		if (session->stop_poll_deadline <= now) {
			selected = session;
			action = 2U;
			break;
		}

		/* An earlier backend notification may wake this same condition sooner. */
		if (deadline == 0U || session->stop_poll_deadline < deadline)
			deadline = session->stop_poll_deadline;

		/* The next open remains independently schedulable through local failure. */
		session = session->next_open;
	}

	/* The close barrier retains private backend state through this unlocked action. */
	if (selected != NULL)
		selected->monitor_pins++;

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Fence and descriptor pollers must observe deadline errors even without a callback. */
	if (selected != NULL || notified != 0U)
		poll_notify();

	/* An empty or healthy scan sleeps until its nearest deadline or a state change. */
	if (selected == NULL)
		return deadline;

	/* Only a driver confirmation can establish actual context retirement. */
	error = 0;
	escalate = 0U;
	fault_error = selected->error;
	if (action == 1U) {
		error = recovery->stop_begin(device->private_data, selected->backend, selected->error);
	} else if (action == 2U) {
		error = recovery->stop_poll(device->private_data, selected->backend);
	} else {
		/* An elapsed stop interval or a missing stop contract leaves the context unconfirmed. */
		escalate = 1U;
	}

	/* Native idle proven: reservations the producer never published are withdrawn by the core. */
	if (action == 2U && error == 0)
		gpu_session_reclaim(device, selected);

	/* A failed stop contract cannot turn an unconfirmed context into reusable storage. */
	if (error != 0 && error != EAGAIN) {
		escalate = 1U;
		fault_error = error;
	}

	/* Isolation quarantines only this context's callbacks and storage while its peers continue. */
	isolated = 0U;
	if (escalate != 0U && recovery != NULL && recovery->isolate != NULL) {
		status = recovery->isolate(device->private_data, selected->backend);
		if (status == 0)
			isolated = 1U;
	}

	/* Without isolation, whole-device fault retains every uncertain descriptor and backing allocation. */
	if (escalate != 0U && isolated == 0U) {
		if (recovery != NULL && recovery->fault != NULL)
			recovery->fault(device->private_data, fault_error);

		/* Fault has armed teardown-time retention before public global error becomes visible. */
		drv_gpu_report_error(device, fault_error);
	}

	/* State publication and the final pin release happen before close may destroy the backend. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	if (isolated != 0U) {
		/* The quarantined count keeps checked reset reachable for a later sole fresh open. */
		selected->stop_state = GPU_STOP_QUARANTINED;
		if (device->quarantined != UINT_MAX)
			device->quarantined++;
	} else if (device->error != 0 || escalate != 0U) {
		selected->stop_state = GPU_STOP_FINISHED;
	} else if (action == 2U && error == 0) {
		selected->stop_state = GPU_STOP_FINISHED;
	} else {
		selected->stop_state = GPU_STOP_PENDING;

		/* The stop interval is armed once, when the handshake actually begins. */
		if (action == 1U)
			selected->stop_deadline = gpu_monitor_deadline(CONFIG_GPU_JOB_STOP_MS);

		/* A completed begin can confirm an already idle context immediately. */
		if (action == 1U && error == 0) {
			selected->stop_poll_deadline = sched_ticks();
		} else {
			selected->stop_poll_deadline = gpu_monitor_deadline(100U);
		}
	}

	/* The worker will rescan every session before sleeping after this transition. */
	selected->monitor_pins--;
	waitq_wake_all(&device->monitor_waitq);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: another scan will select the next job or finite stop deadline. */
	return 0U;
}

/* Confirms native retirement before ending supervisor borrowing and resource lifetime. */
static void
gpu_monitor_close(
	struct gpu_session *session)
{
	struct drv_gpu_device *device;
	uint64_t observed;
	uint64_t deadline;
	unsigned finished;
	unsigned long irq;

	/* An optional backend stop contract covers raw native work as well as recorded jobs. */
	device = session->device;
	while (device->ops->recovery != NULL) {
		/* Observe before helping the common policy so synchronous acknowledgements cannot be lost. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		observed = waitq_sequence(&device->monitor_waitq);
		finished = 0U;
		if (device->error != 0 ||
		    session->stop_state == GPU_STOP_FINISHED ||
		    session->stop_state == GPU_STOP_QUARANTINED)
			finished = 1U;

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* Logical completion alone is insufficient; stop acknowledgement or quarantine is required. */
		if (finished != 0U)
			break;

		/* Close uses the same policy as the worker, with pins excluding duplicate stop callbacks. */
		deadline = gpu_monitor_step(device);

		/* Retains all resources while asynchronous native stop remains unconfirmed. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		if (device->error == 0 &&
		    session->stop_state != GPU_STOP_FINISHED &&
		    session->stop_state != GPU_STOP_QUARANTINED) {
			(void)waitq_sleep(
				&device->monitor_waitq,
				&gpu_registry_lock,
				observed,
				deadline,
				0U);
		}

		spin_unlock_irqrestore(&gpu_registry_lock, irq);
	}

	/* No new scan may borrow private state once final teardown has begun. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	session->closing = 1U;
	waitq_wake_all(&device->monitor_waitq);

	/* Nonblocking backend callbacks finish before their close-time lifetime owner leaves. */
	while (session->monitor_pins != 0U) {
		observed = waitq_sequence(&device->monitor_waitq);
		(void)waitq_sleep(
			&device->monitor_waitq,
			&gpu_registry_lock,
			observed,
			0U,
			0U);
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Succeeded: proven native retirement permits the existing resource and backend close order. */
	return;
}

/* Withdraws every unpublished reservation of a proven-idle context through the backend's own cancel. */
static void
gpu_session_reclaim(
	struct drv_gpu_device *device,
	struct gpu_session *session)
{
	struct drv_gpu_completion *completion;
	void *reservation;
	unsigned pending;
	unsigned index;
	unsigned long irq;
	int error;

	/* Backends without reservations retain nothing the core has to withdraw. */
	if (device->ops->jobs == NULL)
		return;

	/* Each retained token is withdrawn outside the registry lock, one exact callback at a time. */
	for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
		completion = &session->completions[index];

		/* Snapshot the token while the registry lock excludes a concurrent job action. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		pending = 0U;
		reservation = completion->job_reservation;
		if (completion->sequence != 0U &&
		    completion->backend_owned != 0U &&
		    completion->reservation_pending != 0U &&
		    reservation != NULL)
			pending = 1U;

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* Committed and ordinary generations retire through their own backend callbacks. */
		if (pending == 0U)
			continue;

		/* Definite nonacceptance withdraws the callback without inventing a completion. */
		error = device->ops->jobs->cancel(
			device->private_data,
			session->backend,
			reservation,
			completion,
			0U);
		if (error != 0)
			continue;

		/* The withdrawn token retires like a late callback and keeps its earlier terminal error. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		completion->reservation_pending = 0U;

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		drv_gpu_complete(completion, ECANCELED);
	}

	/* Succeeded: no unpublished reservation of this context retains a backend callback. */
	return;
}

/* Invokes optional checked reset only for the sole fresh open without old external owners. */
static int
gpu_recover_open(
	struct drv_gpu_device *device)
{
	uint64_t fault_epoch;
	uint64_t observed;
	unsigned recover;
	unsigned long irq;
	int error;

	/* Recovery authority is distinct from logical error observation. */
	recover = 0U;
	irq = spin_lock_irqsave(&gpu_registry_lock);

	/* A second fresh open arriving during checked reset waits for its result instead of failing. */
	while (device->recovering != 0U && device->online != 0U) {
		observed = waitq_sequence(&device->capacity_waitq);
		error = waitq_sleep(
			&device->capacity_waitq,
			&gpu_registry_lock,
			observed,
			0U,
			WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&gpu_registry_lock, irq);
			return error;
		}
	}

	error = device->error;
	fault_epoch = device->fault_epoch;
	if (device->online == 0U) {
		error = ENODEV;
	} else if ((error != 0 || device->quarantined != 0U) &&
	    device->sessions == 1U &&
	    device->shares == 0U &&
	    device->open_sessions == NULL &&
	    device->ops->recovery != NULL &&
	    device->ops->recovery->reset != NULL) {
		device->recovering = 1U;
		recover = 1U;
	}

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Healthy opens need no hardware transition, and retained old owners prevent recovery. */
	if (recover == 0U) {
		if (error != 0)
			return error;

		/* Succeeded: the existing hardware instance can create a fresh context. */
		return 0;
	}

	/* The backend must prove IRQ, descriptor and DMA retirement before reconstructing hardware. */
	error = device->ops->recovery->reset(device->private_data);

	/* Only checked success clears device loss; old Vulkan objects are never revived. */
	irq = spin_lock_irqsave(&gpu_registry_lock);

	device->recovering = 0U;

	/* Only the successful reset result can be compared with its original fault epoch. */
	if (error == 0) {
		/* A restart-time IRQ fault must not be overwritten by an earlier reset success. */
		if (device->fault_epoch != fault_epoch || device->fault_epoch_overflow != 0U) {
			error = device->error;

			/* Epoch exhaustion cannot certify a safe transition even after a separate clear. */
			if (error == 0)
				error = EIO;
		} else {
			/* Checked reset retired every quarantined context's storage together with the loss. */
			device->error = 0;
			device->quarantined = 0U;
		}
	}

	gpu_capacity_changed_locked(device);

	spin_unlock_irqrestore(&gpu_registry_lock, irq);

	/* Failed reset leaves the original device fault visible for a later explicit retry. */
	if (error != 0)
		return error;

	/* Succeeded: all future opens refer to a newly checked hardware generation. */
	return 0;
}

/* Waits for a relevant capacity transition without owning submission or table admission. */
static int
gpu_capacity_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_job_capacity request;
	struct drv_gpu_device *device;
	uint64_t deadline;
	uint64_t observed;
	unsigned available;
	unsigned records;
	unsigned index;
	unsigned ready;
	unsigned long irq;
	int error;

	/* A capacity contract needs the backend's nonblocking domain-aware snapshot. */
	device = session->device;
	if ((device->ops->capabilities & GPU_CAP_JOB_CAPACITY) == 0U)
		return EOPNOTSUPP;

	/* Fixed-width framing keeps both process ABIs independent from pointer size. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Only the documented query flag may alter the wait operation. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    (request.flags & ~GPU_JOB_CAPACITY_QUERY) != 0U ||
	    request.sequence != 0U ||
	    request.available != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* A snapshot has no previous observation or requested sleep interval. */
	if (request.flags != 0U) {
		if (request.observed_sequence != 0U || request.timeout_ns != 0U)
			return EINVAL;
	} else if (request.observed_sequence == 0U) {
		return EINVAL;
	}

	/* Time arithmetic is validated before borrowing any backend operation. */
	error = gpu_completion_deadline(request.timeout_ns, &deadline);
	if (error != 0)
		return error;

	/* Both backend capacity and common record retirement can change independently. */
	while (1) {
		/* Snapshot first so release between backend observation and sleep is visible. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		error = gpu_session_error_locked(session);
		observed = waitq_sequence(&device->capacity_waitq);

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* A failed context never waits for space it can no longer use. */
		if (error != 0)
			return error;

		/* The backend validates its queue domain without holding a common spinlock. */
		available = 0U;
		error = device->ops->jobs->capacity(device->private_data, session->backend, request.domain, &available);
		if (error != 0)
			return error;

		/* Record capacity is sampled together with the sequence used by atomic sleep. */
		irq = spin_lock_irqsave(&gpu_registry_lock);

		error = gpu_session_error_locked(session);

		/* A saturated observation sequence cannot authorize an aliased wait token. */
		if (device->capacity_overflow != 0U)
			error = EOVERFLOW;

		/* Future snapshots cannot be fabricated to suppress a real transition. */
		if (request.observed_sequence > device->capacity_sequence)
			error = EINVAL;

		/* A consumed result cannot be reused while a backend or observer still owns it. */
		records = 0U;
		for (index = 0U; index < GPU_SUBMIT_MAX; index++) {
			if (session->completions[index].sequence == 0U)
				records++;
		}

		/* Existing terminal levels do not repeatedly wake pinned-record retries. */
		ready = 0U;
		if (request.flags != 0U) {
			ready = 1U;
		} else if (request.observed_sequence != device->capacity_sequence) {
			ready = 1U;
		} else if (records != 0U && available != 0U) {
			ready = 1U;
		}

		/* A failed condition never changes caller ownership or reserves native work. */
		if (error != 0) {
			spin_unlock_irqrestore(&gpu_registry_lock, irq);
			return error;
		}

		/* The returned sequence lets the next reclaim-and-reserve attempt register a new wait. */
		if (ready != 0U) {
			request.sequence = device->capacity_sequence;

			/* Availability remains advisory because another caller can reserve first. */
			if (records != 0U && available != 0U)
				request.available = 1U;

			spin_unlock_irqrestore(&gpu_registry_lock, irq);

			/* The output snapshot no longer needs either capacity owner locked. */
			break;
		}

		/* Zero timeout observes pressure without entering a kernel wait. */
		if (request.timeout_ns == 0U) {
			spin_unlock_irqrestore(&gpu_registry_lock, irq);
			return EAGAIN;
		}

		/* Atomic sequence comparison closes both backend-release and common-reap races. */
		error = waitq_sleep(
			&device->capacity_waitq,
			&gpu_registry_lock,
			observed,
			deadline,
			WAITQ_INTERRUPTIBLE);

		spin_unlock_irqrestore(&gpu_registry_lock, irq);

		/* A changed condition is rechecked; interruption and timeout leave native work unaccepted. */
		if (error != 0 && error != EAGAIN)
			return error;
	}

	/* Observation does not consume capacity, even when the output copy fails. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: userspace may reclaim and retry the ordinary nonblocking reservation. */
	return 0;
}

/* Returns the immutable finite administrator policy independently from client wait timeouts. */
static int
gpu_policy_ioctl(
	struct gpu_session *session,
	uintptr_t argument)
{
	struct gpu_job_policy request;
	int error;

	/* Only supervised backends expose the common GPU job policy. */
	if ((session->device->ops->capabilities & GPU_CAP_JOB) == 0U)
		return EOPNOTSUPP;

	/* Every output field begins at zero, with no hidden setter semantics. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* The effective policy cannot be changed through a process descriptor. */
	if (request.version != GPU_ABI_VERSION ||
	    request.size != sizeof(request) ||
	    request.reservation_timeout_ns != 0U ||
	    request.execution_timeout_ns != 0U ||
	    request.stop_timeout_ns != 0U ||
	    request.flags != 0U ||
	    request.reserved != 0U)
		return EINVAL;

	/* Public nanoseconds describe the configured minimum before clock rounding. */
	request.reservation_timeout_ns = (uint64_t)CONFIG_GPU_JOB_RESERVATION_MS * 1000000U;
	request.execution_timeout_ns = (uint64_t)CONFIG_GPU_JOB_EXECUTION_MS * 1000000U;
	request.stop_timeout_ns = (uint64_t)CONFIG_GPU_JOB_STOP_MS * 1000000U;

	/* A failed copy leaves policy and every reservation unchanged. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the client can distinguish admission time from supervised GPU time. */
	return 0;
}
