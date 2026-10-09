/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete Vulkan transport callbacks bind only after native ownership and recovery exist. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-job.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/render-runtime.h"
#include "drivers/gpu/bcm2711/vulkan-dispatch.h"

#define VULKAN_CAPSET_ID 4U
#define VULKAN_CAPSET_BYTES 168U
#define VULKAN_XML_VERSION ((1U << 22) | (3U << 12) | 269U)
#define VULKAN_VENDOR_MAGIC 0x5a424453U
/* Opaque sharing, supervised native queues and checked stop are the exact paired-client profile. */
#define VULKAN_VENDOR_FLAGS 7U

/* One heap-owned immutable CPU frame survives acceptance until the existing worker disposes it. */
struct runtime_command {
	uint32_t bytes;
	uint8_t wire[1];
};

static int runtime_submit(void *opaque, void *private_session, const void *wire, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void runtime_drain(void *opaque, void *private_session);

/* Permanent registered operations retain callbacks until common close has joined the worker. */
static const struct drv_gpu_command_ops runtime_operations = {
	runtime_submit,
	runtime_drain
};

static int runtime_capset(void *opaque, void *private_session, struct gpu_capset *capset);
static int runtime_command(void *opaque, void *private_session, const void *wire, uint32_t bytes);
static int execute_command(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, void *payload, bool *retired);
static int dispose_command(struct bcm2711_render_device *controller, void *payload, bool retired);
static int runtime_ready(struct bcm2711_render_session *session);
static bool runtime_uncertain(struct bcm2711_render_device *controller);
static void capset_word(uint8_t *data, uint32_t offset, uint32_t word);

/*
 * Binds complete transport, capability and supervised-job operations before node registration.
 */
void
bcm2711_render_runtime_bind(
	struct drv_gpu_ops *operations)
{
	/* Actual native dispatch, decoder notification and callback drain form one publication boundary. */
	operations->get_capset = runtime_capset;
	operations->command = runtime_command;
	operations->commands = &runtime_operations;
	operations->capabilities |= GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
	bcm2711_render_jobs_bind(operations);

	/* Succeeded: publication can advertise the complete existing-client transport profile. */
	return;
}

/* Publishes the actual client's bounded protocol record without advertising unrelated video or host scanout. */
static int
runtime_capset(
	void *opaque,
	void *private_session,
	struct gpu_capset *capset)
{
	struct bcm2711_render_session *session;
	int error;

	/* Only this controller's completed Vulkan namespace may negotiate its implemented protocol. */
	session = private_session;
	if (session == NULL ||
	    session->device != opaque ||
	    capset == NULL)
		return EINVAL;

	/* Unknown protocol selectors cannot negotiate the native runtime profile. */
	if (capset->capset_id != VULKAN_CAPSET_ID || capset->capset_version != 0)
		return ENOTSUP;

	/* The whole fixed record must fit the requested ordinary inline reply capacity. */
	if (capset->capacity < VULKAN_CAPSET_BYTES || capset->capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* Namespace close cannot interleave with discovery or leave a partially populated successful record. */
	mutex_lock(&session->device->mutex);

	error = runtime_ready(session);
	if (error != 0) {
		mutex_unlock(&session->device->mutex);
		return error;
	}

	/* Unused protocol words and all padding remain explicit zero; known fields use independent little-endian stores. */
	kern_memset(capset->data, 0, sizeof(capset->data));
	capset_word(capset->data, 0, 1);
	capset_word(capset->data, 4, VULKAN_XML_VERSION);
	capset_word(capset->data, 152, 1);
	capset_word(capset->data, 160, VULKAN_VENDOR_MAGIC);
	capset_word(capset->data, 164, VULKAN_VENDOR_FLAGS);
	capset->bytes = VULKAN_CAPSET_BYTES;

	mutex_unlock(&session->device->mutex);

	/* Succeeded: the record selects only implemented opaque, strict queue and quiescence contracts. */
	return 0;
}

/* Executes a legacy copied frame only when no older callback owns this session's order. */
static int
runtime_command(
	void *opaque,
	void *private_session,
	const void *wire,
	uint32_t bytes)
{
	struct bcm2711_render_session *session;
	struct bcm2711_render_device *controller;
	unsigned long enabled;
	uint32_t pending;
	bool uncertain;
	int error;

	/* Ordinary command receipt consumes its borrowed kernel snapshot before returning. */
	session = private_session;
	controller = opaque;
	if (session == NULL ||
	    session->device != controller ||
	    wire == NULL)
		return EINVAL;

	/* Legacy frames contain complete protocol words within the existing command bound. */
	if (bytes == 0 ||
	    bytes > GPU_COMMAND_MAX ||
	    (bytes & 3U) != 0)
		return EINVAL;

	/* Native decode and typed ownership remain serialized with ordinary resource destruction. */
	mutex_lock(&controller->mutex);

	/* A legacy caller may retry instead of overtaking an accepted decoder or unpublished supervised marker. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	pending = session->pending;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* A pending callback requires a retry before legacy synchronous execution. */
	error = EAGAIN;
	if (pending == 0)
		error = runtime_ready(session);

	/* The complete private namespace and typed dispatcher execute under their single ownership mutex. */
	if (error == 0)
		error = bcm2711_vulkan_stream_execute(session->vulkan, wire, bytes, bcm2711_vulkan_dispatch);

	/* Native faults remain visible even if the protocol encoded an ordinary Vulkan result. */
	uncertain = runtime_uncertain(controller);
	if (uncertain && error == 0)
		error = EIO;

	mutex_unlock(&controller->mutex);

	/* Native jobs have already rooted uncertain storage before common observers receive global loss. */
	if (uncertain)
		bcm2711_render_fail(controller, error);
	if (error != 0)
		return error;

	/* Succeeded: the legacy frame consumed all input without bypassing an older callback. */
	return 0;
}

/* Copies a bounded frame before the worker receives exactly one real common callback obligation. */
static int
runtime_submit(
	void *opaque,
	void *private_session,
	const void *wire,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct bcm2711_render_session *session;
	struct runtime_command *command;
	int error;

	/* Refused callback arguments never transfer a completion or retain borrowed command bytes. */
	session = private_session;
	if (session == NULL ||
	    session->device != opaque ||
	    completion == NULL)
		return EINVAL;

	/* Only the existing selected context-completion flag has a defined interpretation. */
	if ((flags & ~GPU_COMMAND_CONTEXT_FENCE) != 0 || timeline >= 64)
		return EINVAL;

	/* An unselected domain cannot silently acquire another native queue's completion identity. */
	if (flags == 0 && timeline != 0)
		return EINVAL;

	/* Every retained snapshot contains complete bounded protocol words. */
	if (bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0)
		return EINVAL;

	/* Nonempty frames require actual borrowed kernel bytes before copying. */
	if (bytes != 0 && wire == NULL)
		return EINVAL;

	/* A frame without bytes is meaningful only as an explicitly selected context marker. */
	if (bytes == 0 && flags == 0)
		return EINVAL;

	/* CPU snapshots own no DMA and allocate before acceptance; an empty selected marker owns only its length. */
	command = kern_calloc(1, sizeof(*command) + bytes);
	if (command == NULL)
		return ENOMEM;

	/* The new CPU owner retains the exact frame length and an independent copy of every input byte. */
	command->bytes = bytes;
	if (bytes != 0)
		kern_memcpy(command->wire, wire, bytes);

	/* Namespace ownership and ordinary resource changes remain mutually exclusive with this callback. */
	mutex_lock(&session->device->mutex);

	/* Callback admission and namespace publication cannot interleave with close or typed owner destruction. */
	error = runtime_ready(session);
	if (error == 0)
		error = bcm2711_render_worker_submit(session, execute_command, dispose_command, command, timeline, completion);

	mutex_unlock(&session->device->mutex);

	/* A refused request still belongs entirely to the caller and leaves no backend callback behind. */
	if (error != 0) {
		kern_free(command);
		return error;
	}

	/* Succeeded: worker pending ownership preserves this exact namespace and immutable frame through final callback. */
	return 0;
}

/* Joins accepted callbacks before the common core retires this session's resources or Vulkan identities. */
static void
runtime_drain(
	void *opaque,
	void *private_session)
{
	struct bcm2711_render_session *session;

	/* Common final close passes the same retained controller and session that accepted all callbacks. */
	session = private_session;
	if (session == NULL || session->device != opaque)
		__builtin_trap();
	bcm2711_render_worker_drain(session);

	/* Succeeded: every accepted common callback ended, while uncertain native storage remains quarantined. */
	return;
}

/* Runs one immutable frame under the existing worker's controller mutex. */
static int
execute_command(
	struct bcm2711_render_device *controller,
	struct bcm2711_render_session *session,
	void *payload,
	bool *retired)
{
	struct runtime_command *command;
	bool uncertain;
	int error;

	/* A selected empty context marker owns no native work and follows all older FIFO frames. */
	command = payload;
	*retired = true;
	error = runtime_ready(session);
	if (error == 0 && command->bytes != 0)
		error = bcm2711_vulkan_stream_execute(session->vulkan, command->wire, command->bytes, bcm2711_vulkan_dispatch);

	/* A Vulkan DEVICE_LOST reply may accompany transport success, so actual native uncertainty is sampled separately. */
	uncertain = runtime_uncertain(controller);
	if (uncertain) {
		*retired = false;

		/* Native uncertainty cannot accompany a successful common command notification. */
		if (error == 0)
			error = EIO;
	}

	/* A refused decoder never permits the notification to masquerade as successful receipt. */
	if (error != 0)
		return error;

	/* Succeeded: all decoded native jobs retired before this frame's success callback. */
	return 0;
}

/* Frees CPU-only copied framing independently of any separately quarantined native job graph. */
static int
dispose_command(
	struct bcm2711_render_device *controller,
	void *payload,
	bool retired)
{
	/* Whole uncertain DMA owners live in controller quarantine, never in this copied CPU input allocation. */
	(void)controller;
	(void)retired;
	kern_free(payload);

	/* Succeeded: disposal neither claims native retirement nor touches quarantined backing. */
	return 0;
}

/* Confirms this complete namespace remains admitted under its controller mutex and native IRQ guard. */
static int
runtime_ready(
	struct bcm2711_render_session *session)
{
	struct bcm2711_render_device *controller;
	unsigned long enabled;
	int error;

	/* A closed namespace cannot route a command even if native hardware is still ready. */
	if (session->vulkan == NULL || session->vulkan->closing)
		return ECANCELED;
	controller = session->device;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	error = 0;
	if (session->stopping ||
	    controller->worker.uncertain ||
	    !controller->space.native->hardware.ready ||
	    controller->space.native->hardware.faulted)
		error = EIO;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Stopping or uncertain namespaces cannot publish fresh callbacks, replies or native work. */
	if (error != 0)
		return error;

	/* Succeeded: the caller may execute or queue a frame for this exact native namespace. */
	return 0;
}

/* Samples native uncertainty even when Vulkan returned an ordinary device-loss result inside a successful stream. */
static bool
runtime_uncertain(
	struct bcm2711_render_device *controller)
{
	unsigned long enabled;
	bool uncertain;

	/* Native IRQ and worker faults remain visible independently of the final decoder trailer. */
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	uncertain = false;
	if (controller->worker.uncertain || controller->space.native->hardware.faulted)
		uncertain = true;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: the caller knows whether success-only native retirement remains unproven. */
	return uncertain;
}

/* Writes one protocol field without relying on host alignment or native byte order. */
static void
capset_word(
	uint8_t *data,
	uint32_t offset,
	uint32_t word)
{
	/* The negotiated wire uses explicit little-endian bytes for every known 32-bit word. */
	data[offset] = word & 0xffU;
	data[offset + 1] = (word >> 8) & 0xffU;
	data[offset + 2] = (word >> 16) & 0xffU;
	data[offset + 3] = (word >> 24) & 0xffU;

	/* Succeeded: one bounded known field has its stable wire representation. */
	return;
}
