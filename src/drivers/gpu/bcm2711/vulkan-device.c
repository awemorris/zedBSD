/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native Vulkan roots validate actual client records and retain parent/domain ownership through final retirement. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/vulkan-device.h"

/* The existing Zlib record codec is device-independent and remains unchanged. */
#include "drivers/gpu/i915/render/vulkan-codec.inc"

/* A graphics-only device exposes one serialized native queue, without compute/video families. */
#define VULKAN_NATIVE_QUEUES 1U
#define VULKAN_TIMELINE_RECORD 1000384005U

static int create_instance(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int enumerate_physical(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_device(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int device_queue(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_root(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int wait_idle(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int publish_root(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity, struct bcm2711_vulkan_object *parent, uint32_t queues, uint32_t timeline, struct bcm2711_vulkan_object **object);
static int release_root(struct bcm2711_vulkan_session *session, void *payload);
static int claim_timeline(struct bcm2711_vulkan_session *session, uint32_t timeline);
static void release_timeline(struct bcm2711_vulkan_session *session, uint32_t timeline);
static void reply_identity(struct i915_wire_writer *reply, VkResult status, uint64_t identity);

/*
 * Executes native instance, logical-device and queue ownership commands under the controller mutex.
 */
int
bcm2711_vulkan_device_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Destruction has no parameter payload, but an ordinary client still requests its echoed opcode. */
	*handled = 1;
	if (opcode == GPU_OP_DESTROY_INSTANCE || opcode == GPU_OP_DESTROY_DEVICE) {
		if (requested > 1)
			return EINVAL;
	} else {
		/* Unknown opcodes remain available to the typed resource and draw routers. */
		if (opcode != GPU_OP_CREATE_INSTANCE &&
		    opcode != GPU_OP_ENUMERATE_PHYSICAL_DEVICES &&
		    opcode != GPU_OP_CREATE_DEVICE &&
		    opcode != GPU_OP_GET_DEVICE_QUEUE2 &&
		    opcode != GPU_OP_QUEUE_WAIT_IDLE &&
		    opcode != GPU_OP_DEVICE_WAIT_IDLE) {
			*handled = 0;
			return 0;
		}

		/* A decoder cannot create objects behind an absent result/output response. */
		if (requested != 1)
			return EINVAL;
	}

	/* Dispatch only the roots whose typed input and retirement semantics are implemented here. */
	switch (opcode) {
	case GPU_OP_CREATE_INSTANCE:
		error = create_instance(session, reader, reply);
		break;
	case GPU_OP_ENUMERATE_PHYSICAL_DEVICES:
		error = enumerate_physical(session, reader, reply);
		break;
	case GPU_OP_CREATE_DEVICE:
		error = create_device(session, reader, reply);
		break;
	case GPU_OP_GET_DEVICE_QUEUE2:
		error = device_queue(session, reader, reply);
		break;
	case GPU_OP_DESTROY_INSTANCE:
		error = destroy_root(session, I915_VK_OBJ_INSTANCE, reader);
		break;
	case GPU_OP_DESTROY_DEVICE:
		error = destroy_root(session, I915_VK_OBJ_DEVICE, reader);
		break;
	case GPU_OP_QUEUE_WAIT_IDLE:
		error = wait_idle(session, I915_VK_OBJ_QUEUE, reader, reply);
		break;
	default:
		error = wait_idle(session, I915_VK_OBJ_DEVICE, reader, reply);
		break;
	}

	/* Framing or native ownership refusal stops this immutable submission. */
	if (error != 0)
		return error;

	/* Succeeded: exactly one typed root operation completed. */
	return 0;
}

/* Creates a native protocol instance from the client's generated record and exact output identity. */
static int
create_instance(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkInstanceCreateInfo info;
	struct bcm2711_vulkan_object *object;
	uint64_t present;
	uint64_t allocator;
	uint64_t identity;
	uint32_t version;
	uint32_t major;
	uint32_t minor;
	int error;

	/* Required generated input is decoded into a command-local bounded arena. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 1 || reader->error != 0)
		return EINVAL;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkInstanceCreateInfo(reader, &session->arena, &info);
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
		return EINVAL;

	/* Native layers and extensions are absent; local WSI is stripped by the real client before this command. */
	if (info.sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO || info.flags != 0 ||
	    info.enabledLayerCount != 0 || info.enabledExtensionCount != 0)
		return ENOTSUP;
	version = VK_API_VERSION_1_0;
	if (info.pApplicationInfo != NULL) {
		if (info.pApplicationInfo->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO)
			return EINVAL;
		if (info.pApplicationInfo->apiVersion != 0)
			version = info.pApplicationInfo->apiVersion;
	}

	/* The private queue transport uses version 1.1, without a formal Vulkan conformance claim. */
	major = version >> 22;
	minor = (version >> 12) & 0x3ffU;
	if (major != 1 || minor > 1) {
		reply_identity(reply, VK_ERROR_INCOMPATIBLE_DRIVER, 0);
		return 0;
	}

	/* Publication acquires a distinct logical root instead of a global unowned instance token. */
	error = publish_root(session, I915_VK_OBJ_INSTANCE, identity, NULL, 0, 0, &object);
	if (error != 0)
		return error;
	reply_identity(reply, VK_SUCCESS, identity);

	/* Succeeded: this session owns the acknowledged native instance identity. */
	return 0;
}

/* Enumerates one stable native physical identity while keeping Vulkan count-only framing exact. */
static int
enumerate_physical(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *instance;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_root *root;
	uint64_t instance_id;
	uint64_t present;
	uint64_t array;
	uint64_t identity;
	uint32_t count;
	int error;

	/* The array's guest-chosen output ID is separate from its count-only query. */
	instance_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1 || array > 1 || (array != 0 && count != 1))
		return EINVAL;
	instance = bcm2711_vulkan_object_find(session, I915_VK_OBJ_INSTANCE, instance_id);
	if (instance == NULL)
		return EINVAL;
	identity = 0;

	/* Repeated enumeration can acknowledge the same identity only for this exact instance object. */
	if (array != 0) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || identity == 0)
			return EINVAL;
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity);
		if (object != NULL) {
			root = object->payload;
			if (root->parent != instance)
				return EEXIST;
		} else {
			error = publish_root(session, I915_VK_OBJ_PHYSICAL_DEVICE, identity, instance, 0, 0, &object);
			if (error != 0)
				return error;
		}
	}

	/* A count-only request returns one physical device and no array elements. */
	drv_i915_wire_reply_u32(reply, VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, array);
	if (array != 0)
		drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the bounded physical snapshot belongs to this native instance. */
	return 0;
}

/* Validates a logical device's real graphics queue and refuses every unimplemented optional feature. */
static int
create_device(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFeatures empty_features;
	const VkDeviceQueueCreateInfo *queue;
	struct bcm2711_vulkan_object *physical;
	struct bcm2711_vulkan_object *object;
	uint64_t parent_id;
	uint64_t present;
	uint64_t allocator;
	uint64_t identity;
	uint32_t priority;
	int different;
	int error;

	/* Parent identity and complete generated queue/feature records precede the output handle. */
	parent_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkDeviceCreateInfo(reader, &session->arena, &info);
	allocator = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || present != 1 || identity == 0)
		return EINVAL;
	physical = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PHYSICAL_DEVICE, parent_id);
	if (physical == NULL)
		return EINVAL;

	/* Native extensions/layers are intentionally absent from the client's remote device request. */
	if (info.sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || info.flags != 0 ||
	    info.enabledLayerCount != 0 || info.enabledExtensionCount != 0 ||
	    info.queueCreateInfoCount != 1 || info.pQueueCreateInfos == NULL)
		return ENOTSUP;
	queue = info.pQueueCreateInfos;
	if (queue->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || queue->flags != 0 ||
	    queue->queueFamilyIndex != 0 || queue->queueCount != VULKAN_NATIVE_QUEUES || queue->pQueuePriorities == NULL)
		return ENOTSUP;

	/* Floating priority bits are examined without using the kernel's unavailable FP register state. */
	kern_memcpy(&priority, queue->pQueuePriorities, sizeof(priority));
	if (priority != 0x80000000U && priority > 0x3f800000U)
		return EINVAL;

	/* Any nonzero optional feature fails rather than silently enabling an absent compiler/native path. */
	if (info.pEnabledFeatures != NULL) {
		kern_memset(&empty_features, 0, sizeof(empty_features));
		different = kern_memcmp(&empty_features, info.pEnabledFeatures, sizeof(empty_features));
		if (different != 0) {
			reply_identity(reply, VK_ERROR_FEATURE_NOT_PRESENT, 0);
			return 0;
		}
	}

	/* Retained physical ownership survives later withdrawal of the physical wire namespace. */
	error = publish_root(session, I915_VK_OBJ_DEVICE, identity, physical, VULKAN_NATIVE_QUEUES, 0, &object);
	if (error != 0)
		return error;
	reply_identity(reply, VK_SUCCESS, identity);

	/* Succeeded: this device owns one real graphics queue creation slot. */
	return 0;
}

/* Binds the actual GetDeviceQueue2 timeline record to a retained native queue exactly once. */
static int
device_queue(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *existing;
	struct bcm2711_vulkan_root *root;
	uint64_t device_id;
	uint64_t present;
	uint64_t chain;
	uint64_t chain_next;
	uint64_t identity;
	uint64_t output_present;
	uint32_t structure;
	uint32_t extension;
	uint32_t timeline;
	uint32_t flags;
	uint32_t family;
	uint32_t index;
	int error;

	/* The pinned private queue record carries an explicit nonzero completion domain. */
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	extension = drv_i915_wire_read_u32(reader);
	chain_next = drv_i915_wire_read_u64(reader);
	timeline = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	family = drv_i915_wire_read_u32(reader);
	index = drv_i915_wire_read_u32(reader);
	output_present = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1 || output_present != 1 || chain != 1 || chain_next != 0 ||
	    structure != VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2 || extension != VULKAN_TIMELINE_RECORD ||
	    flags != 0 || family != 0 || index != 0 || timeline == 0 || timeline >= 64 || identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;
	root = device->payload;
	if (root->queue_count != VULKAN_NATIVE_QUEUES)
		return EINVAL;

	/* Repeated lookup is idempotent only for the exact existing identity, parent, index and timeline. */
	existing = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUEUE, identity);
	if (existing != NULL) {
		root = existing->payload;
		if (root->parent != device || root->index != index || root->timeline != timeline)
			return EEXIST;
	} else {
		/* One native family/index has only one protocol identity in this logical device. */
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_QUEUE)
				continue;
			root = object->payload;
			if (root->parent == device && root->index == index)
				return EEXIST;
		}

		/* Complete payload/registry ownership before any IRQ-guarded producer can observe the new domain. */
		error = publish_root(session, I915_VK_OBJ_QUEUE, identity, device, 0, 0, &object);
		if (error != 0)
			return error;

		/* A domain cannot be recycled while a native queue or old common callback still owns it. */
		error = claim_timeline(session, timeline);
		if (error != 0) {
			/* This unpublished domain is zero, so rollback cannot clear another live queue's producer bit. */
			(void)bcm2711_vulkan_object_remove(session, I915_VK_OBJ_QUEUE, identity);
			return error;
		}

		/* Borrowed namespace lookups remain excluded by the controller mutex until domain ownership is complete. */
		root = object->payload;
		root->timeline = timeline;
	}

	/* This Vulkan command returns an output pointer and identity without a VkResult field. */
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the live queue owns the exact nonzero domain used by supervised submissions. */
	return 0;
}

/* Removes implicit physical/queue children before withdrawing their native parent identity. */
static int
destroy_root(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *parent;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *next;
	struct bcm2711_vulkan_root *root;
	enum i915_vk_object_kind child_kind;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Destruction accepts no remote allocation callback and never skips a truncated handle. */
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	parent = bcm2711_vulkan_object_find(session, kind, identity);
	if (parent == NULL)
		return EINVAL;
	child_kind = I915_VK_OBJ_PHYSICAL_DEVICE;
	if (kind == I915_VK_OBJ_DEVICE)
		child_kind = I915_VK_OBJ_QUEUE;

	/* Unvisited namespace entries keep their registry reference through every child destructor. */
	for (object = session->objects; object != NULL; object = next) {
		next = object->next;
		if (object->kind != child_kind)
			continue;
		root = object->payload;
		if (root->parent != parent)
			continue;
		error = bcm2711_vulkan_object_remove(session, child_kind, object->identity);
		if (error != 0)
			return error;
	}

	/* Independent prepared references keep removed queue/device payloads alive until actual native retirement. */
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: neither this native root nor its implicit children remain reachable by wire identity. */
	return 0;
}

/* Proves preceding synchronous native execution has retired before reporting a queue/device idle result. */
static int
wait_idle(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_render_device *controller;
	struct bcm2711_v3d_hardware *hardware;
	uint64_t identity;
	unsigned long enabled;
	VkResult status;

	/* Only a live typed native owner may wait on its preceding ordered work. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	/* The sole controller-serialized worker executes every preceding QueueSubmit to native retirement. */
	controller = session->render->device;
	hardware = &controller->space.native->hardware;
	enabled = spin_lock_irqsave(&hardware->guard);

	status = VK_SUCCESS;
	if (session->render->stopping || controller->worker.uncertain ||
	    !hardware->ready || hardware->faulted || hardware->job_busy)
		status = VK_ERROR_DEVICE_LOST;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* A loss or uncertain native owner is never hidden behind decoder progress. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);

	/* Succeeded: this typed idle query reports the actual serialized native retirement state. */
	return 0;
}

/* Constructs one retained parent edge before publishing a typed native root. */
static int
publish_root(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	struct bcm2711_vulkan_object *parent,
	uint32_t queues,
	uint32_t timeline,
	struct bcm2711_vulkan_object **object)
{
	struct bcm2711_vulkan_root *root;
	int error;

	/* A failed payload allocation acquires no parent or protocol identity. */
	root = kern_calloc(1, sizeof(*root));
	if (root == NULL)
		return ENOMEM;

	/* Parent ownership is established before any namespace can discover the new payload. */
	if (parent != NULL) {
		error = bcm2711_vulkan_object_retain(parent);
		if (error != 0) {
			kern_free(root);
			return error;
		}
	}

	/* A queue payload remembers the same completion domain protected by the native IRQ guard. */
	root->parent = parent;
	root->queue_count = queues;
	root->timeline = timeline;
	error = bcm2711_vulkan_object_publish(session, kind, identity, root, release_root, object);
	if (error != 0) {
		/* Failed publication returns the independently acquired parent edge without consuming a timeline. */
		(void)bcm2711_vulkan_object_release(parent);
		kern_free(root);
		return error;
	}

	/* Succeeded: the registry owns the payload and its independent parent edge. */
	return 0;
}

/* Releases a root's native domain only after every dependent/prepared object reference has retired. */
static int
release_root(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_root *root;
	struct bcm2711_vulkan_object *parent;
	int error;

	/* The remaining queue domain cannot be recycled before final root retirement. */
	root = payload;
	if (root->timeline != 0)
		release_timeline(session, root->timeline);
	parent = root->parent;
	kern_free(root);

	/* The removed parent may finally retire when its last native child releases this edge. */
	error = bcm2711_vulkan_object_release(parent);
	if (error != 0)
		return error;

	/* Succeeded: this root no longer owns a payload, parent or completion domain. */
	return 0;
}

/* Claims a completion domain only when neither a live queue nor an old supervised callback retains it. */
static int
claim_timeline(
	struct bcm2711_vulkan_session *session,
	uint32_t timeline)
{
	struct bcm2711_render_session *render;
	struct bcm2711_render_device *controller;
	struct bcm2711_render_request *request;
	uint64_t mask;
	unsigned long enabled;
	uint32_t slot;

	/* All queue publication shares the native IRQ guard with producer reservation and callback retirement. */
	render = session->render;
	controller = render->device;
	mask = UINT64_C(1) << timeline;
	enabled = spin_lock_irqsave(&controller->space.native->hardware.guard);

	if (render->stopping || controller->worker.uncertain ||
	    !controller->space.native->hardware.ready || controller->space.native->hardware.faulted) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EIO;
	}

	/* A retained removed queue still owns its domain until its last native reference ends. */
	if ((render->timelines & mask) != 0) {
		spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
		return EEXIST;
	}

	/* Even a withdrawn queue cannot hand an old RESERVED/QUEUED/ACTIVE/FINISHING callback's domain to a new identity. */
	for (slot = 0; slot < BCM2711_RENDER_REQUESTS; slot++) {
		request = &controller->worker.requests[slot];
		if (request->session == render && request->timeline == timeline) {
			spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);
			return EAGAIN;
		}
	}

	/* A freshly published native queue becomes the sole producer admission owner of this domain. */
	render->timelines |= mask;

	spin_unlock_irqrestore(&controller->space.native->hardware.guard, enabled);

	/* Succeeded: this complete native queue owns one unique guarded producer domain. */
	return 0;
}

/* Withdraws queue producer admission while old callbacks independently keep their slot/domain identity. */
static void
release_timeline(
	struct bcm2711_vulkan_session *session,
	uint32_t timeline)
{
	struct bcm2711_render_session *render;
	unsigned long enabled;
	uint64_t mask;

	/* Marker lifetime remains in its request slot after queue ownership is withdrawn. */
	render = session->render;
	mask = UINT64_C(1) << timeline;
	enabled = spin_lock_irqsave(&render->device->space.native->hardware.guard);

	render->timelines &= ~mask;

	spin_unlock_irqrestore(&render->device->space.native->hardware.guard, enabled);

	/* Succeeded: no new producer can reserve this withdrawn queue's domain. */
	return;
}

/* Frames a Vulkan creation result without publishing a nonzero output identity after failure. */
static void
reply_identity(
	struct i915_wire_writer *reply,
	VkResult status,
	uint64_t identity)
{
	/* The exact library framing includes an output pointer even for an unsuccessful VkResult. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the response distinguishes failure from an acknowledged live native identity. */
	return;
}
