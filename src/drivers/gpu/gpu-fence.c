/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Resettable completion payloads retained by descriptors and pending kernel work.
 * The producer owns authority to signal a bound generation, consumers retain and
 * observe that same payload without acquiring the producer's GPU session.
 */

#include <drivers/gpu/gpu-fence.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <stddef.h>

/* One independently retained payload; its lock protects generation, owner and state. */
struct drv_gpu_fence {
	struct spinlock lock;
	struct wait_queue waitq;
	uint64_t device;
	uint64_t generation;
	void *owner;
	uint32_t state;
	unsigned admitted;
	int error;
};

static void fence_release(void *object);
static int fence_poll(void *object, short events, short *revents);
static const struct kernel_handle_ops *fence_operations(void);
static int fence_resolve(struct kernel_handle *handle, struct drv_gpu_fence **result);
static int fence_wait(struct kernel_handle *handle, uint64_t generation, uint64_t deadline, unsigned immediate, unsigned work, struct drv_gpu_fence_state *state);
static void fence_snapshot(struct drv_gpu_fence *fence, struct drv_gpu_fence_state *state);

/*
 * Allocates one payload whose initial reference can be transferred into an fd.
 */
int
drv_gpu_fence_create(
	uint64_t device,
	unsigned signaled,
	struct kernel_handle **result)
{
	struct drv_gpu_fence *fence;
	const struct kernel_handle_ops *operations;
	int error;

	/* Invalid input must never publish partial ownership. */
	if (result == NULL)
		return EINVAL;

	/* Only a registered device identity may establish this capability namespace. */
	*result = NULL;
	if (device == 0U || signaled > 1U)
		return EINVAL;

	/* Reserve the independent payload before creating its public handle wrapper. */
	fence = kern_calloc(1U, sizeof(*fence));
	if (fence == NULL)
		return ENOMEM;

	/* Payload state is private until handle creation succeeds. */
	spin_init(&fence->lock, LOCK_RANK_DEVICE, "GPU fence");
	waitq_init(&fence->waitq, "GPU fence");
	fence->device = device;
	fence->generation = 1U;

	/* Creation may begin with no pending producer work. */
	if (signaled != 0U)
		fence->state = DRV_GPU_FENCE_SIGNALED;

	/* A failed wrapper allocation leaves payload cleanup with this caller. */
	operations = fence_operations();
	error = handle_create(KERNEL_HANDLE_DRIVER, operations, fence, result);
	if (error != 0) {
		kern_free(fence);
		return error;
	}

	/* Succeeded: descriptors and kernel work may retain the same payload reference. */
	return 0;
}

/*
 * Enforces an immutable device namespace without borrowing the originating open.
 */
int
drv_gpu_fence_device(
	struct kernel_handle *handle,
	uint64_t device)
{
	struct drv_gpu_fence *fence;
	int error;

	/* Typed authority is required before interpreting the payload. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* Device identities are never reused after unregistration. */
	if (fence->device != device)
		return EXDEV;

	/* Succeeded: this capability belongs to the requested device. */
	return 0;
}

/*
 * Reads one atomic observation, optionally requiring a particular generation.
 */
int
drv_gpu_fence_query(
	struct kernel_handle *handle,
	uint64_t generation,
	struct drv_gpu_fence_state *state)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* A missing output cannot own an observation. */
	if (state == NULL)
		return EINVAL;

	/* Validate type before locking a subsystem-owned payload. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* A query never mixes the state of different reset generations. */
	irq = spin_lock_irqsave(&fence->lock);

	if (generation != 0U && generation != fence->generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Return fields belong to the one generation protected by this critical section. */
	fence_snapshot(fence, state);

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: state and error describe one exact generation. */
	return 0;
}

/*
 * Observes one generation without requiring a submitted GPU producer.
 */
int
drv_gpu_fence_wait(
	struct kernel_handle *handle,
	uint64_t generation,
	uint64_t deadline,
	unsigned immediate,
	struct drv_gpu_fence_state *state)
{
	int error;

	/* CPU observation permits unsubmitted payloads and honors its own deadline. */
	error = fence_wait(handle, generation, deadline, immediate, 0U, state);
	if (error != 0)
		return error;

	/* Succeeded: the exact generation reached a terminal state. */
	return 0;
}

/*
 * Waits only for work whose driver has accepted autonomous completion responsibility.
 */
int
drv_gpu_fence_wait_work(
	struct kernel_handle *handle,
	uint64_t generation,
	struct drv_gpu_fence_state *state)
{
	int error;

	/* The producer deadline owns failure detection; unadmitted work is refused immediately. */
	error = fence_wait(handle, generation, 0U, 0U, 1U, state);
	if (error != 0)
		return error;

	/* Succeeded: the accepted GPU dependency reached a terminal state. */
	return 0;
}

/*
 * Starts a new unsignaled generation only when no pending producer owns the old one.
 */
int
drv_gpu_fence_reset(
	struct kernel_handle *handle,
	uint64_t generation,
	struct drv_gpu_fence_state *state)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* Reset requires an exact current generation and observable successor. */
	if (generation == 0U || state == NULL)
		return EINVAL;

	/* Resolve the payload before touching its lifetime state. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* Owner and generation checks serialize reset with every binding and signal. */
	irq = spin_lock_irqsave(&fence->lock);

	if (generation != fence->generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Pending device work must never acquire a new generation under its producer. */
	if (fence->owner != NULL) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EBUSY;
	}

	/* Refuse wrap instead of making a stale producer identity valid again. */
	if (fence->generation == UINT64_MAX) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EOVERFLOW;
	}

	/* Publish one new pending state and wake old-generation observers. */
	fence->generation++;
	fence->state = DRV_GPU_FENCE_PENDING;
	fence->admitted = 0U;
	fence->error = 0;
	fence_snapshot(fence, state);
	waitq_wake_all(&fence->waitq);

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Notify descriptor observers only after the payload lock has been released. */
	poll_notify();

	/* Succeeded: old references remain valid capabilities for the new generation. */
	return 0;
}

/*
 * Reserves one producer's authority before native work is accepted.
 */
int
drv_gpu_fence_bind(
	struct kernel_handle *handle,
	uint64_t generation,
	void *owner)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* An unowned or unspecified work generation grants no signal authority. */
	if (owner == NULL || generation == 0U)
		return EINVAL;

	/* The caller retains the handle and owner until signal or rollback. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* Exactly one producer may reserve a pending generation. */
	irq = spin_lock_irqsave(&fence->lock);

	if (fence->generation != generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* An already terminal payload requires explicit reset before reuse. */
	if (fence->state != DRV_GPU_FENCE_PENDING || fence->owner != NULL) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EBUSY;
	}

	/* The retained producer owns this generation until rollback or terminal completion. */
	fence->owner = owner;
	fence->admitted = 0U;

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: the retained owner now has exclusive signal authority. */
	return 0;
}

/*
 * Admits a reserved generation after its driver accepts the GPU completion obligation.
 */
int
drv_gpu_fence_admit(
	struct kernel_handle *handle,
	uint64_t generation,
	void *owner)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* Admission cannot manufacture producer authority for an arbitrary descriptor. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* The exact reserved generation becomes eligible atomically with waiter observation. */
	irq = spin_lock_irqsave(&fence->lock);

	if (fence->generation != generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Only the retained producer may commit its still-pending reservation. */
	if (owner == NULL || fence->owner != owner) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EPERM;
	}

	/* A completed reservation must never reopen a previous completion obligation. */
	if (fence->state != DRV_GPU_FENCE_PENDING) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EINVAL;
	}

	/* Driver completion, expiry or final close now guarantees a terminal outcome. */
	fence->admitted = 1U;

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: kernel dependency waits may follow this generation. */
	return 0;
}

/*
 * Cancels a proven unaccepted submission without manufacturing an error visible to aliases.
 */
int
drv_gpu_fence_unbind(
	struct kernel_handle *handle,
	uint64_t generation,
	void *owner)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* Resolve the typed payload before checking ownership. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* A late rollback cannot cancel a different submission or reset generation. */
	irq = spin_lock_irqsave(&fence->lock);

	if (fence->generation != generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Possessing an imported descriptor alone grants no producer authority. */
	if (owner == NULL || fence->owner != owner) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EPERM;
	}

	/* No accepted work remains associated with the canceled reservation. */
	fence->owner = NULL;
	fence->admitted = 0U;

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: state and generation remain unchanged and no work owns the payload. */
	return 0;
}

/*
 * Publishes verified completion and then wakes process poll observers without a caller-owned registry lock.
 */
int
drv_gpu_fence_signal(
	struct kernel_handle *handle,
	uint64_t generation,
	void *owner,
	int status)
{
	int error;

	/* Payload state and waiter registration change together before global readiness notification. */
	error = drv_gpu_fence_signal_deferred(handle, generation, owner, status);
	if (error != 0)
		return error;

	/* Notify descriptor observers only after the payload lock has been released. */
	poll_notify();

	/* Succeeded: both payload waiters and descriptor pollers can observe completion. */
	return 0;
}

/*
 * Publishes verified success or terminal error, releasing the bound producer's authority.
 */
int
drv_gpu_fence_signal_deferred(
	struct kernel_handle *handle,
	uint64_t generation,
	void *owner,
	int status)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	int error;

	/* Error values are positive kernel errno values; zero alone means success. */
	if (status < 0)
		return EINVAL;

	/* A strong typed reference protects the payload independently from its fd. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* Exact owner and generation prevent stale or consumer-forged completion. */
	irq = spin_lock_irqsave(&fence->lock);

	if (fence->generation != generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Only pending bound work may transition into a terminal state. */
	if (owner == NULL || fence->owner != owner) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return EPERM;
	}

	/* Error notification remains terminal without being represented as successful execution. */
	fence->state = DRV_GPU_FENCE_SIGNALED;
	if (status != 0)
		fence->state = DRV_GPU_FENCE_ERROR;

	/* Clearing owner permits the next explicit reset after all observers see the terminal state. */
	fence->error = status;
	fence->owner = NULL;
	fence->admitted = 0U;
	waitq_wake_all(&fence->waitq);

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: every descriptor alias observes the same terminal result. */
	return 0;
}

/* Observes one exact generation while its admission condition and sleep registration remain atomic. */
static int
fence_wait(
	struct kernel_handle *handle,
	uint64_t generation,
	uint64_t deadline,
	unsigned immediate,
	unsigned work,
	struct drv_gpu_fence_state *state)
{
	struct drv_gpu_fence *fence;
	uint64_t observed;
	unsigned long irq;
	int error;

	/* Waiting on an unspecified generation could silently follow a reset. */
	if (generation == 0U || state == NULL)
		return EINVAL;

	/* Resolve only typed kernel payload authority. */
	error = fence_resolve(handle, &fence);
	if (error != 0)
		return error;

	/* A reset, signal or failure wakes the same condition channel. */
	irq = spin_lock_irqsave(&fence->lock);

	while (generation == fence->generation && fence->state == DRV_GPU_FENCE_PENDING) {
		/* A kernel dependency cannot wait for userspace to publish or signal future work. */
		if (work != 0U && fence->admitted == 0U) {
			spin_unlock_irqrestore(&fence->lock, irq);
			return EAGAIN;
		}

		/* An immediate observation does not consume or alter pending work. */
		if (immediate != 0U) {
			spin_unlock_irqrestore(&fence->lock, irq);
			return EAGAIN;
		}

		/* Registration and owner signal cannot cross without changing the observed sequence. */
		observed = waitq_sequence(&fence->waitq);
		error = waitq_sleep(&fence->waitq, &fence->lock, observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&fence->lock, irq);
			return error;
		}
	}

	/* A concurrent reset invalidates this observer's original work identity. */
	if (generation != fence->generation) {
		spin_unlock_irqrestore(&fence->lock, irq);
		return ESTALE;
	}

	/* Return fields belong to the one generation protected by this critical section. */
	fence_snapshot(fence, state);

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Succeeded: terminal error remains distinct from successful completion. */
	return 0;
}

/* Releases metadata after the last descriptor, waiter and producer reference disappears. */
static void
fence_release(
	void *object)
{
	/* Bound producers retain their own handle reference until they terminate or roll back. */
	kern_free(object);

	/* Succeeded: no open session or VFS object is involved in final destruction. */
	return;
}

/* Returns level readiness until the payload is explicitly reset. */
static int
fence_poll(
	void *object,
	short events,
	short *revents)
{
	struct drv_gpu_fence *fence;
	unsigned long irq;
	short result;

	/* Descriptor lookup retains this payload while the readiness snapshot is taken. */
	fence = object;
	result = 0;
	irq = spin_lock_irqsave(&fence->lock);

	if (fence->state != DRV_GPU_FENCE_PENDING)
		result = events & (POLLIN | POLLRDNORM);

	/* Terminal failure is visible even when the caller requests no ordinary input bits. */
	if (fence->state == DRV_GPU_FENCE_ERROR)
		result |= POLLERR;

	spin_unlock_irqrestore(&fence->lock, irq);

	/* Publish the complete readiness snapshot after dropping the payload lock. */
	*revents = result;

	/* Succeeded: observation never consumes completion or mutates reference ownership. */
	return 0;
}

/* Keeps the immutable callback table beside the callbacks without forward data declarations. */
static const struct kernel_handle_ops *
fence_operations(
	void)
{
	static const struct kernel_handle_ops operations = {
		fence_release,
		fence_poll
	};

	/* Succeeded: every fence handle uses the same trusted payload implementation. */
	return &operations;
}

/* Refuses foreign handle implementations even when their numeric type happens to match. */
static int
fence_resolve(
	struct kernel_handle *handle,
	struct drv_gpu_fence **result)
{
	const struct kernel_handle_ops *operations;

	/* A matching numeric tag alone is not sufficient authority to interpret memory. */
	if (handle == NULL || handle->type != KERNEL_HANDLE_DRIVER)
		return EINVAL;

	/* Only handles created by this subsystem carry this payload layout. */
	operations = fence_operations();
	if (handle->ops != operations)
		return EINVAL;

	/* The strong handle reference retains this verified payload for the caller. */
	*result = handle->object;

	/* Succeeded: the retained handle owns a valid completion payload. */
	return 0;
}

/* Copies state while the caller holds the payload lock. */
static void
fence_snapshot(
	struct drv_gpu_fence *fence,
	struct drv_gpu_fence_state *state)
{
	/* One observation never combines fields from separate generations. */
	state->generation = fence->generation;
	state->state = fence->state;
	state->error = fence->error;

	/* Succeeded: the caller owns an immutable value snapshot. */
	return;
}
