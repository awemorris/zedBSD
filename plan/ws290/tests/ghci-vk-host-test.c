/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the GHCI 1.0 driver's Vulkan executor (vk/) against
 * the GPU model (ghci-model.c) and the kernel stubs (ghci-stubs.c): the
 * capset, the registration, the answers of each profile and of each
 * capability flag, the format features from FMT and from the table, the
 * devices and their GHCI sessions (the sixteenth fails), memory, buffers
 * and images, the recording, fences, semaphores, events, the empty
 * submission, the job markers and frames, the GPU core's recovery
 * calls (design section 14, the scenes of p007), and the display's
 * copied presents by the driver's copy program on a GPU without the
 * blitter.
 *
 * The executor is driven through the node's operations with frames of
 * the wire built by hand as libvulkan encodes them (uapi/gpu-op.h); the
 * replies are read back from the reply blob.  Every expected value is
 * written here by hand from the specification and the driver's design.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/gpu.h"
#include "drivers/gpu/ghci/ghci.h"
#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "drivers/gpu/ghci/vk/vk.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "uapi/errno.h"
#include "uapi/gpu.h"
#include "uapi/gpu-allocation.h"
#include "uapi/gpu-display.h"
#include "uapi/gpu-fence.h"
#include "uapi/gpu-job.h"
#include "uapi/gpu-op.h"
#include "ghci-model.h"
#include "ghci-stubs.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The PCI IDs the scenes give the device (the KC705 board). */
#define TEST_PCI_VENDOR			0x10eeU
#define TEST_PCI_DEVICE			0x7024U

/* The reply blob: its bytes, and where the trailer (the version probe's 20 bytes) goes. */
#define REPLY_BYTES			4096U
#define TRAILER_AT			(REPLY_BYTES - 20U)

/* The most bytes of one hand-built frame. */
#define FRAME_BYTES			8192U

/* The version the trailer answers: 1.1 on the wire. */
#define WIRE_VERSION			0x00401000U

/* The identities the scenes give their objects (libvulkan chooses them; any nonzero value). */
#define ID_INSTANCE			0x100U
#define ID_PHYSICAL			0x101U
#define ID_DEVICE			0x200U
#define ID_QUEUE			0x300U
#define ID_MEMORY			0x400U
#define ID_BUFFER			0x500U
#define ID_IMAGE			0x600U
#define ID_FENCE			0x700U
#define ID_SEMAPHORE			0x800U
#define ID_EVENT			0x900U
#define ID_POOL				0xA00U
#define ID_COMMANDS			0xB00U

/*
 * One frame being built: the bytes and how many are used.  It lives on
 * the stack of a scene.
 */
struct frame {
	uint8_t bytes[FRAME_BYTES];
	size_t used;
};

/* One open node session of a scene: the core's opaque session and its reply blob's view. */
struct node {
	void *session;
	uint32_t reply_id;
	uint8_t *reply;
};

/* The counts of the checks. */
static unsigned checks_passed;
static unsigned checks_failed;

/* The device of the scene; one at a time. */
static struct ghci_device device;

/* Whether the sleep hook runs the service thread's work (the core's reset), and that it is running. */
static int service_auto;
static int service_running;

/*
 * What the next start gives the device besides the profile: capability
 * bits taken out of DEVICE_CAPS (test_caps_clear, for a variant of a
 * profile).
 */
static uint32_t test_caps_clear;

/* The completions the scenes hand the executor: distinct addresses the stub counts. */
static char completions[8];

/*
 * The session whose completion the completing hook reports, as the
 * service thread would between its retirement and its report, and the
 * sleep of the waiter it ran in (0 until then).
 */
static struct ghci_vk_session *completing_session;
static unsigned completing_sleep;

/*
 * What the clear hook does at the sleeps of an allocation's wait for its
 * clear (clear_mode), the sleeps it has seen, the session the clear runs
 * in, and the guard that keeps it from running inside a recovery's own
 * sleeps.
 */
enum clear_mode {
	CLEAR_BUSY = 1,
	CLEAR_RESET,
	CLEAR_CULPRIT
};
static enum clear_mode clear_mode;
static unsigned clear_sleeps;
static struct ghci_vm *clear_vm;
static int clear_running;

static void expect(int held, const char *name);
static int start(enum model_profile profile);
static void stop(const char *name);
static void model_irq(void *argument);
static void sleep_hook(unsigned sleep, void *argument);
static void completing_hook(unsigned sleep, void *argument);
static int fake_interrupt_take(struct ghci_device *target);
static int fake_interrupt_release(struct ghci_device *target);
static int fake_bus_master(struct ghci_device *target, int on);
static int node_open(struct node *node);
static void node_close(struct node *node);
static void put32(struct frame *frame, uint32_t value);
static void put64(struct frame *frame, uint64_t value);
static void begin(struct frame *frame, const struct node *node, uint32_t opcode, uint32_t reply);
static void trail(struct node *node, struct frame *frame);
static int run(struct node *node, struct frame *frame);
static int run_queued(struct node *node, struct frame *frame, uint32_t timeline, struct drv_gpu_completion *completion);
static uint32_t reply32(const struct node *node, size_t at);
static uint64_t reply64(const struct node *node, size_t at);
static int trailer_ok(const struct node *node);
static int create_device(struct node *node, uint64_t handle, const VkPhysicalDeviceFeatures *features, uint32_t *result);
static int get_queue(struct node *node, uint64_t handle, uint32_t timeline, uint64_t queue);
static int destroy(struct node *node, uint32_t opcode, uint64_t handle, uint64_t identity);
static int allocate_memory(struct node *node, uint64_t handle, uint64_t bytes, uint32_t import, uint64_t memory, uint32_t *result);
static int create_image(struct node *node, uint64_t handle, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint64_t image, uint32_t *result);
static int simple_create(struct node *node, uint32_t opcode, uint32_t type, uint64_t handle, uint32_t flags, uint64_t identity, uint32_t *result);
static int object_query(struct node *node, uint32_t opcode, uint64_t handle, uint64_t identity, uint32_t *result);
static int submit(struct node *node, uint64_t queue, uint64_t commands, uint64_t fence);
static int allocate_buffers(struct node *node, uint64_t handle, uint64_t pool, const uint64_t *identities, uint32_t count);
static int ticket_passed(const struct ghci_ticket *ticket);
static int log_names_no_session(void);
static void scene_capset(void);
static void scene_registration(void);
static void scene_profiles(void);
static void scene_profile(enum model_profile profile, const char *name, const unsigned *features, unsigned feature_count, uint32_t device_type, uint32_t attachments, uint32_t shared, uint32_t samples);
static void scene_formats(void);
static void scene_flags(void);
static void scene_optional(void);
static void flag_case(const char *name, uint32_t device_caps, uint32_t feature, uint32_t fmt0, uint32_t max_mrt, uint32_t lds, const unsigned *features, unsigned feature_count, const size_t *limits, unsigned limit_count, const uint32_t *formats, unsigned format_count, uint32_t lost);
static void scene_devices(void);
static void scene_memory(void);
static void scene_recording(void);
static void scene_sync(void);
static void scene_jobs(void);
static void scene_completions(void);
static void scene_recovery(void);
static void scene_core_reset(void);
static void scene_discrete(void);
static void scene_discrete_clear(void);
static void clear_hook(unsigned sleep, void *argument);
static int allocate_typed(struct node *node, uint64_t handle, uint64_t bytes, uint32_t type, int exported, uint64_t memory, uint32_t *result);
static int all_bytes(const uint8_t *bytes, uint64_t count, uint8_t value);
static void scene_display_copy(void);
static void display_copy_case(enum model_profile profile, uint32_t caps_clear, const char *name);
static uint32_t display_routes(void *open, uint64_t generation);
static int display_claim(void **open, uint64_t *lease, uint64_t *generation);
static struct ghci_resource *display_storage(void *open, uint64_t bytes, uint32_t seed);
static int display_send(void *open, uint64_t lease, uint64_t generation, struct ghci_resource *storage, uint32_t stride);
static void display_end(void *open, uint64_t lease);
static int display_holds(struct ghci_allocation *frame, struct ghci_resource *storage, uint32_t stride);
static int display_dispatch_check(unsigned mark, uint32_t stride, const struct ghci_mapping *frame_map, const struct ghci_allocation *storage);

/* The operations of the fake bus; they live as long as the test. */
static const struct ghci_bus_ops fake_bus_ops = {
	fake_interrupt_take,
	fake_interrupt_release,
	fake_bus_master
};

/*
 * Runs the scenes and reports.
 */
int
main(void)
{
	/* Keeps the driver's log out of the report. */
	stub.quiet = 1;

	/* The scenes. */
	scene_capset();
	scene_registration();
	scene_profiles();
	scene_formats();
	scene_flags();
	scene_devices();
	scene_memory();
	scene_recording();
	scene_sync();
	scene_jobs();
	scene_completions();
	scene_recovery();
	scene_core_reset();
	scene_discrete();
	scene_discrete_clear();
	scene_display_copy();
	model_fini();

	/* The counts. */
	printf("ghci-vk-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U) {
		printf("FAIL ghci-vk-host-test\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS ghci-vk-host-test\n");
	return 0;
}

/* The service thread is not started on the host: the scenes run its steps. */
int
ghci_service_start(
	struct ghci_device *target)
{
	(void)target;
	return 0;
}

/* The poll thread is not started on the host. */
int
ghci_poll_start(
	struct ghci_device *target)
{
	(void)target;
	return 0;
}

/* Counts a check, and reports one that failed by name. */
static void
expect(
	int held,
	const char *name)
{
	/* A check that held is only counted. */
	if (held) {
		checks_passed++;
		return;
	}

	/* A failed check is reported by name. */
	checks_failed++;
	printf("FAIL %s\n", name);
}

/* Starts a device of a profile, with the driver's lock order and forbidden waits.  Returns what the start returned. */
static int
start(
	enum model_profile profile)
{
	static const char *const order[] = {
		"ghci device", "ghci display", "ghci ring", "ghci interrupt"
	};
	int error;

	/* The model (less the bits the scene takes out) and the stubs, with the rules the driver must keep. */
	model_init(profile, NULL);
	model.window[GHCI_REG_DEVICE_CAPS / 4U] &= ~test_caps_clear;
	stub_reset();
	stub_lock_order(order, 4U);
	stub_forbid_wait("ghci fence", "ghci device");
	stub_forbid_wait("ghci fence", "ghci ring");
	stub_forbid_wait("ghci gate", "ghci device");
	stub_forbid_wait("ghci gate", "ghci ring");
	stub_set_sleep_hook(sleep_hook, NULL);
	service_auto = 0;
	service_running = 0;

	/* The device as the bus glue leaves it. */
	memset(&device, 0, sizeof(device));
	device.registers = stub_bar0();
	device.register_bytes = 4096U;
	device.bus_ops = &fake_bus_ops;
	device.pci_vendor = TEST_PCI_VENDOR;
	device.pci_device = TEST_PCI_DEVICE;
	error = ghci_device_start(&device);
	return error;
}

/* Stops the scene's device and checks how the driver locked and waited. */
static void
stop(
	const char *name)
{
	char label[128];
	int error;

	/* The device stops. */
	if (device.state != GHCI_STATE_DETACHED && device.fence_page != NULL) {
		error = ghci_device_stop(&device);
		snprintf(label, sizeof(label), "%s: stop", name);
		expect(error == 0, label);
	}

	/* The locks and the waits. */
	snprintf(label, sizeof(label), "%s: lock order", name);
	expect(stub.order_violations == 0U, label);
	snprintf(label, sizeof(label), "%s: waits", name);
	expect(stub.wait_violations == 0U, label);
	snprintf(label, sizeof(label), "%s: nothing held", name);
	expect(stub.held_count == 0U && stub.double_locks == 0U && stub.unlock_errors == 0U, label);
}

/* The model's interrupt line, wired to the driver's handler unless the driver holds its interrupt lock. */
static void
model_irq(
	void *argument)
{
	if (stub_holds("ghci interrupt"))
		return;
	(void)ghci_cp_interrupt(argument);
}

/* Each sleep of the driver: the service thread's reset for the core, when a scene asks for it. */
static void
sleep_hook(
	unsigned sleep,
	void *argument)
{
	(void)sleep;
	(void)argument;

	/* The service thread's work, never inside itself. */
	if (!service_auto || service_running || !device.reset_asked)
		return;
	service_running = 1;
	ghci_service_step(&device, GHCI_WORK_RESET);
	service_running = 0;
}

/*
 * Each sleep of a waiter while the completing hook is set: the first one
 * reports the completion the scene left in flight, as the service thread
 * does once drv_gpu_complete has returned (the count lowered and the
 * waiter woken under irq_lock).
 */
static void
completing_hook(
	unsigned sleep,
	void *argument)
{
	struct ghci_vk *vk;
	unsigned long irq;

	(void)argument;

	/* Only once, and only for a session left in flight. */
	if (completing_sleep != 0U || completing_session == NULL)
		return;
	completing_sleep = sleep;

	/* The report: one fewer completing, the last one wakes the waiter. */
	vk = device.vk;
	irq = spin_lock_irqsave(&device.irq_lock);

	completing_session->completing--;
	if (completing_session->completing == 0U)
		waitq_wake_all(&vk->completing_wake);

	spin_unlock_irqrestore(&device.irq_lock, irq);
}

/* Takes the interrupt: the model's line. */
static int
fake_interrupt_take(
	struct ghci_device *target)
{
	target->polled = 0;
	model_set_irq_handler(model_irq, target);
	return 0;
}

/* Lets the interrupt go. */
static int
fake_interrupt_release(
	struct ghci_device *target)
{
	(void)target;
	model_set_irq_handler(NULL, NULL);
	return 0;
}

/* Turns bus mastering on or off in the model. */
static int
fake_bus_master(
	struct ghci_device *target,
	int on)
{
	(void)target;
	model_set_bus_master(on);
	return 0;
}

/* Opens a node session and its reply blob.  Returns 0 or the error. */
static int
node_open(
	struct node *node)
{
	struct gpu_blob_create request;
	struct ghci_resource *resource;
	void *object;
	int error;

	/* The session. */
	memset(node, 0, sizeof(*node));
	error = device.ops.open(&device, &node->session);
	if (error != 0)
		return error;

	/* The reply blob, mappable as libvulkan makes it. */
	memset(&request, 0, sizeof(request));
	request.bytes = REPLY_BYTES;
	request.flags = GPU_BLOB_MAPPABLE;
	error = device.ops.blob_create(&device, node->session, &request, &object, &node->reply_id);
	if (error != 0)
		return error;
	resource = object;
	node->reply = resource->allocation->view;
	return 0;
}

/* Closes a node session as the core does: the drain, then the close (the core destroys the resources). */
static void
node_close(
	struct node *node)
{
	device.ops.commands->drain(&device, node->session);
	device.ops.close(&device, node->session);
	node->session = NULL;
}

/* Appends a 32-bit word, little-endian. */
static void
put32(
	struct frame *frame,
	uint32_t value)
{
	memcpy(frame->bytes + frame->used, &value, 4U);
	frame->used += 4U;
}

/* Appends a 64-bit word, little-endian. */
static void
put64(
	struct frame *frame,
	uint64_t value)
{
	memcpy(frame->bytes + frame->used, &value, 8U);
	frame->used += 8U;
}

/*
 * Starts a frame as libvulkan's transaction does: the reply stream (the
 * whole blob), then the header of the command (its number and the reply
 * flag).
 */
static void
begin(
	struct frame *frame,
	const struct node *node,
	uint32_t opcode,
	uint32_t reply)
{
	/* GPU_OP_SET_REPLY_STREAM: [u64 1][u32 resource][u64 offset][u64 capacity]. */
	frame->used = 0U;
	put32(frame, GPU_OP_SET_REPLY_STREAM);
	put32(frame, 0U);
	put64(frame, 1U);
	put32(frame, node->reply_id);
	put64(frame, 0U);
	put64(frame, REPLY_BYTES);

	/* The command's header. */
	put32(frame, opcode);
	put32(frame, reply);
}

/*
 * Ends a frame with the trailer (the seek to the blob's last 20 bytes and
 * the version probe), clears the replies, and runs it through the node's
 * command.  Returns what the command returned.
 */
static int
run(
	struct node *node,
	struct frame *frame)
{
	int error;

	/* The trailer, then the frame. */
	trail(node, frame);
	error = device.ops.command(&device, node->session, frame->bytes, (uint32_t)frame->used);
	return error;
}

/*
 * Ends a frame with the trailer and clears the replies, as run() does:
 * GPU_OP_SEEK_REPLY_STREAM to the blob's last 20 bytes, then
 * GPU_OP_ENUMERATE_INSTANCE_VERSION with its output.
 */
static void
trail(
	struct node *node,
	struct frame *frame)
{
	put32(frame, GPU_OP_SEEK_REPLY_STREAM);
	put32(frame, 0U);
	put64(frame, TRAILER_AT);
	put32(frame, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	put32(frame, GPU_OP_FLAG_REPLY);
	put64(frame, 1U);
	memset(node->reply, 0, REPLY_BYTES);
}

/*
 * Ends a frame with the trailer and queues it with a completion, as
 * libvulkan's transaction does with the notification (the context's
 * fence flag on timeline 0).  Returns what the submit returned.
 */
static int
run_queued(
	struct node *node,
	struct frame *frame,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	int error;

	/* The trailer, then the queued frame. */
	trail(node, frame);
	error = device.ops.commands->submit(&device, node->session, frame->bytes, (uint32_t)frame->used,
	    GPU_COMMAND_CONTEXT_FENCE, timeline, completion);
	return error;
}

/* Reads a 32-bit word of the replies. */
static uint32_t
reply32(
	const struct node *node,
	size_t at)
{
	uint32_t value;

	memcpy(&value, node->reply + at, 4U);
	return value;
}

/* Reads a 64-bit word of the replies. */
static uint64_t
reply64(
	const struct node *node,
	size_t at)
{
	uint64_t value;

	memcpy(&value, node->reply + at, 8U);
	return value;
}

/* Reports whether the trailer says the frame is complete: [137][VK_SUCCESS][u64 1][version]. */
static int
trailer_ok(
	const struct node *node)
{
	if (reply32(node, TRAILER_AT) != GPU_OP_ENUMERATE_INSTANCE_VERSION)
		return 0;
	if (reply32(node, TRAILER_AT + 4U) != 0U || reply64(node, TRAILER_AT + 8U) != 1U)
		return 0;
	if (reply32(node, TRAILER_AT + 16U) != WIRE_VERSION)
		return 0;
	return 1;
}

/*
 * vkCreateDevice as libvulkan sends it: one queue of family 0 with one
 * priority, no layer, no extension, and the features when given.  The
 * reply's VkResult goes to result.  Returns what the frame returned.
 */
static int
create_device(
	struct node *node,
	uint64_t handle,
	const VkPhysicalDeviceFeatures *features,
	uint32_t *result)
{
	struct frame frame;
	const VkBool32 *words;
	unsigned index;
	int error;

	/* [physical][u64 1] and VkDeviceCreateInfo: sType, pNext, flags, the queues. */
	begin(&frame, node, GPU_OP_CREATE_DEVICE, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, 1U);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, 1U);
	put64(&frame, 1U);
	put32(&frame, 0x3f800000U);

	/* No layer, no extension, then the features. */
	put32(&frame, 0U);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 0U);
	if (features != NULL) {
		put64(&frame, 1U);
		words = (const VkBool32 *)(const void *)features;
		for (index = 0U; index < sizeof(*features) / sizeof(VkBool32); index++)
			put32(&frame, words[index]);
	} else {
		put64(&frame, 0U);
	}

	/* The tail: no allocator, the identity. */
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, handle);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/* vkGetDeviceQueue2 with the timeline record.  Returns what the frame returned. */
static int
get_queue(
	struct node *node,
	uint64_t handle,
	uint32_t timeline,
	uint64_t queue)
{
	struct frame frame;

	/* [device][u64 1][sType][u64 1][timeline record][u64 0][timeline][flags][family][index][u64 1][queue]. */
	begin(&frame, node, GPU_OP_GET_DEVICE_QUEUE2, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
	put64(&frame, 1U);
	put32(&frame, GPU_OP_RECORD_QUEUE_TIMELINE);
	put64(&frame, 0U);
	put32(&frame, timeline);
	put32(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, queue);
	return run(node, &frame);
}

/* A destroy (the destroy form; vkDestroyDevice's is [device][u64 0]).  Returns what the frame returned. */
static int
destroy(
	struct node *node,
	uint32_t opcode,
	uint64_t handle,
	uint64_t identity)
{
	struct frame frame;

	/* The device, the object (but for the device itself), no allocator. */
	begin(&frame, node, opcode, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	if (opcode != GPU_OP_DESTROY_DEVICE)
		put64(&frame, identity);
	put64(&frame, 0U);
	return run(node, &frame);
}

/* vkAllocateMemory of type 0, importing a resource when import is not 0.  Returns what the frame returned. */
static int
allocate_memory(
	struct node *node,
	uint64_t handle,
	uint64_t bytes,
	uint32_t import,
	uint64_t memory,
	uint32_t *result)
{
	struct frame frame;
	int error;

	/* [device][u64 1][sType][u64 chain] and the import record. */
	begin(&frame, node, GPU_OP_ALLOCATE_MEMORY, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	if (import != 0U) {
		put64(&frame, 1U);
		put32(&frame, GPU_OP_RECORD_IMPORT_RESOURCE);
		put64(&frame, 0U);
		put32(&frame, import);
	} else {
		put64(&frame, 0U);
	}

	/* [u64 size][u32 type][u64 0][u64 1][memory]. */
	put64(&frame, bytes);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, memory);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/* vkCreateImage of a 2D image, optimal tiling, sampled and copied.  Returns what the frame returned. */
static int
create_image(
	struct node *node,
	uint64_t handle,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	uint32_t layers,
	uint64_t image,
	uint32_t *result)
{
	struct frame frame;
	int error;

	/* [device][u64 1] and VkImageCreateInfo. */
	begin(&frame, node, GPU_OP_CREATE_IMAGE, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, VK_IMAGE_TYPE_2D);
	put32(&frame, format);
	put32(&frame, width);
	put32(&frame, height);
	put32(&frame, 1U);
	put32(&frame, levels);
	put32(&frame, layers);
	put32(&frame, VK_SAMPLE_COUNT_1_BIT);
	put32(&frame, VK_IMAGE_TILING_OPTIMAL);
	put32(&frame, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	put32(&frame, VK_SHARING_MODE_EXCLUSIVE);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put32(&frame, VK_IMAGE_LAYOUT_UNDEFINED);

	/* The tail. */
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, image);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/*
 * A create of the form [device][u64 1][sType][u64 0][u32 flags] and the
 * tail: a fence, a semaphore or an event, or a command pool (whose flags
 * word is followed by the family, 0).  Returns what the frame returned.
 */
static int
simple_create(
	struct node *node,
	uint32_t opcode,
	uint32_t type,
	uint64_t handle,
	uint32_t flags,
	uint64_t identity,
	uint32_t *result)
{
	struct frame frame;
	int error;

	/* The create info. */
	begin(&frame, node, opcode, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, type);
	put64(&frame, 0U);
	put32(&frame, flags);
	if (opcode == GPU_OP_CREATE_COMMAND_POOL)
		put32(&frame, 0U);

	/* The tail. */
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, identity);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/* A query of the form [device][object] -> [result]: a fence's or an event's status, or an event set or reset. */
static int
object_query(
	struct node *node,
	uint32_t opcode,
	uint64_t handle,
	uint64_t identity,
	uint32_t *result)
{
	struct frame frame;
	int error;

	/* The device and the object. */
	begin(&frame, node, opcode, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, identity);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/* vkQueueSubmit of one submission with one command buffer (none for 0) and a fence (0 for none). */
static int
submit(
	struct node *node,
	uint64_t queue,
	uint64_t commands,
	uint64_t fence)
{
	struct frame frame;

	/* [queue][u32 1][u64 1] and VkSubmitInfo: no waits, the command buffer, no signals. */
	begin(&frame, node, GPU_OP_QUEUE_SUBMIT, GPU_OP_FLAG_REPLY);
	put64(&frame, queue);
	put32(&frame, 1U);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_SUBMIT_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 0U);
	if (commands != 0U) {
		put32(&frame, 1U);
		put64(&frame, 1U);
		put64(&frame, commands);
	} else {
		put32(&frame, 0U);
		put64(&frame, 0U);
	}
	put32(&frame, 0U);
	put64(&frame, 0U);

	/* The fence. */
	put64(&frame, fence);
	return run(node, &frame);
}

/* vkAllocateCommandBuffers of primary buffers from a pool on a device.  Returns what the frame returned. */
static int
allocate_buffers(
	struct node *node,
	uint64_t handle,
	uint64_t pool,
	const uint64_t *identities,
	uint32_t count)
{
	struct frame frame;
	uint32_t index;

	/* [device][u64 1][VkCommandBufferAllocateInfo][u64 count][count x identity]. */
	begin(&frame, node, GPU_OP_ALLOCATE_COMMAND_BUFFERS, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(&frame, 0U);
	put64(&frame, pool);
	put32(&frame, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	put32(&frame, count);
	put64(&frame, count);
	for (index = 0U; index < count; index++)
		put64(&frame, identities[index]);
	return run(node, &frame);
}

/* Reports whether a ticket has passed. */
static int
ticket_passed(
	const struct ghci_ticket *ticket)
{
	return ghci_cp_ticket(&device, ticket) == GHCI_TICKET_DONE;
}

/*
 * Reports whether the submission log names no session: after the core's
 * reset only the driver's own fences are in it.
 */
static int
log_names_no_session(void)
{
	unsigned index;

	for (index = 0U; index < device.submit_count; index++) {
		if (device.submit_log[(device.submit_first + index) % GHCI_SUBMIT_LOG].owner != NULL)
			return 0;
	}
	return 1;
}

/*
 * The capset: id 4, version 0, 176 bytes with the wire and XML versions,
 * the timelines word, the vendor suffix and the native word.
 */
static void
scene_capset(void)
{
	struct gpu_capset capset;
	uint32_t word;
	int error;

	/* A device that runs shaders. */
	error = start(MODEL_KC705_UMA);
	expect(error == 0, "capset: start");

	/* The capset libvulkan asks for. */
	memset(&capset, 0, sizeof(capset));
	capset.capset_id = 4U;
	capset.capset_version = 0U;
	capset.capacity = GPU_CAPSET_MAX;
	error = device.ops.get_capset(&device, NULL, &capset);
	expect(error == 0 && capset.bytes == 176U, "capset: 176 bytes");

	/* Values referenced: zedBSD userland/desktop/libvulkan/context.c (the words of the native capset). */
	memcpy(&word, capset.data + 0U, 4U);
	expect(word == 1U, "capset: wire version 1");
	memcpy(&word, capset.data + 4U, 4U);
	expect(word == ((1U << 22) | (3U << 12) | 269U), "capset: XML 1.3.269");
	memcpy(&word, capset.data + 152U, 4U);
	expect(word == 1U, "capset: timelines");
	memcpy(&word, capset.data + 160U, 4U);
	expect(word == 0x5a424453U, "capset: vendor magic");
	memcpy(&word, capset.data + 164U, 4U);
	expect(word == 7U, "capset: vendor flags");
	memcpy(&word, capset.data + 168U, 4U);
	expect(word == 0x5a4e4154U, "capset: native magic");
	memcpy(&word, capset.data + 172U, 4U);
	expect(word == 0U, "capset: no optional queue");

	/* Another capset is refused. */
	capset.capset_id = 3U;
	error = device.ops.get_capset(&device, NULL, &capset);
	expect(error == ENOTSUP, "capset: other id refused");
	stop("capset");
}

/*
 * The registration: the executor's capabilities on a device that runs
 * shaders, accepted by the core's checks; none on a device without a
 * shader unit; the executor on flat and PROT devices too.
 */
static void
scene_registration(void)
{
	uint32_t wanted;
	int error;

	/* The capabilities of the resources and of the executor. */
	wanted = GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE |
	    GPU_CAP_ALLOCATION_SHARE | GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_FENCE |
	    GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
	error = start(MODEL_KC705_UMA);
	expect(error == 0 && stub.registrations == 1U && stub.register_errors == 0U, "registration: accepted");
	expect(device.ops.capabilities == (wanted | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS), "registration: capabilities with the display");
	expect(device.ops.recovery != NULL && device.ops.recovery->isolate != NULL, "registration: recovery");
	stop("registration");

	/* No shader unit: no executor. */
	error = start(MODEL_NO_SU);
	expect(error == 0 && device.executor_ops == NULL, "registration: no-su has no executor");
	expect((device.ops.capabilities & GPU_CAP_COMMAND) == 0U, "registration: no-su has no command");
	stop("registration no-su");

	/* A flat device (without DISP2) and a PROT device (with it) run Vulkan too. */
	error = start(MODEL_FLAT);
	expect(error == 0 && device.ops.capabilities == wanted, "registration: flat has the executor");
	stop("registration flat");
	error = start(MODEL_PROT);
	expect(error == 0 && device.ops.capabilities == (wanted | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS), "registration: prot has the executor");
	stop("registration prot");
}

/*
 * The answers of each profile through the wire: the features, the
 * properties with a few limits, the queue family and the memory types.
 */
static void
scene_profiles(void)
{
	static const unsigned base[] = { 0U, 1U, 11U, 12U, 13U, 23U, 25U, 26U };
	static const unsigned kc705[] = { 0U, 1U, 11U, 12U, 13U, 20U, 23U, 25U, 26U };
	static const unsigned full[] = { 0U, 1U, 3U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 20U, 23U, 25U, 26U };
	static const unsigned discrete[] = { 0U, 1U, 3U, 7U, 9U, 10U, 11U, 12U, 13U, 20U, 23U, 25U, 26U };

	/*
	 * The true features by their place in VkPhysicalDeviceFeatures (0
	 * robustBufferAccess, 1 fullDrawIndexUint32, 3 independentBlend, 7
	 * dualSrcBlend, 8 logicOp, 9 multiDrawIndirect, 10
	 * drawIndirectFirstInstance, 11 depthClamp, 12 depthBiasClamp, 13
	 * fillModeNonSolid, 20 textureCompressionETC2, 23
	 * occlusionQueryPrecise, 25 vertexPipelineStoresAndAtomics, 26
	 * fragmentStoresAndAtomics); the colour attachments, the shared
	 * memory and the sample counts of the profile's registers.
	 */
	scene_profile(MODEL_KC705_UMA, "kc705-uma", kc705, 9U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 4U, 4096U, 1U);
	scene_profile(MODEL_KC705_DISCRETE, "kc705-discrete", kc705, 9U, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 4U, 4096U, 1U);
	scene_profile(MODEL_MINIMAL, "minimal", base, 8U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1U, 0U, 1U);
	scene_profile(MODEL_FULL, "full", full, 14U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 8U, 16384U, 5U);
	scene_profile(MODEL_FLAT, "flat", base, 8U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 4U, 4096U, 1U);
	scene_profile(MODEL_PROT, "prot", kc705, 9U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 4U, 4096U, 1U);
	scene_profile(MODEL_NO_BLT, "no-blt", kc705, 9U, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 4U, 4096U, 1U);
	scene_profile(MODEL_DISCRETE, "discrete", discrete, 13U, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 8U, 16384U, 5U);
}

/* The answers of one profile, against the values the caller wrote by hand. */
static void
scene_profile(
	enum model_profile profile,
	const char *name,
	const unsigned *features,
	unsigned feature_count,
	uint32_t device_type,
	uint32_t attachments,
	uint32_t shared,
	uint32_t samples)
{
	struct i915_wire_reader reader;
	struct i915_wire_arena arena;
	static uint8_t scratch[16384];
	VkPhysicalDeviceFeatures got;
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties family;
	VkPhysicalDeviceMemoryProperties memory;
	struct frame frame;
	struct node node;
	const VkBool32 *words;
	char label[160];
	unsigned index;
	unsigned at;
	uint32_t period;
	int wanted;
	int matched;
	int error;

	/* The device and a node session. */
	error = start(profile);
	if (error == 0)
		error = node_open(&node);
	snprintf(label, sizeof(label), "%s: start", name);
	expect(error == 0, label);
	if (error != 0) {
		stop(name);
		return;
	}
	memset(&reader, 0, sizeof(reader));
	memset(&arena, 0, sizeof(arena));
	arena.base = scratch;
	arena.size = sizeof(scratch);

	/* The instance, with no create info. */
	begin(&frame, &node, GPU_OP_CREATE_INSTANCE, GPU_OP_FLAG_REPLY);
	put64(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, ID_INSTANCE);
	error = run(&node, &frame);
	snprintf(label, sizeof(label), "%s: instance", name);
	expect(error == 0 && reply32(&node, 4U) == 0U && reply64(&node, 16U) == ID_INSTANCE && trailer_ok(&node), label);

	/* The features: exactly the ones the caller names are true. */
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_FEATURES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put64(&frame, 1U);
	error = run(&node, &frame);
	reader.base = node.reply + 12U;
	reader.size = REPLY_BYTES - 12U;
	reader.offset = 0U;
	i915_vkc_dec_VkPhysicalDeviceFeatures(&reader, &arena, &got);
	words = (const VkBool32 *)(const void *)&got;
	matched = error == 0 && reply64(&node, 4U) == 1U;
	for (index = 0U; index < sizeof(got) / sizeof(VkBool32); index++) {
		wanted = 0;
		for (at = 0U; at < feature_count; at++) {
			if (features[at] == index)
				wanted = 1;
		}
		if ((words[index] != VK_FALSE) != wanted)
			matched = 0;
	}
	snprintf(label, sizeof(label), "%s: features", name);
	expect(matched, label);

	/* The properties: the version, the IDs, the type and the limits the registers decide. */
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put64(&frame, 1U);
	error = run(&node, &frame);
	reader.base = node.reply + 12U;
	reader.offset = 0U;
	arena.used = 0U;
	i915_vkc_dec_VkPhysicalDeviceProperties(&reader, &arena, &properties);
	memcpy(&period, &properties.limits.timestampPeriod, 4U);
	snprintf(label, sizeof(label), "%s: properties", name);
	expect(error == 0 &&
	       properties.apiVersion == WIRE_VERSION &&
	       properties.vendorID == TEST_PCI_VENDOR &&
	       properties.deviceID == TEST_PCI_DEVICE &&
	       properties.deviceType == device_type, label);
	snprintf(label, sizeof(label), "%s: limits", name);
	expect(properties.limits.maxColorAttachments == attachments &&
	       properties.limits.maxComputeSharedMemorySize == shared &&
	       properties.limits.framebufferColorSampleCounts == samples &&
	       properties.limits.maxTexelBufferElements == 4194304U &&
	       properties.limits.maxDescriptorSetSampledImages == 96U &&
	       properties.limits.maxPerStageDescriptorSamplers == 16U &&
	       properties.limits.maxBoundDescriptorSets == 4U &&
	       properties.limits.maxPushConstantsSize == 128U &&
	       properties.limits.maxImageDimension3D == 2048U &&
	       properties.limits.minStorageBufferOffsetAlignment == 4U &&
	       properties.limits.bufferImageGranularity == 256U &&
	       period == 0x41200000U, label);

	/* The one queue family: graphics, compute and transfer, one queue, 64-bit timestamps. */
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put64(&frame, 1U);
	put32(&frame, 1U);
	put64(&frame, 1U);
	error = run(&node, &frame);
	reader.base = node.reply + 24U;
	reader.offset = 0U;
	i915_vkc_dec_VkQueueFamilyProperties(&reader, &arena, &family);
	snprintf(label, sizeof(label), "%s: queue family", name);
	expect(error == 0 && reply32(&node, 12U) == 1U && family.queueFlags == 7U && family.queueCount == 1U &&
	       family.timestampValidBits == 64U, label);

	/*
	 * The memory: one heap of 256 MiB, one type device local, host
	 * visible and coherent; on a discrete profile two heaps (the local
	 * memory, device local; host memory) and two types (device local
	 * alone; host visible and coherent).
	 */
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put64(&frame, 1U);
	put64(&frame, 32U);
	put64(&frame, 16U);
	error = run(&node, &frame);
	reader.base = node.reply + 12U;
	reader.offset = 0U;
	arena.used = 0U;
	i915_vkc_dec_VkPhysicalDeviceMemoryProperties(&reader, &arena, &memory);
	snprintf(label, sizeof(label), "%s: memory types", name);
	if (device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		expect(error == 0 && memory.memoryTypeCount == 2U &&
		       memory.memoryTypes[0].propertyFlags == 1U && memory.memoryTypes[0].heapIndex == 0U &&
		       memory.memoryTypes[1].propertyFlags == 6U && memory.memoryTypes[1].heapIndex == 1U &&
		       memory.memoryHeapCount == 2U && memory.memoryHeaps[0].size == MODEL_LOCAL_BYTES &&
		       memory.memoryHeaps[0].flags == 1U && memory.memoryHeaps[1].size == (256ULL << 20) &&
		       memory.memoryHeaps[1].flags == 0U, label);
	else
		expect(error == 0 && memory.memoryTypeCount == 1U && memory.memoryTypes[0].propertyFlags == 7U &&
		       memory.memoryHeapCount == 1U && memory.memoryHeaps[0].size == (256ULL << 20) &&
		       memory.memoryHeaps[0].flags == 1U, label);

	/* The session and the device. */
	node_close(&node);
	stop(name);
}

/*
 * The format features: from FMT (the model answers formats 37, 43, 44
 * and 50 with every bit, 126 with sample and depth, the rest with
 * nothing) on the full profile, and from the table without FMT on the
 * minimal profile; through the wire and from the executor's table.
 */
static void
scene_formats(void)
{
	struct frame frame;
	struct node node;
	struct ghci_vk *vk;
	unsigned index;
	int same;
	int error;

	/* FMT: R8G8B8A8_UNORM has every feature (storage from the table), as FMT says. */
	error = start(MODEL_FULL);
	if (error == 0)
		error = node_open(&node);
	expect(error == 0, "formats: start full");
	vk = device.vk;
	expect(vk->formats.from_registers == 1, "formats: full reads FMT");
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put32(&frame, 37U);
	put64(&frame, 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply64(&node, 4U) == 1U, "formats: wire reply");
	expect(reply32(&node, 12U) == 0xDF83U, "formats: 37 linear (FMT)");
	expect(reply32(&node, 16U) == 0xDF83U, "formats: 37 optimal (FMT)");
	expect(reply32(&node, 20U) == 0x58U, "formats: 37 buffer (FMT)");

	/* An sRGB format loses storage (the table gives none); a depth format has no texel buffer, no blit. */
	expect(vk->formats.optimal[43] == 0xDF81U && vk->formats.buffer[43] == 0x48U, "formats: 43 (FMT)");
	expect(vk->formats.optimal[126] == 0xC201U && vk->formats.buffer[126] == 0U, "formats: 126 (FMT)");
	expect(vk->formats.optimal[9] == 0U && vk->formats.optimal[53] == 0U, "formats: FMT says nothing");

	/*
	 * FMT_OPTIMAL and FMT_LINEAR that differ: every image is linear, so
	 * optimal tiling has what both have (sample and filter here, with the
	 * copies; the full profile's blit engine adds no blit): through the
	 * wire too.
	 */
	ghci_vk_formats_from_bits(&device.caps, 37U, 0x1FFU, 0x003U, 0x1FFU, &vk->formats);
	expect(vk->formats.optimal[37] == vk->formats.linear[37] && vk->formats.optimal[37] == 0xD001U,
	       "formats: optimal is linear's when linear has less");
	begin(&frame, &node, GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_PHYSICAL);
	put32(&frame, 37U);
	put64(&frame, 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 12U) == 0xD001U && reply32(&node, 16U) == 0xD001U, "formats: the wire answers the intersection");
	ghci_vk_formats_from_bits(&device.caps, 37U, 0x003U, 0x1FFU, 0x1FFU, &vk->formats);
	expect(vk->formats.optimal[37] == 0xD001U && vk->formats.linear[37] == 0xDF83U, "formats: optimal has less than linear");
	node_close(&node);
	stop("formats full");

	/* The table: no packer, no float filter, no sRGB, no BC or ETC2, the blits of the driver's draws. */
	error = start(MODEL_MINIMAL);
	expect(error == 0, "formats: start minimal");
	vk = device.vk;
	expect(vk->formats.from_registers == 0, "formats: minimal has no FMT");
	expect(vk->formats.optimal[37] == 0xDD83U && vk->formats.buffer[37] == 0x58U, "formats: 37 (table)");
	expect(vk->formats.optimal[98] == 0xC403U, "formats: R32_UINT without the packer");
	expect(vk->formats.optimal[100] == 0xC403U, "formats: R32_SFLOAT without filter and packer");
	expect(vk->formats.optimal[43] == 0U && vk->formats.optimal[147] == 0U && vk->formats.optimal[131] == 0U,
	       "formats: no sRGB, ETC2, BC");
	expect(vk->formats.optimal[124] == 0xC201U, "formats: D16 without a blit");
	expect(vk->formats.optimal[51] == vk->formats.optimal[37], "formats: A8B8G8R8 as R8G8B8A8");
	expect(vk->formats.buffer[106] == 0x40U && vk->formats.optimal[106] == 0U, "formats: R32G32B32 vertex only");
	same = 1;
	for (index = 0U; index < GHCI_VK_FORMATS; index++) {
		if (vk->formats.optimal[index] != vk->formats.linear[index])
			same = 0;
	}
	expect(same, "formats: the table's optimal and linear are the same");
	stop("formats minimal");
}

/*
 * The difference each capability flag makes (design section 15.2): the
 * answers of the full profile with one flag cleared against those of the
 * full profile, with every difference written here by hand: the
 * features that turn false, the limit words that change, and the
 * formats that lose features (the table's, so that FMT's answers do not
 * hide the flags).
 */
static void
scene_flags(void)
{
	static const unsigned indirect_features[] = { 9U, 10U };
	static const size_t indirect_limits[] = { offsetof(VkPhysicalDeviceLimits, maxDrawIndirectCount) };
	static const unsigned dual_features[] = { 7U };
	static const size_t dual_limits[] = { offsetof(VkPhysicalDeviceLimits, maxFragmentDualSrcAttachments) };
	static const unsigned logic_features[] = { 8U };
	static const unsigned blend_features[] = { 3U };
	static const size_t msaa_limits[] = {
		offsetof(VkPhysicalDeviceLimits, framebufferColorSampleCounts),
		offsetof(VkPhysicalDeviceLimits, framebufferDepthSampleCounts),
		offsetof(VkPhysicalDeviceLimits, framebufferStencilSampleCounts),
		offsetof(VkPhysicalDeviceLimits, framebufferNoAttachmentsSampleCounts),
		offsetof(VkPhysicalDeviceLimits, sampledImageColorSampleCounts),
		offsetof(VkPhysicalDeviceLimits, sampledImageDepthSampleCounts),
		offsetof(VkPhysicalDeviceLimits, sampledImageStencilSampleCounts)
	};
	static const size_t mrt_limits[] = {
		offsetof(VkPhysicalDeviceLimits, maxFragmentOutputAttachments),
		offsetof(VkPhysicalDeviceLimits, maxFragmentCombinedOutputResources),
		offsetof(VkPhysicalDeviceLimits, maxColorAttachments)
	};
	static const size_t lds_limits[] = { offsetof(VkPhysicalDeviceLimits, maxComputeSharedMemorySize) };
	static const unsigned etc2_features[] = { 20U };
	static const uint32_t etc2_formats[] = { 147U, 148U, 149U, 150U, 151U, 152U, 153U, 154U, 155U, 156U };
	static const uint32_t srgb_formats[] = { 43U, 50U, 57U, 148U, 150U, 152U };
	static const uint32_t bc_formats[] = { 131U, 133U, 135U, 137U, 139U, 141U, 145U };
	static const uint32_t filter_formats[] = { 76U, 83U, 97U, 100U, 103U, 109U };
	static const uint32_t pack_formats[] = {
		13U, 14U, 20U, 21U, 41U, 42U, 55U, 56U, 74U, 75U, 81U, 82U, 95U, 96U, 98U, 99U, 100U, 101U, 102U,
		103U, 107U, 108U, 109U
	};
	static const uint32_t blit_formats[] = { 124U, 126U, 129U, 130U };
	static const uint32_t bilinear_formats[] = {
		9U, 10U, 16U, 17U, 37U, 38U, 43U, 44U, 50U, 51U, 52U, 57U, 64U, 70U, 71U, 76U, 77U, 78U, 83U, 84U,
		86U, 91U, 92U, 97U, 100U, 103U, 109U, 122U, 131U, 133U, 135U, 137U, 139U, 141U, 145U, 147U, 148U,
		149U, 150U, 151U, 152U, 153U, 154U, 155U, 156U
	};
	static const uint32_t filter32_formats[] = { 100U, 103U, 109U };
	uint32_t caps;
	uint32_t feature;
	uint32_t fmt0;
	int error;

	/* The full profile's registers, the base of every case. */
	error = start(MODEL_FULL);
	expect(error == 0, "flags: start full");
	caps = device.caps.device_caps;
	feature = device.caps.feature;
	fmt0 = device.caps.fmt0;

	/* Each V10 row of section 15.2, and a DRV flag that changes no answer. */
	flag_case("INDIRECT_DRAW", caps & ~GHCI_CAP_INDIRECT_DRAW, feature, fmt0, 8U, 16384U,
	    indirect_features, 2U, indirect_limits, 1U, NULL, 0U, 0U);
	flag_case("DUAL_SOURCE", caps & ~GHCI_CAP_DUAL_SOURCE, feature, fmt0, 8U, 16384U,
	    dual_features, 1U, dual_limits, 1U, NULL, 0U, 0U);
	flag_case("LOGIC_OP", caps, feature & ~GHCI_FEATURE_LOGIC_OP, fmt0, 8U, 16384U,
	    logic_features, 1U, NULL, 0U, NULL, 0U, 0U);
	flag_case("MRT_BLEND", caps, feature & ~GHCI_FEATURE_MRT_BLEND, fmt0, 8U, 16384U,
	    blend_features, 1U, NULL, 0U, NULL, 0U, 0U);
	flag_case("MSAA", caps & ~GHCI_CAP_MSAA, feature, fmt0, 8U, 16384U,
	    NULL, 0U, msaa_limits, 7U, NULL, 0U, 0U);
	flag_case("MAX_MRT 2", caps, feature, fmt0, 2U, 16384U,
	    NULL, 0U, mrt_limits, 3U, NULL, 0U, 0U);
	flag_case("LDS_KB 4", caps, feature, fmt0, 8U, 4096U,
	    NULL, 0U, lds_limits, 1U, NULL, 0U, 0U);
	flag_case("ETC2", caps, feature, fmt0 & ~GHCI_FMT0_ETC2, 8U, 16384U,
	    etc2_features, 1U, NULL, 0U, etc2_formats, 10U, 0xFFFFFFFFU);
	flag_case("SRGB", caps, feature, fmt0 & ~GHCI_FMT0_SRGB, 8U, 16384U,
	    etc2_features, 1U, NULL, 0U, srgb_formats, 6U, 0xFFFFFFFFU);
	flag_case("BC", caps, feature, fmt0 & ~GHCI_FMT0_BC_MASK, 8U, 16384U,
	    NULL, 0U, NULL, 0U, bc_formats, 7U, 0xFFFFFFFFU);
	flag_case("TEX_FFILT", caps & ~GHCI_CAP_TEX_FFILT, feature, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, filter_formats, 6U, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	flag_case("ROP_PACK", caps & ~GHCI_CAP_ROP_PACK, feature, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, pack_formats, 23U,
	    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
	flag_case("BLT", caps & ~GHCI_CAP_BLT, feature, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, blit_formats, 4U, VK_FORMAT_FEATURE_BLIT_SRC_BIT);
	flag_case("PROT", caps & ~GHCI_CAP_PROT, feature, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, NULL, 0U, 0U);

	/*
	 * The optional blocks: without the bilinear filter no format
	 * filters; without the filter of 32-bit floats those do not; a
	 * blitter without scaling is not used, as without the blitter; the
	 * clipper, the blitter's blending and tiling change no answer.
	 */
	flag_case("TU_BILINEAR", caps, feature, fmt0 & ~GHCI_FMT0_TU_BILINEAR, 8U, 16384U,
	    NULL, 0U, NULL, 0U, bilinear_formats, 45U, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	flag_case("FFILT_FP32", caps, feature, fmt0 & ~GHCI_FMT0_FFILT_FP32, 8U, 16384U,
	    NULL, 0U, NULL, 0U, filter32_formats, 3U, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	flag_case("BLT_SCALE", caps, feature & ~GHCI_FEATURE_BLT_SCALE, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, blit_formats, 4U, VK_FORMAT_FEATURE_BLIT_SRC_BIT);
	flag_case("CLIP_POLY", caps, feature & ~GHCI_FEATURE_CLIP_POLY, fmt0, 8U, 16384U,
	    NULL, 0U, NULL, 0U, NULL, 0U, 0U);
	flag_case("BLT_BLEND", caps, feature, fmt0 & ~GHCI_FMT0_BLT_BLEND, 8U, 16384U,
	    NULL, 0U, NULL, 0U, NULL, 0U, 0U);
	flag_case("BLT_TILE", caps, feature, fmt0 & ~GHCI_FMT0_BLT_TILE, 8U, 16384U,
	    NULL, 0U, NULL, 0U, NULL, 0U, 0U);
	stop("flags");

	/* The KC705's minimal build and a blitter without its options. */
	scene_optional();
}

/*
 * The KC705's minimal build: no format filters, whatever FMT says; the
 * attach's list names the filter but not the clipper; no blitter.  A blitter
 * that does not scale is not used either.
 */
static void
scene_optional(void)
{
	const char *names[8];
	struct ghci_vk *vk;
	unsigned count;
	unsigned index;
	int clip;
	int bilinear;
	int filtered;
	int error;

	/* kc705-min: the shortfalls and the formats. */
	error = start(MODEL_KC705_MIN);
	vk = device.vk;
	expect(error == 0 && vk != NULL, "optional: start kc705-min");
	if (error != 0 || vk == NULL) {
		stop("optional kc705-min");
		return;
	}
	count = ghci_vk_caps_shortfalls(&device.caps, &vk->formats, names, 8U);
	clip = 0;
	bilinear = 0;
	for (index = 0U; index < count; index++) {
		if (strstr(names[index], "CAP_CLIP_POLY") != NULL)
			clip = 1;
		if (strstr(names[index], "CAP_TU_BILINEAR") != NULL)
			bilinear = 1;
	}
	expect(!clip && bilinear, "optional: kc705-min names the filter, not the clipper");
	filtered = 0;
	for (index = 0U; index < GHCI_VK_FORMATS; index++) {
		if (((vk->formats.optimal[index] | vk->formats.linear[index]) &
		     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U)
			filtered = 1;
	}
	expect(!filtered && vk->formats.optimal[37] != 0U, "optional: kc705-min filters no format");
	expect(ghci_vk_caps_blitter(&device.caps) == 0, "optional: kc705-min has no blitter");
	stop("optional kc705-min");

	/* blt-min: the blitter is there, but not used; the full profile's is. */
	error = start(MODEL_BLT_MIN);
	expect(error == 0 && ghci_caps_has(&device.caps, GHCI_HAS_BLT) && ghci_vk_caps_blitter(&device.caps) == 0,
	       "optional: blt-min's blitter not used");
	stop("optional blt-min");
	error = start(MODEL_FULL);
	expect(error == 0 && ghci_vk_caps_blitter(&device.caps) == 1, "optional: full's blitter used");
	stop("optional full");
}

/*
 * One case of the flags: the answers with the registers given against
 * the full profile's.  features are the places in
 * VkPhysicalDeviceFeatures that turn false, limits the offsets of the
 * 32-bit words of VkPhysicalDeviceLimits that change, formats the formats
 * that lose the bits lost (every bit for 0xFFFFFFFF) on both tilings.
 */
static void
flag_case(
	const char *name,
	uint32_t device_caps,
	uint32_t feature,
	uint32_t fmt0,
	uint32_t max_mrt,
	uint32_t lds,
	const unsigned *features,
	unsigned feature_count,
	const size_t *limits,
	unsigned limit_count,
	const uint32_t *formats,
	unsigned format_count,
	uint32_t lost)
{
	static struct ghci_vk_formats base_formats;
	static struct ghci_vk_formats case_formats;
	VkPhysicalDeviceFeatures base_features;
	VkPhysicalDeviceFeatures case_features;
	VkPhysicalDeviceLimits base_limits;
	VkPhysicalDeviceLimits case_limits;
	struct ghci_caps caps;
	const VkBool32 *before;
	const VkBool32 *after;
	const uint32_t *base_words;
	const uint32_t *case_words;
	uint32_t want_optimal;
	uint32_t want_buffer;
	char label[128];
	unsigned index;
	unsigned at;
	int listed;
	int matched;

	/* The full profile's answers and the case's. */
	ghci_vk_caps_features(&device.caps, &base_features);
	ghci_vk_caps_limits(&device.caps, &base_limits);
	ghci_vk_formats_table(&device.caps, &base_formats);
	caps = device.caps;
	caps.device_caps = device_caps;
	caps.feature = feature;
	caps.fmt0 = fmt0;
	caps.max_mrt = max_mrt;
	caps.lds_bytes = lds;
	ghci_vk_caps_features(&caps, &case_features);
	ghci_vk_caps_limits(&caps, &case_limits);
	ghci_vk_formats_table(&caps, &case_formats);

	/* The features: exactly the listed ones turn false. */
	before = (const VkBool32 *)(const void *)&base_features;
	after = (const VkBool32 *)(const void *)&case_features;
	matched = 1;
	for (index = 0U; index < sizeof(base_features) / sizeof(VkBool32); index++) {
		listed = 0;
		for (at = 0U; at < feature_count; at++) {
			if (features[at] == index)
				listed = 1;
		}
		if (listed && !(before[index] == VK_TRUE && after[index] == VK_FALSE))
			matched = 0;
		if (!listed && before[index] != after[index])
			matched = 0;
	}
	snprintf(label, sizeof(label), "flags %s: features", name);
	expect(matched, label);

	/* The limits: exactly the listed words change. */
	base_words = (const uint32_t *)(const void *)&base_limits;
	case_words = (const uint32_t *)(const void *)&case_limits;
	matched = 1;
	for (index = 0U; index < sizeof(base_limits) / 4U; index++) {
		listed = 0;
		for (at = 0U; at < limit_count; at++) {
			if (limits[at] / 4U == index)
				listed = 1;
		}
		if (listed != (base_words[index] != case_words[index]))
			matched = 0;
	}
	snprintf(label, sizeof(label), "flags %s: limits", name);
	expect(matched, label);

	/* The formats: exactly the listed ones lose the bits, nothing is gained. */
	matched = 1;
	for (index = 0U; index < GHCI_VK_FORMATS; index++) {
		listed = 0;
		for (at = 0U; at < format_count; at++) {
			if (formats[at] == index)
				listed = 1;
		}
		want_optimal = base_formats.optimal[index];
		want_buffer = base_formats.buffer[index];
		if (listed && lost == 0xFFFFFFFFU) {
			want_optimal = 0U;
			want_buffer = 0U;
		} else if (listed) {
			want_optimal &= ~lost;
		}
		if (listed && want_optimal == base_formats.optimal[index] && lost != 0xFFFFFFFFU)
			matched = 0;
		if (case_formats.optimal[index] != want_optimal || case_formats.linear[index] != want_optimal)
			matched = 0;
		if (case_formats.buffer[index] != want_buffer)
			matched = 0;
	}
	snprintf(label, sizeof(label), "flags %s: formats", name);
	expect(matched, label);
}

/*
 * The devices: each takes a GHCI session (CMD_SESSION_OPEN), fifteen at
 * most (the sixteenth fails with VK_ERROR_TOO_MANY_OBJECTS), a feature
 * the device lacks fails with VK_ERROR_FEATURE_NOT_PRESENT, the destroy
 * closes the session; on a flat device no packet at all.
 */
static void
scene_devices(void)
{
	VkPhysicalDeviceFeatures features;
	struct node first;
	struct node second;
	uint32_t result;
	unsigned opens;
	unsigned index;
	int error;

	/* Fifteen devices over two node sessions, the sixteenth refused. */
	error = start(MODEL_PROT);
	if (error == 0)
		error = node_open(&first);
	if (error == 0)
		error = node_open(&second);
	expect(error == 0, "devices: start");
	opens = model_count_commands(GHCI_OP_SESSION_OPEN);
	for (index = 0U; index < 15U; index++) {
		error = create_device(index < 8U ? &first : &second, ID_DEVICE + index, NULL, &result);
		expect(error == 0 && result == 0U, "devices: one of fifteen");
	}
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == opens + 15U, "devices: fifteen SESSION_OPEN");
	error = create_device(&second, ID_DEVICE + 15U, NULL, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_TOO_MANY_OBJECTS, "devices: the sixteenth fails");
	expect(trailer_ok(&second), "devices: the frame goes on");

	/* A destroyed device gives its session back once its close passed; the next device takes one. */
	error = destroy(&first, GPU_OP_DESTROY_DEVICE, ID_DEVICE, 0U);
	expect(error == 0 && model_count_commands(GHCI_OP_SESSION_CLOSE) == 1U, "devices: destroy closes");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	error = create_device(&first, ID_DEVICE + 16U, NULL, &result);
	expect(error == 0 && result == 0U, "devices: a number back");

	/* A feature the device does not offer. */
	memset(&features, 0, sizeof(features));
	features.geometryShader = VK_TRUE;
	error = destroy(&first, GPU_OP_DESTROY_DEVICE, ID_DEVICE + 1U, 0U);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	error = create_device(&first, ID_DEVICE + 17U, &features, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT, "devices: feature not present");
	node_close(&first);
	node_close(&second);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	for (index = 1U; index < GHCI_SESSIONS; index++)
		expect(device.vm[index] == NULL, "devices: every number back after the close");
	stop("devices");

	/* A flat device: a device without a session packet. */
	error = start(MODEL_FLAT);
	if (error == 0)
		error = node_open(&first);
	error = create_device(&first, ID_DEVICE, NULL, &result);
	expect(error == 0 && result == 0U, "devices: flat device");
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == 0U, "devices: flat places no SESSION_OPEN");
	node_close(&first);
	stop("devices flat");

	/* CP_HEAP's bound without room for a session's tables: out of device memory, not of host memory. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&first);
	expect(error == 0, "devices: start bound");
	device.heap_budget = device.heap_used;
	error = create_device(&first, ID_DEVICE, NULL, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && trailer_ok(&first),
	       "devices: CP_HEAP's bound is out of device memory");
	node_close(&first);
	stop("devices bound");
}

/*
 * Memory, buffers and images: an allocation is mapped into the device's
 * session (CMD_VM_MAP at its bus address) and its blob shows it; the
 * requirements, the binding and the linear layout; an imported resource;
 * the free unmaps.
 */
static void
scene_memory(void)
{
	struct ghci_layout_image layout;
	struct gpu_blob_create request;
	struct ghci_resource *resource;
	const struct model_command *command;
	struct ghci_vk_object *object;
	struct ghci_vk_session *session;
	struct frame frame;
	struct node node;
	void *blob;
	uint32_t resource_id;
	uint32_t result;
	uint32_t maps;
	int error;

	/* A device. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	expect(error == 0 && result == 0U, "memory: device");
	session = ((struct ghci_session *)node.session)->vk;

	/* 64 KiB of memory, mapped once at its bus address. */
	maps = model_count_commands(GHCI_OP_VM_MAP);
	error = allocate_memory(&node, ID_DEVICE, 65536U, 0U, ID_MEMORY, &result);
	expect(error == 0 && result == 0U && reply64(&node, 16U) == ID_MEMORY, "memory: allocated");
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY);
	expect(object != NULL && model_count_commands(GHCI_OP_VM_MAP) == maps + 1U, "memory: VM_MAP");
	command = model_last_command(GHCI_OP_VM_MAP);
	expect(object != NULL && command != NULL &&
	       model_lookup(command->session, object->as.memory.mapping->va) != NULL &&
	       model_lookup(command->session, object->as.memory.mapping->va)->phys == object->as.memory.allocation->bus,
	       "memory: mapped at its bus address");

	/* The blob of the memory shows the same bytes. */
	memset(&request, 0, sizeof(request));
	request.bytes = 65536U;
	request.blob_id = ID_MEMORY;
	request.flags = GPU_BLOB_MAPPABLE;
	error = device.ops.blob_create(&device, node.session, &request, &blob, &resource_id);
	resource = blob;
	expect(error == 0 && object != NULL && resource->allocation == object->as.memory.allocation, "memory: blob");
	request.blob_id = 0x12345U;
	error = device.ops.blob_create(&device, node.session, &request, &blob, &resource_id);
	expect(error == ENOENT, "memory: blob of no memory");

	/* A buffer: its requirements (words, 256-byte alignment, the one type) and its binding, once. */
	begin(&frame, &node, GPU_OP_CREATE_BUFFER, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 1001U);
	put32(&frame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	put32(&frame, VK_SHARING_MODE_EXCLUSIVE);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, ID_BUFFER);
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 4U) == 0U, "memory: buffer");
	begin(&frame, &node, GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, ID_BUFFER);
	put64(&frame, 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply64(&node, 12U) == 1004U && reply64(&node, 20U) == 256U && reply32(&node, 28U) == 1U,
	       "memory: buffer requirements");
	begin(&frame, &node, GPU_OP_BIND_BUFFER_MEMORY, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, ID_BUFFER);
	put64(&frame, ID_MEMORY);
	put64(&frame, 256U);
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 4U) == 0U, "memory: buffer bound");
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY, "memory: bound once");

	/*
	 * An image of 30 x 10 R8G8B8A8 texels, 3 levels, 2 layers: rows of
	 * 128, 64 and 64 bytes, planes of 1280, 320 and 128, layers of 1728.
	 */
	error = create_image(&node, ID_DEVICE, VK_FORMAT_R8G8B8A8_UNORM, 30U, 10U, 3U, 2U, ID_IMAGE, &result);
	expect(error == 0 && result == 0U, "memory: image");
	begin(&frame, &node, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, ID_IMAGE);
	put64(&frame, 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply64(&node, 12U) == 3584U && reply64(&node, 20U) == 256U, "memory: image requirements");
	begin(&frame, &node, GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, ID_IMAGE);
	put64(&frame, 1U);
	put32(&frame, VK_IMAGE_ASPECT_COLOR_BIT);
	put32(&frame, 2U);
	put32(&frame, 1U);
	put64(&frame, 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply64(&node, 12U) == 3328U && reply64(&node, 20U) == 128U && reply64(&node, 28U) == 64U &&
	       reply64(&node, 36U) == 1728U && reply64(&node, 44U) == 128U, "memory: subresource layout");

	/*
	 * Multisampled layouts (ghci.md sections 8.4 and 12.11: rows of
	 * roundup(P_min, 64) with P_min = roundup(width, 2) x S x ps): 5 x 3
	 * R8G8B8A8 with 4 samples, P_min 6 x 4 x 4 = 96, rows of 128, a plane
	 * and layers of 384; 7 x 5 R8 with 2 samples, P_min 8 x 2 = 16, rows of
	 * 64, a plane and layers of 320; 30 x 10 with one sample keeps its rows
	 * of 128.
	 */
	memset(&layout, 0, sizeof(layout));
	layout.texel_bytes = 4U;
	layout.width = 5U;
	layout.height = 3U;
	layout.depth = 1U;
	layout.levels = 1U;
	layout.layers = 2U;
	layout.samples = 4U;
	expect(ghci_layout_row(&layout, 0U) == 128U && ghci_layout_plane(&layout, 0U) == 384U && ghci_layout_layer_stride(&layout) == 384U &&
	       ghci_layout_size(&layout) == 768U, "memory: a 4-sample 5 x 3 layout, rows of 128, layers of 384");
	layout.texel_bytes = 1U;
	layout.width = 7U;
	layout.height = 5U;
	layout.samples = 2U;
	expect(ghci_layout_row(&layout, 0U) == 64U && ghci_layout_plane(&layout, 0U) == 320U && ghci_layout_layer_stride(&layout) == 320U,
	       "memory: a 2-sample 7 x 5 R8 layout, rows of 64, layers of 320");
	layout.texel_bytes = 4U;
	layout.width = 30U;
	layout.height = 10U;
	layout.samples = 1U;
	expect(ghci_layout_row(&layout, 0U) == 128U && ghci_layout_layer_stride(&layout) == 1280U, "memory: one sample keeps rows of 128");

	/* An sRGB image on a device without the sRGB bit is refused (the minimal profile below), a big one too. */
	error = create_image(&node, ID_DEVICE, VK_FORMAT_R8G8B8A8_UNORM, 16384U, 16384U, 1U, 1U, ID_IMAGE + 1U, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY, "memory: an image past the heap");

	/* An import of a resource of the session: mapped at the resource's bus address. */
	memset(&request, 0, sizeof(request));
	request.bytes = 8192U;
	request.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;
	error = device.ops.blob_create(&device, node.session, &request, &blob, &resource_id);
	resource = blob;
	expect(error == 0, "memory: resource to import");
	error = allocate_memory(&node, ID_DEVICE, 8192U, resource_id, ID_MEMORY + 1U, &result);
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY + 1U);
	expect(error == 0 && result == 0U && object != NULL && object->as.memory.allocation == resource->allocation,
	       "memory: imported");
	error = allocate_memory(&node, ID_DEVICE, 8192U, 0x777U, ID_MEMORY + 2U, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_INVALID_EXTERNAL_HANDLE, "memory: import of no resource");

	/* The free unmaps. */
	error = destroy(&node, GPU_OP_FREE_MEMORY, ID_DEVICE, ID_MEMORY + 1U);
	expect(error == 0 && model_count_commands(GHCI_OP_VM_UNMAP) == 1U, "memory: free unmaps");
	error = destroy(&node, GPU_OP_DESTROY_BUFFER, ID_DEVICE, ID_BUFFER);
	error |= destroy(&node, GPU_OP_DESTROY_IMAGE, ID_DEVICE, ID_IMAGE);
	expect(error == 0 && ghci_vk_object_find(session, GHCI_VK_BUFFER, ID_BUFFER) == NULL, "memory: destroyed");
	device.ops.resource_destroy(&device, node.session, blob);
	node_close(&node);
	stop("memory");

	/* The minimal profile has no sRGB format. */
	error = start(MODEL_MINIMAL);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	error = create_image(&node, ID_DEVICE, VK_FORMAT_R8G8B8A8_SRGB, 16U, 16U, 1U, 1U, ID_IMAGE, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_FORMAT_NOT_SUPPORTED, "memory: no sRGB image on minimal");
	node_close(&node);
	stop("memory minimal");
}

/*
 * The recording: a pool, two buffers, a recording of three commands kept
 * as they came, the end, the submission of an empty buffer (a fence
 * alone) and the refusal of a recorded one whose draw has no binds.
 */
static void
scene_recording(void)
{
	struct ghci_vk_object *object;
	struct ghci_vk_session *session;
	struct frame frame;
	struct node node;
	uint64_t identities[3];
	size_t recorded;
	uint32_t result;
	unsigned fences;
	int error;

	/* A device, its queue and a pool. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = get_queue(&node, ID_DEVICE, 1U, ID_QUEUE);
	if (error == 0)
		error = simple_create(&node, GPU_OP_CREATE_COMMAND_POOL, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, ID_DEVICE, 0U, ID_POOL, &result);
	expect(error == 0 && result == 0U, "recording: pool");
	session = ((struct ghci_session *)node.session)->vk;

	/* Two primary buffers. */
	begin(&frame, &node, GPU_OP_ALLOCATE_COMMAND_BUFFERS, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
	put64(&frame, 0U);
	put64(&frame, ID_POOL);
	put32(&frame, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	put32(&frame, 2U);
	put64(&frame, 2U);
	put64(&frame, ID_COMMANDS);
	put64(&frame, ID_COMMANDS + 1U);
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 4U) == 0U && reply64(&node, 8U) == 2U && reply64(&node, 24U) == ID_COMMANDS + 1U,
	       "recording: buffers");

	/* The begin, then three recorded commands and the end in one frame, as libvulkan sends them. */
	begin(&frame, &node, GPU_OP_BEGIN_COMMAND_BUFFER, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_COMMANDS);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
	put64(&frame, 0U);
	put32(&frame, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	put64(&frame, 0U);
	error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 4U) == 0U, "recording: begin");
	begin(&frame, &node, GPU_OP_CMD_DRAW, 0U);
	put64(&frame, ID_COMMANDS);
	put32(&frame, 3U);
	put32(&frame, 1U);
	put32(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, GPU_OP_CMD_SET_BLEND_CONSTANTS);
	put32(&frame, 0U);
	put64(&frame, ID_COMMANDS);
	put64(&frame, 4U);
	put32(&frame, 0x3f800000U);
	put32(&frame, 0U);
	put32(&frame, 0U);
	put32(&frame, 0x3f800000U);
	put32(&frame, GPU_OP_CMD_END_RENDER_PASS);
	put32(&frame, 0U);
	put64(&frame, ID_COMMANDS);
	recorded = frame.used - 36U;
	put32(&frame, GPU_OP_END_COMMAND_BUFFER);
	put32(&frame, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_COMMANDS);
	error = run(&node, &frame);
	object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS);
	expect(error == 0 && reply32(&node, 0U) == GPU_OP_END_COMMAND_BUFFER && reply32(&node, 4U) == 0U, "recording: end");
	expect(object != NULL && object->as.commands.commands == 3U && object->as.commands.used == recorded &&
	       memcmp(object->as.commands.bytes, frame.bytes + 36U, recorded) == 0, "recording: kept as it came");
	expect(object != NULL && object->as.commands.state == GHCI_VK_RECORD_EXECUTABLE, "recording: executable");

	/* A pool of another device is refused; a buffer that cannot be made takes the ones before it back. */
	error = create_device(&node, ID_DEVICE + 1U, NULL, &result);
	expect(error == 0 && result == 0U, "recording: a second device");
	identities[0] = ID_COMMANDS + 8U;
	error = allocate_buffers(&node, ID_DEVICE + 1U, ID_POOL, identities, 1U);
	expect(error == EINVAL && ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS + 8U) == NULL,
	       "recording: a pool of another device");
	identities[1] = ID_COMMANDS + 9U;
	identities[2] = ID_COMMANDS;
	error = allocate_buffers(&node, ID_DEVICE, ID_POOL, identities, 3U);
	expect(error == EINVAL && ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS + 8U) == NULL &&
	       ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS + 9U) == NULL &&
	       ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS) != NULL,
	       "recording: a failed allocation takes the made buffers back");

	/* A recorded command of a malformed shape ends the frame without the trailer. */
	begin(&frame, &node, GPU_OP_CMD_SET_BLEND_CONSTANTS, 0U);
	put64(&frame, ID_COMMANDS + 1U);
	put64(&frame, 3U);
	error = run(&node, &frame);
	expect(error != 0 && !trailer_ok(&node), "recording: malformed refused");

	/* The empty buffer: a submission of the fence alone (BIND_SESSION, FENCE, BIND_SESSION 0). */
	begin(&frame, &node, GPU_OP_BEGIN_COMMAND_BUFFER, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_COMMANDS + 1U);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put32(&frame, GPU_OP_END_COMMAND_BUFFER);
	put32(&frame, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_COMMANDS + 1U);
	(void)run(&node, &frame);
	fences = model_count_commands(GHCI_OP_FENCE);
	error = submit(&node, ID_QUEUE, ID_COMMANDS + 1U, 0U);
	expect(error == 0 && reply32(&node, 4U) == 0U && model_count_commands(GHCI_OP_FENCE) == fences + 1U,
	       "recording: empty submission places a fence");
	expect(model_last_command(GHCI_OP_BIND_SESSION) != NULL && model_last_command(GHCI_OP_BIND_SESSION)->words[1] == 0U,
	       "recording: back in session 0");

	/* A buffer that draws outside a render pass and with no pipeline bound: the submission is refused, nothing placed. */
	fences = model_count_commands(GHCI_OP_FENCE);
	error = submit(&node, ID_QUEUE, ID_COMMANDS, 0U);
	expect(error == EINVAL && model_count_commands(GHCI_OP_FENCE) == fences && model_count_commands(GHCI_OP_DRAW) == 0U,
	       "recording: a draw without its binds refuses the submission");

	/* The pool's destroy takes its buffers. */
	error = destroy(&node, GPU_OP_DESTROY_COMMAND_POOL, ID_DEVICE, ID_POOL);
	expect(error == 0 && ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, ID_COMMANDS) == NULL, "recording: pool destroyed");
	node_close(&node);
	stop("recording");
}

/*
 * Fences, semaphores and events: a fence created signaled, a fence a
 * submission signals once its ticket passed, the reset; a semaphore; an
 * event set and reset on the host.
 */
static void
scene_sync(void)
{
	struct ghci_vk_object *object;
	struct ghci_vk_session *session;
	struct node node;
	uint32_t result;
	int error;

	/* A device and its queue. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = get_queue(&node, ID_DEVICE, 1U, ID_QUEUE);
	expect(error == 0, "sync: device");
	session = ((struct ghci_session *)node.session)->vk;

	/* A fence created signaled, and one not. */
	error = simple_create(&node, GPU_OP_CREATE_FENCE, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, ID_DEVICE, VK_FENCE_CREATE_SIGNALED_BIT, ID_FENCE, &result);
	expect(error == 0 && result == 0U, "sync: fence");
	error = object_query(&node, GPU_OP_GET_FENCE_STATUS, ID_DEVICE, ID_FENCE, &result);
	expect(error == 0 && result == 0U, "sync: created signaled");
	error = simple_create(&node, GPU_OP_CREATE_FENCE, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, ID_DEVICE, 0U, ID_FENCE + 1U, &result);
	error |= object_query(&node, GPU_OP_GET_FENCE_STATUS, ID_DEVICE, ID_FENCE + 1U, &result);
	expect(error == 0 && result == (uint32_t)VK_NOT_READY, "sync: not ready");

	/* The empty submission signals it once its ticket passed. */
	error = submit(&node, ID_QUEUE, 0U, ID_FENCE + 1U);
	object = ghci_vk_object_find(session, GHCI_VK_FENCE, ID_FENCE + 1U);
	expect(error == 0 && object != NULL && object->as.fence.submitted, "sync: submitted");
	expect(object != NULL && ticket_passed(&object->as.fence.ticket), "sync: the model wrote the fence");
	error = object_query(&node, GPU_OP_GET_FENCE_STATUS, ID_DEVICE, ID_FENCE + 1U, &result);
	expect(error == 0 && result == 0U, "sync: signaled by the GPU");

	/* The reset. */
	{
		struct frame frame;

		begin(&frame, &node, GPU_OP_RESET_FENCES, GPU_OP_FLAG_REPLY);
		put64(&frame, ID_DEVICE);
		put32(&frame, 2U);
		put64(&frame, 2U);
		put64(&frame, ID_FENCE);
		put64(&frame, ID_FENCE + 1U);
		error = run(&node, &frame);
	}
	expect(error == 0 && reply32(&node, 4U) == 0U, "sync: reset");
	error = object_query(&node, GPU_OP_GET_FENCE_STATUS, ID_DEVICE, ID_FENCE, &result);
	expect(error == 0 && result == (uint32_t)VK_NOT_READY, "sync: reset fence not ready");

	/* A semaphore. */
	error = simple_create(&node, GPU_OP_CREATE_SEMAPHORE, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, ID_DEVICE, 0U, ID_SEMAPHORE, &result);
	expect(error == 0 && result == 0U, "sync: semaphore");

	/* An event in the executor heap: reset, set, reset. */
	error = simple_create(&node, GPU_OP_CREATE_EVENT, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, ID_DEVICE, 0U, ID_EVENT, &result);
	expect(error == 0 && result == 0U, "sync: event");
	object = ghci_vk_object_find(session, GHCI_VK_EVENT, ID_EVENT);
	expect(object != NULL && object->device->heap[GHCI_VK_HEAP_DATA] != NULL, "sync: the event's word is in the data heap");
	error = object_query(&node, GPU_OP_GET_EVENT_STATUS, ID_DEVICE, ID_EVENT, &result);
	expect(error == 0 && result == (uint32_t)VK_EVENT_RESET, "sync: event reset");
	error = object_query(&node, GPU_OP_SET_EVENT, ID_DEVICE, ID_EVENT, &result);
	error |= object_query(&node, GPU_OP_GET_EVENT_STATUS, ID_DEVICE, ID_EVENT, &result);
	expect(error == 0 && result == (uint32_t)VK_EVENT_SET && object != NULL && *object->as.event.word == 1U,
	       "sync: event set");
	error = object_query(&node, GPU_OP_RESET_EVENT, ID_DEVICE, ID_EVENT, &result);
	error |= object_query(&node, GPU_OP_GET_EVENT_STATUS, ID_DEVICE, ID_EVENT, &result);
	expect(error == 0 && result == (uint32_t)VK_EVENT_RESET, "sync: event reset again");
	error = destroy(&node, GPU_OP_DESTROY_EVENT, ID_DEVICE, ID_EVENT);
	expect(error == 0 && ghci_vk_object_find(session, GHCI_VK_EVENT, ID_EVENT) == NULL, "sync: event destroyed");
	node_close(&node);
	stop("sync");
}

/*
 * The job markers and the frames: a marker on the queue's timeline
 * completes once the submission before it passed (on the service
 * thread), one with nothing before it at once; the capacity; a frame
 * completes behind the work; a marker of no timeline is refused.
 */
static void
scene_jobs(void)
{
	struct drv_gpu_completion *first;
	struct drv_gpu_completion *second;
	struct node node;
	unsigned available;
	uint32_t result;
	void *token;
	int error;

	/* A device and its queue on timeline 1. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = get_queue(&node, ID_DEVICE, 1U, ID_QUEUE);
	expect(error == 0, "jobs: device");
	first = (struct drv_gpu_completion *)(void *)&completions[0];
	second = (struct drv_gpu_completion *)(void *)&completions[1];

	/* No marker on a timeline no queue owns. */
	error = device.ops.jobs->reserve(&device, node.session, 2U, first, &token);
	expect(error == EINVAL, "jobs: no queue on timeline 2");

	/* A marker with no work before it completes at its commit. */
	error = device.ops.jobs->capacity(&device, node.session, 1U, &available);
	expect(error == 0 && available == GHCI_JOB_MARKERS, "jobs: capacity");
	error = device.ops.jobs->reserve(&device, node.session, 1U, first, &token);
	expect(error == 0, "jobs: reserved");
	error = device.ops.jobs->capacity(&device, node.session, 1U, &available);
	expect(error == 0 && available == GHCI_JOB_MARKERS - 1U, "jobs: one fewer");
	error = device.ops.jobs->commit(&device, node.session, token, first);
	expect(error == 0 && stub.completions == 1U && stub.last_completion_error == 0, "jobs: done at once");

	/* A marker behind a submission the model holds completes on the service thread once it passed. */
	model_hold(1);
	error = submit(&node, ID_QUEUE, 0U, 0U);
	error |= device.ops.jobs->reserve(&device, node.session, 1U, second, &token);
	error |= device.ops.jobs->commit(&device, node.session, token, second);
	expect(error == 0 && stub.completions == 1U, "jobs: waits for the work");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == 1U, "jobs: still waits while held");
	expect(device.ops.recovery->stop_poll(&device, node.session) == EAGAIN, "jobs: stop poll waits");
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == 2U && stub.last_completion_error == 0, "jobs: completed by the service");

	/* A context fence frame of the timeline, and a frame of commands. */
	error = device.ops.commands->submit(&device, node.session, NULL, 0U, GPU_COMMAND_CONTEXT_FENCE, 1U, first);
	expect(error == 0, "jobs: fence frame queued");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == 3U, "jobs: fence frame completed");
	error = device.ops.commands->submit(&device, node.session, NULL, 0U, 0U, 0U, first);
	expect(error == EINVAL, "jobs: an empty frame without the fence flag");
	expect(device.ops.recovery->stop_poll(&device, node.session) == 0, "jobs: stop poll done");
	node_close(&node);
	stop("jobs");
}

/*
 * The completions of the queued frames and their count: a frame of
 * timeline 0 (the decoder's notification) completes once it has run,
 * though earlier work of the session is still in the GPU and a context
 * fence of a timeline waits; a frame of a timeline no queue owns is
 * refused before it runs.  A completion that a retirement took and has
 * not yet reported (the service thread between its retirement and
 * drv_gpu_complete) keeps the stop poll waiting and holds the drain and
 * the isolation until it is reported.
 */
static void
scene_completions(void)
{
	struct drv_gpu_completion *notification;
	struct drv_gpu_completion *fence;
	struct ghci_vk_session *session;
	struct frame frame;
	struct node node;
	unsigned completed;
	unsigned sleeps;
	uint32_t result;
	int error;

	/* A device and its queue on timeline 1. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = get_queue(&node, ID_DEVICE, 1U, ID_QUEUE);
	expect(error == 0, "completions: device");
	session = ((struct ghci_session *)node.session)->vk;
	notification = (struct drv_gpu_completion *)(void *)&completions[2];
	fence = (struct drv_gpu_completion *)(void *)&completions[3];

	/* Work the model holds, and a context fence of timeline 1 behind it. */
	model_hold(1);
	error = submit(&node, ID_QUEUE, 0U, 0U);
	error |= device.ops.commands->submit(&device, node.session, NULL, 0U, GPU_COMMAND_CONTEXT_FENCE, 1U, fence);
	expect(error == 0, "completions: work held, a fence frame behind it");

	/* A notification frame runs, and completes at once though the work is held. */
	completed = stub.completions;
	begin(&frame, &node, GPU_OP_ENUMERATE_INSTANCE_VERSION, GPU_OP_FLAG_REPLY);
	put64(&frame, 1U);
	error = run_queued(&node, &frame, 0U, notification);
	expect(error == 0 && trailer_ok(&node), "completions: the notification frame ran");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == completed + 1U && stub.last_completion_error == 0, "completions: timeline 0 completes while the work is held");
	expect(session->frame_count == 1U && session->completing == 0U, "completions: the fence frame still waits");
	expect(device.ops.recovery->stop_poll(&device, node.session) == EAGAIN, "completions: stop poll waits for the fence frame");

	/* A frame of a timeline no queue owns is refused before it runs. */
	begin(&frame, &node, GPU_OP_ENUMERATE_INSTANCE_VERSION, GPU_OP_FLAG_REPLY);
	put64(&frame, 1U);
	error = run_queued(&node, &frame, 5U, notification);
	expect(error == EINVAL && !trailer_ok(&node), "completions: unknown timeline refused, not run");

	/* The work passes: the fence frame completes. */
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(stub.completions == completed + 2U && session->frame_count == 0U && session->completing == 0U,
	       "completions: the fence frame completes once the work passed");
	expect(device.ops.recovery->stop_poll(&device, node.session) == 0, "completions: stop poll done");

	/* A completion in flight (taken, not reported yet) keeps the stop poll waiting. */
	session->completing = 1U;
	expect(device.ops.recovery->stop_poll(&device, node.session) == EAGAIN, "completions: stop poll waits for a completion in flight");

	/* The drain does not return until the completion in flight is reported. */
	completing_session = session;
	completing_sleep = 0U;
	sleeps = stub.sleeps;
	stub_set_sleep_hook(completing_hook, NULL);
	device.ops.commands->drain(&device, node.session);
	expect(completing_sleep > sleeps && session->completing == 0U, "completions: drain waits for the report");

	/* The isolation does not return until a completion in flight is reported either. */
	session->completing = 1U;
	completing_sleep = 0U;
	sleeps = stub.sleeps;
	error = device.ops.recovery->isolate(&device, node.session);
	expect(error == 0 && completing_sleep > sleeps && session->completing == 0U, "completions: isolate waits for the report");
	stub_set_sleep_hook(sleep_hook, NULL);
	completing_session = NULL;
	node_close(&node);
	stop("completions");
}

/*
 * The GPU core's recovery: the stop of a node session, the isolation
 * (the session takes no work and its memory stays with the device until
 * a reset), and the core's fault (the device is lost).
 */
static void
scene_recovery(void)
{
	struct ghci_vk_object *object;
	struct ghci_vk_session *session;
	struct ghci_allocation *allocation;
	struct ghci_vm *vm;
	struct node node;
	struct node other;
	uint32_t result;
	unsigned closes;
	unsigned unmaps;
	unsigned slot;
	int error;

	/* A device with memory, and another session that goes on. */
	error = start(MODEL_PROT);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = node_open(&other);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = create_device(&other, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = allocate_memory(&node, ID_DEVICE, 65536U, 0U, ID_MEMORY, &result);
	expect(error == 0 && result == 0U, "recovery: devices");
	session = ((struct ghci_session *)node.session)->vk;
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY);
	allocation = object->as.memory.allocation;
	vm = object->device->vm;
	slot = vm->slot;

	/* The stop of an idle session is done at once. */
	error = device.ops.recovery->stop_begin(&device, node.session, EIO);
	expect(error == 0 && device.ops.recovery->stop_poll(&device, node.session) == 0, "recovery: stop of an idle session");

	/* The isolation: no more work, no packet at the close, the memory kept. */
	error = device.ops.recovery->isolate(&device, node.session);
	expect(error == 0 && vm->state == GHCI_VM_ISOLATED, "recovery: isolated");
	error = create_device(&node, ID_DEVICE + 1U, NULL, &result);
	expect(error == EIO, "recovery: an isolated session takes no command");
	closes = model_count_commands(GHCI_OP_SESSION_CLOSE);
	unmaps = model_count_commands(GHCI_OP_VM_UNMAP);
	node_close(&node);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(model_count_commands(GHCI_OP_SESSION_CLOSE) == closes && model_count_commands(GHCI_OP_VM_UNMAP) == unmaps,
	       "recovery: the isolated close places nothing");
	expect(device.vm[slot] == vm && vm->closed && allocation->holds == 1U, "recovery: the device holds the session and its memory");

	/* The other session goes on. */
	error = get_queue(&other, ID_DEVICE, 1U, ID_QUEUE);
	error |= submit(&other, ID_QUEUE, 0U, 0U);
	expect(error == 0 && reply32(&other, 4U) == 0U, "recovery: the other session submits");

	/* The core's fault: the device is lost, a submission fails. */
	device.ops.recovery->fault(&device, EIO);
	expect(device.state == GHCI_STATE_LOST, "recovery: fault loses the device");
	device.ops.recovery->fault(&device, EIO);
	expect(device.state == GHCI_STATE_LOST, "recovery: fault again changes nothing");
	error = submit(&other, ID_QUEUE, 0U, 0U);
	expect(error == 0 && reply32(&other, 4U) == (uint32_t)VK_ERROR_DEVICE_LOST, "recovery: a lost device's submission");
	node_close(&other);

	/* The core's reset (no owner left): the device runs again and the isolated session's memory went back. */
	service_auto = 1;
	error = device.ops.recovery->reset(&device);
	service_auto = 0;
	expect(error == 0 && device.state == GHCI_STATE_RUNNING && !device.recovering, "recovery: core reset");
	expect(device.vm[slot] == NULL, "recovery: the isolated session retired");
	expect(device.quarantined == NULL, "recovery: quarantine back");
	expect(log_names_no_session(), "recovery: the log names no session");
	stop("recovery");
}

/*
 * The core's reset of a device lost with work in the ring: the closes
 * while lost retire the sessions at once (their memory to the
 * quarantine) though the submission log still names them; the reset
 * empties the log first, then gives the quarantine back.
 */
static void
scene_core_reset(void)
{
	struct node node;
	uint32_t result;
	int error;

	/* A device with memory and a submission the model holds. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	if (error == 0)
		error = get_queue(&node, ID_DEVICE, 1U, ID_QUEUE);
	if (error == 0)
		error = allocate_memory(&node, ID_DEVICE, 65536U, 0U, ID_MEMORY, &result);
	expect(error == 0, "core reset: device");
	model_hold(1);
	error = submit(&node, ID_QUEUE, 0U, 0U);
	expect(error == 0 && device.submit_count != 0U, "core reset: work in the ring");

	/* Lost, then closed: the memory is quarantined. */
	device.ops.recovery->fault(&device, EIO);
	node_close(&node);
	expect(device.quarantined != NULL, "core reset: quarantined while lost");

	/* The reset. */
	model_hold(0);
	service_auto = 1;
	error = device.ops.recovery->reset(&device);
	service_auto = 0;
	expect(error == 0 && device.state == GHCI_STATE_RUNNING, "core reset: runs again");
	expect(device.quarantined == NULL, "core reset: quarantine back");
	expect(log_names_no_session(), "core reset: the log names no session");
	stop("core reset");
}

/*
 * vkAllocateMemory of a memory type, made for export or not.  Returns
 * what the frame returned; *result is the VkResult.
 */
static int
allocate_typed(
	struct node *node,
	uint64_t handle,
	uint64_t bytes,
	uint32_t type,
	int exported,
	uint64_t memory,
	uint32_t *result)
{
	struct frame frame;
	int error;

	/* [device][u64 1][sType][u64 chain] and the export record. */
	begin(&frame, node, GPU_OP_ALLOCATE_MEMORY, GPU_OP_FLAG_REPLY);
	put64(&frame, handle);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	if (exported) {
		put64(&frame, 1U);
		put32(&frame, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
		put64(&frame, 0U);
		put32(&frame, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
	} else {
		put64(&frame, 0U);
	}

	/* [u64 size][u32 type][u64 0][u64 1][memory]. */
	put64(&frame, bytes);
	put32(&frame, type);
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, memory);
	error = run(node, &frame);
	*result = reply32(node, 4U);
	return error;
}

/* Reports whether every one of count bytes is value: 1 when it is. */
static int
all_bytes(
	const uint8_t *bytes,
	uint64_t count,
	uint8_t value)
{
	uint64_t index;

	/* Each byte in turn. */
	for (index = 0U; index < count; index++) {
		if (bytes[index] != value)
			return 0;
	}

	/* Succeeded: all of them are. */
	return 1;
}

/*
 * A discrete device: device-local memory in local memory, mapped with
 * VM_FLAG_LOCAL and cleared by FILL_BUFFER before the allocation returns
 * (and by the copy program without the blitter), exported device-local
 * memory in local memory too and its importer's mapping with
 * VM_FLAG_LOCAL, host-visible memory in host memory, buffers that take
 * both types, a client's read, write and mapping of local memory refused
 * and no view of it for the processor, a local heap that runs out, and
 * the runs going back to the allocator.
 */
static void
scene_discrete(void)
{
	static const uint8_t written[16] = {
		1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U
	};
	struct gpu_blob_create request;
	struct drv_gpu_mapping view;
	const struct model_command *command;
	struct ghci_allocation *allocation;
	struct ghci_allocation *exported;
	struct ghci_vk_object *object;
	struct ghci_vk_session *session;
	struct frame frame;
	struct node node;
	uint8_t read_back[16];
	uint32_t shared_id;
	void *shared;
	uint64_t offset;
	unsigned fills;
	unsigned dispatches;
	uint32_t result;
	uint32_t resource_id;
	void *blob;
	int error;

	/* A discrete device; local memory holds what an earlier client left. */
	error = start(MODEL_KC705_DISCRETE);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	expect(error == 0 && result == 0U, "discrete: device");
	session = ((struct ghci_session *)node.session)->vk;
	expect(model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U] == 0U, "discrete: window closed while the driver does not use it");
	memset(model.local, 0xa5, MODEL_LOCAL_BYTES);

	/* Device-local memory: a run of local memory, mapped with VM_FLAG_LOCAL at its local address. */
	fills = model_count_commands(GHCI_OP_FILL_BUFFER);
	error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 0, ID_MEMORY, &result);
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY);
	allocation = object != NULL ? object->as.memory.allocation : NULL;
	expect(error == 0 && result == 0U && allocation != NULL && allocation->kind == GHCI_MEMORY_LOCAL &&
	       allocation->bus >= MODEL_LOCAL_BASE && allocation->bus + allocation->bytes <= MODEL_LOCAL_BASE + MODEL_LOCAL_BYTES,
	       "discrete: device-local memory is local");
	command = model_last_command(GHCI_OP_VM_MAP);
	expect(command != NULL && allocation != NULL && (command->words[2] & GHCI_VM_FLAG_LOCAL) != 0U &&
	       ((uint64_t)command->words[7] << 32 | command->words[6]) == allocation->bus, "discrete: VM_MAP with VM_FLAG_LOCAL");

	/* Cleared by FILL_BUFFER over the whole mapping, before the allocation returned; the next byte is untouched. */
	command = model_last_command(GHCI_OP_FILL_BUFFER);
	expect(model_count_commands(GHCI_OP_FILL_BUFFER) == fills + 1U && command != NULL && object != NULL &&
	       ((uint64_t)command->words[2] << 32 | command->words[1]) == object->as.memory.mapping->va &&
	       command->words[3] == 65536U && command->words[4] == 0U, "discrete: FILL_BUFFER of zeroes");
	offset = allocation != NULL ? allocation->bus - MODEL_LOCAL_BASE : 0U;
	expect(allocation != NULL && all_bytes(model.local + offset, 65536U, 0U) && model.local[offset + 65536U] == 0xa5U,
	       "discrete: the run holds zeroes only");

	/* Host-visible memory stays in host memory, and is not filled. */
	fills = model_count_commands(GHCI_OP_FILL_BUFFER);
	error = allocate_typed(&node, ID_DEVICE, 65536U, 1U, 0, ID_MEMORY + 1U, &result);
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY + 1U);
	command = model_last_command(GHCI_OP_VM_MAP);
	expect(error == 0 && result == 0U && object != NULL && object->as.memory.allocation->kind == GHCI_MEMORY_HOST &&
	       command != NULL && (command->words[2] & GHCI_VM_FLAG_LOCAL) == 0U, "discrete: host-visible memory is host memory");
	expect(model_count_commands(GHCI_OP_FILL_BUFFER) == fills, "discrete: host memory not filled");

	/* Device-local memory made for export is local memory too, and filled. */
	error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 1, ID_MEMORY + 2U, &result);
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY + 2U);
	exported = object != NULL ? object->as.memory.allocation : NULL;
	expect(error == 0 && result == 0U && exported != NULL && exported->kind == GHCI_MEMORY_LOCAL &&
	       model_count_commands(GHCI_OP_FILL_BUFFER) == fills + 1U, "discrete: exported device-local memory is local memory");

	/* Its importer maps it as local memory, at the same local address, without another fill. */
	memset(&request, 0, sizeof(request));
	request.bytes = 65536U;
	request.blob_id = ID_MEMORY + 2U;
	shared = NULL;
	shared_id = 0U;
	error = device.ops.blob_create(&device, node.session, &request, &shared, &shared_id);
	if (error == 0)
		error = allocate_memory(&node, ID_DEVICE, 65536U, shared_id, ID_MEMORY + 5U, &result);
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY + 5U);
	command = model_last_command(GHCI_OP_VM_MAP);
	expect(error == 0 && result == 0U && object != NULL && object->as.memory.allocation == exported && command != NULL &&
	       (command->words[2] & GHCI_VM_FLAG_LOCAL) != 0U && exported != NULL &&
	       ((uint64_t)command->words[7] << 32 | command->words[6]) == exported->bus &&
	       model_count_commands(GHCI_OP_FILL_BUFFER) == fills + 1U, "discrete: imported local memory mapped with VM_FLAG_LOCAL");
	error = allocate_typed(&node, ID_DEVICE, 65536U, 2U, 0, ID_MEMORY + 3U, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY, "discrete: no third type");

	/* A buffer takes either type. */
	begin(&frame, &node, GPU_OP_CREATE_BUFFER, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, 1U);
	put32(&frame, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
	put64(&frame, 0U);
	put32(&frame, 0U);
	put64(&frame, 4096U);
	put32(&frame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	put32(&frame, VK_SHARING_MODE_EXCLUSIVE);
	put32(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 0U);
	put64(&frame, 1U);
	put64(&frame, ID_BUFFER);
	error = run(&node, &frame);
	begin(&frame, &node, GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS, GPU_OP_FLAG_REPLY);
	put64(&frame, ID_DEVICE);
	put64(&frame, ID_BUFFER);
	put64(&frame, 1U);
	if (error == 0)
		error = run(&node, &frame);
	expect(error == 0 && reply32(&node, 28U) == 3U, "discrete: buffer memory types");

	/* A client neither reads, writes nor maps local memory; the window stays closed. */
	memset(&request, 0, sizeof(request));
	request.bytes = 65536U;
	request.blob_id = ID_MEMORY;
	request.flags = GPU_BLOB_MAPPABLE;
	blob = NULL;
	error = device.ops.blob_create(&device, node.session, &request, &blob, &resource_id);
	expect(error == 0 && blob != NULL, "discrete: blob of local memory");
	if (error == 0) {
		error = device.ops.resource_write(&device, node.session, blob, 64U, written, sizeof(written));
		expect(error == ENOTSUP && model.local[offset + 64U] == 0U, "discrete: a client's write refused");
		error = device.ops.resource_read(&device, node.session, blob, 64U, read_back, sizeof(read_back));
		expect(error == ENOTSUP, "discrete: a client's read refused");
		memset(&view, 0, sizeof(view));
		error = device.ops.resource_map(&device, node.session, blob, &view);
		expect(error == ENOTSUP, "discrete: a client's mapping refused");
		device.ops.resource_destroy(&device, node.session, blob);
	}
	expect(model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U] == 0U, "discrete: the window stays closed");

	/* The processor has no view of local memory: the GPU alone reads and writes it. */
	expect(allocation != NULL && allocation->view == NULL && device.window_users == 0U, "discrete: no view of local memory");

	/* The local heap runs out: more than local memory has. */
	error = allocate_typed(&node, ID_DEVICE, MODEL_LOCAL_BYTES + 65536U, 0U, 0, ID_MEMORY + 4U, &result);
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY, "discrete: local heap exhausted");

	/* Freed local memory goes back to the allocator once its unmaps passed and its last holder let it go. */
	error = destroy(&node, GPU_OP_FREE_MEMORY, ID_DEVICE, ID_MEMORY);
	error |= destroy(&node, GPU_OP_FREE_MEMORY, ID_DEVICE, ID_MEMORY + 5U);
	error |= destroy(&node, GPU_OP_FREE_MEMORY, ID_DEVICE, ID_MEMORY + 2U);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(error == 0 && device.local_map.count == 1U, "discrete: the export's resource keeps its run");
	if (shared != NULL)
		device.ops.resource_destroy(&device, node.session, shared);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(device.local_map.count == 0U, "discrete: freed local memory back in the allocator");
	node_close(&node);
	stop("discrete");

	/* Without the blitter, the driver's copy program clears local memory: a dispatch, no FILL_BUFFER. */
	test_caps_clear = GHCI_CAP_BLT;
	error = start(MODEL_KC705_DISCRETE);
	test_caps_clear = 0U;
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	fills = model_count_commands(GHCI_OP_FILL_BUFFER);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	if (error == 0)
		error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 0, ID_MEMORY, &result);
	expect(error == 0 && result == 0U && model_count_commands(GHCI_OP_FILL_BUFFER) == fills &&
	       model_count_commands(GHCI_OP_DISPATCH) > dispatches, "discrete without blitter: cleared by the copy program");
	node_close(&node);
	stop("discrete without blitter");
}

/*
 * Each sleep of an allocation's wait for its clear, as clear_mode says:
 * a busy GPU that holds the clear for a minute of the clock and then
 * runs it; a device fault and its recovery while the clear is held; or
 * the clear run and its session then found a culprit, with the GPU held
 * again so that the unmap of the failed allocation waits.
 */
static void
clear_hook(
	unsigned sleep,
	void *argument)
{
	(void)sleep;
	(void)argument;

	/* Never inside the hook's own recovery. */
	if (clear_running)
		return;
	clear_running = 1;
	clear_sleeps++;

	/* A busy GPU: a minute passes, then the clear runs. */
	if (clear_mode == CLEAR_BUSY && clear_sleeps == 1U)
		stub.ticks += kern_ms_to_ticks(60000U);
	if (clear_mode == CLEAR_BUSY && clear_sleeps == 4U)
		model_hold(0);

	/* A device fault while the clear is held, and the recovery. */
	if (clear_mode == CLEAR_RESET && clear_sleeps == 2U) {
		model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
		ghci_service_step(&device, GHCI_WORK_RECOVER);
	}

	/* The clear runs, then its session is found a culprit before the recovery; the GPU holds what comes next. */
	if (clear_mode == CLEAR_CULPRIT && clear_sleeps == 1U) {
		model_hold(0);
		if (clear_vm != NULL)
			clear_vm->state = GHCI_VM_FAULTED;
		model_hold(1);
	}
	clear_running = 0;
}

/*
 * The wait of vkAllocateMemory for the GPU's clear of new local memory:
 * it outlasts a GPU busy for a minute; a device fault and its reset
 * during the wait fail the allocation with out of device memory (never
 * device lost), the reset having ended the GPU's reach of the run; and a
 * clear that passed in a session found a culprit fails too, its run
 * going back only after the unmap's ticket.
 */
static void
scene_discrete_clear(void)
{
	struct ghci_vk_session *session;
	struct ghci_vk_device *owner;
	struct ghci_vk_object *object;
	struct node node;
	uint32_t result;
	unsigned fills;
	int error;

	/* A discrete device whose GPU holds the clear while a minute passes. */
	error = start(MODEL_KC705_DISCRETE);
	if (error == 0)
		error = node_open(&node);
	if (error == 0)
		error = create_device(&node, ID_DEVICE, NULL, &result);
	expect(error == 0 && result == 0U, "clear: device");
	session = ((struct ghci_session *)node.session)->vk;
	memset(model.local, 0xa5, MODEL_LOCAL_BYTES);
	fills = model_count_commands(GHCI_OP_FILL_BUFFER);
	model_hold(1);
	clear_mode = CLEAR_BUSY;
	clear_sleeps = 0U;
	stub_set_sleep_hook(clear_hook, NULL);
	error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 0, ID_MEMORY, &result);
	stub_set_sleep_hook(sleep_hook, NULL);
	clear_mode = 0;
	object = ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY);
	expect(error == 0 && result == 0U && clear_sleeps >= 4U && object != NULL &&
	       model_count_commands(GHCI_OP_FILL_BUFFER) == fills + 1U &&
	       all_bytes(model.local + (object->as.memory.allocation->bus - MODEL_LOCAL_BASE), 65536U, 0U),
	       "clear: the wait outlasts a busy GPU");

	/* The clear passed, but its session is found a culprit: out of device memory, the run out until the unmap passed. */
	owner = object != NULL ? object->device : NULL;
	clear_vm = owner != NULL ? owner->vm : NULL;
	model_hold(1);
	clear_mode = CLEAR_CULPRIT;
	clear_sleeps = 0U;
	stub_set_sleep_hook(clear_hook, NULL);
	error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 0, ID_MEMORY + 1U, &result);
	stub_set_sleep_hook(sleep_hook, NULL);
	clear_mode = 0;
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY &&
	       ghci_vk_object_find(session, GHCI_VK_MEMORY, ID_MEMORY + 1U) == NULL, "clear: a culprit's clear fails the allocation");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(device.local_map.count == 2U, "clear: the run out while its unmap is held");
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(device.local_map.count == 1U, "clear: the run back once the unmap passed");
	if (clear_vm != NULL)
		clear_vm->state = GHCI_VM_OPEN;
	clear_vm = NULL;

	/* A device fault and its reset while the GPU holds the clear: out of device memory, not device lost. */
	model_hold(1);
	clear_mode = CLEAR_RESET;
	clear_sleeps = 0U;
	stub_set_sleep_hook(clear_hook, NULL);
	error = allocate_typed(&node, ID_DEVICE, 65536U, 0U, 0, ID_MEMORY + 2U, &result);
	stub_set_sleep_hook(sleep_hook, NULL);
	clear_mode = 0;
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && device.resets == 1U &&
	       device.state == GHCI_STATE_RUNNING, "clear: a reset during the wait gives out of device memory");
	expect(owner != NULL && owner->vm->state == GHCI_VM_DEAD, "clear: the session that lost the clear is dead");
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(device.local_map.count == 1U, "clear: the run of the failed clear back after the reset");
	node_close(&node);
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
	expect(device.local_map.count == 0U, "clear: every run back");
	stop("clear");
}

/* The size of the copied presents of the display scenes: 640 x 480 pixels of 4 bytes, rows 2560 bytes apart in a frame. */
#define DISPLAY_WIDTH			640U
#define DISPLAY_HEIGHT			480U
#define DISPLAY_FRAME_STRIDE		2560U

/*
 * The display's copied presents: on the GPUs without the blitter (the
 * KC705's minimal build, flat with host frames; no-blt, unified; the
 * discrete KC705 without its blitter, with local frames) the copy is a
 * dispatch of the driver's copy program in session 0, with neither the
 * blitter's packet nor the processor's copy, and the frames are mapped
 * into session 0; the dispatch, run here by hand from its table and
 * constants, makes the frame.  A GPU with the blitter copies with the
 * blitter into local and host frames alike.  A copy program the compiler
 * refused leaves the GPU without a copied route: the scan-out query
 * offers the shared route only and a copied present is refused, as the
 * processor never copies a frame.
 */
static void
scene_display_copy(void)
{
	struct ghci_resource *storage;
	struct ghci_vk *vk;
	uint64_t lease;
	uint64_t generation;
	unsigned copies;
	unsigned dispatches;
	void *open;
	int error;

	/* The GPUs without the blitter: the copy program. */
	display_copy_case(MODEL_KC705_MIN, 0U, "display copy kc705-min");
	display_copy_case(MODEL_NO_BLT, 0U, "display copy no-blt");
	display_copy_case(MODEL_KC705_DISCRETE, GHCI_CAP_BLT, "display copy discrete without the blitter");

	/* The discrete KC705 with the blitter: CMD_COPY_BUFFER into the local frame, no dispatch. */
	error = start(MODEL_KC705_DISCRETE);
	if (error == 0)
		error = display_claim(&open, &lease, &generation);
	expect(error == 0 && device.display.frame_maps[0] != NULL, "display copy blitter: claimed, frames mapped");
	storage = display_storage(open, DISPLAY_FRAME_STRIDE * DISPLAY_HEIGHT, 21U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	error = display_send(open, lease, generation, storage, DISPLAY_FRAME_STRIDE);
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies + 1U &&
	       model_count_commands(GHCI_OP_DISPATCH) == dispatches && display_holds(device.display.frames[0], storage, DISPLAY_FRAME_STRIDE),
	       "display copy blitter: the blitter's copy, as before");
	device.ops.resource_destroy(&device, open, storage);
	display_end(open, lease);
	stop("display copy blitter");

	/* The unified KC705 with the blitter: CMD_COPY_BUFFER into the host frame, mapped into session 0, no dispatch. */
	error = start(MODEL_KC705_UMA);
	if (error == 0)
		error = display_claim(&open, &lease, &generation);
	expect(error == 0 && device.display.frames[0] != NULL && device.display.frames[0]->kind == GHCI_MEMORY_HOST &&
	       device.display.frame_maps[0] != NULL && device.display.frame_maps[1] != NULL, "display copy uma blitter: claimed, host frames mapped");
	storage = display_storage(open, DISPLAY_FRAME_STRIDE * DISPLAY_HEIGHT, 23U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	error = display_send(open, lease, generation, storage, DISPLAY_FRAME_STRIDE);
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies + 1U && model_count_commands(GHCI_OP_DISPATCH) == dispatches &&
	       display_holds(device.display.frames[0], storage, DISPLAY_FRAME_STRIDE), "display copy uma blitter: the blitter's copy");
	device.ops.resource_destroy(&device, open, storage);
	display_end(open, lease);
	stop("display copy uma blitter");

	/* A copy program the compiler refused: no copied route, no frames, a copied present refused with nothing placed. */
	error = start(MODEL_KC705_MIN);
	vk = device.vk;
	if (vk != NULL)
		vk->meta_refused[GHCI_VK_META_COPY] = 1;
	if (error == 0)
		error = display_claim(&open, &lease, &generation);
	expect(error == 0 && vk != NULL && vk->rows.memory == NULL && device.display.frames[0] == NULL &&
	       device.display.frame_maps[0] == NULL, "display copy refused: claimed, no frames");
	expect(display_routes(open, generation) == GPU_SCANOUT_SHARED, "display copy refused: the shared route only");
	storage = display_storage(open, DISPLAY_FRAME_STRIDE * DISPLAY_HEIGHT, 27U);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	copies = model.command_count;
	error = display_send(open, lease, generation, storage, DISPLAY_FRAME_STRIDE);
	expect(error == ENOTSUP && model_count_commands(GHCI_OP_DISPATCH) == dispatches && model.command_count == copies &&
	       vk != NULL && vk->rows.memory == NULL && device.window_users == 0U, "display copy refused: a copied present refused");
	device.ops.resource_destroy(&device, open, storage);
	display_end(open, lease);
	stop("display copy refused");
}

/*
 * One GPU without the blitter: a present of rows as the frame's and one
 * of rows 4096 bytes apart into the other frame, each one dispatch of the
 * copy program and nothing of the blitter or the processor.
 */
static void
display_copy_case(
	enum model_profile profile,
	uint32_t caps_clear,
	const char *name)
{
	struct ghci_resource *storage;
	struct ghci_allocation *frame;
	struct ghci_mapping *source;
	char label[160];
	uint64_t lease;
	uint64_t generation;
	unsigned copies;
	unsigned dispatches;
	unsigned mark;
	void *open;
	int error;

	/* The device and the lease; the frames mapped into session 0. */
	test_caps_clear = caps_clear;
	error = start(profile);
	test_caps_clear = 0U;
	if (error == 0)
		error = display_claim(&open, &lease, &generation);
	snprintf(label, sizeof(label), "%s: claimed, both frames mapped", name);
	expect(error == 0 && device.executor_ops != NULL && !ghci_caps_has(&device.caps, GHCI_HAS_BLT) &&
	       device.display.frame_maps[0] != NULL && device.display.frame_maps[1] != NULL, label);
	snprintf(label, sizeof(label), "%s: both routes offered", name);
	expect(display_routes(open, generation) == (GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY), label);
	if (error != 0 || device.display.frame_maps[0] == NULL) {
		stop(name);
		return;
	}

	/* Rows as the frame's: one dispatch, no COPY_BUFFER, the frame untouched by the processor. */
	storage = display_storage(open, DISPLAY_FRAME_STRIDE * DISPLAY_HEIGHT, 31U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	mark = model.command_count;
	error = display_send(open, lease, generation, storage, DISPLAY_FRAME_STRIDE);
	frame = device.display.frames[0];
	snprintf(label, sizeof(label), "%s: one dispatch, no blitter, no processor", name);
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies && model_count_commands(GHCI_OP_DISPATCH) == dispatches + 1U &&
	       !display_holds(frame, storage, DISPLAY_FRAME_STRIDE) && device.vk != NULL && ((struct ghci_vk *)device.vk)->rows.memory != NULL, label);
	snprintf(label, sizeof(label), "%s: the window not used", name);
	expect(device.window_users == 0U && model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U] == 0U, label);

	/* The dispatch's packets and table, and the frame it makes. */
	source = NULL;
	for (source = device.vm0.mappings; source != NULL; source = source->next) {
		if (source->allocation == storage->allocation)
			break;
	}
	snprintf(label, sizeof(label), "%s: the storage's mapping let go", name);
	expect(source == NULL || source->state == GHCI_MAPPING_UNMAPPING, label);
	snprintf(label, sizeof(label), "%s: the dispatch copies the rows", name);
	expect(display_dispatch_check(mark, DISPLAY_FRAME_STRIDE, device.display.frame_maps[0], storage->allocation) &&
	       display_holds(frame, storage, DISPLAY_FRAME_STRIDE), label);
	device.ops.resource_destroy(&device, open, storage);

	/* Rows 4096 bytes apart, into the other frame: one dispatch again, the program placed once. */
	storage = display_storage(open, 4096U * DISPLAY_HEIGHT, 37U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	dispatches = model_count_commands(GHCI_OP_DISPATCH);
	mark = model.command_count;
	error = display_send(open, lease, generation, storage, 4096U);
	frame = device.display.frames[1];
	snprintf(label, sizeof(label), "%s: another stride, the other frame", name);
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies && model_count_commands(GHCI_OP_DISPATCH) == dispatches + 1U &&
	       display_dispatch_check(mark, 4096U, device.display.frame_maps[1], storage->allocation) && display_holds(frame, storage, 4096U), label);
	device.ops.resource_destroy(&device, open, storage);
	display_end(open, lease);
	stop(name);
}

/* Reports the routes the scan-out query offers for the output's generation (GPU_SCANOUT_*), or 0 when the query fails. */
static uint32_t
display_routes(
	void *open,
	uint64_t generation)
{
	struct gpu_scanout_constraints constraints;
	int error;

	/* The query of the one output. */
	memset(&constraints, 0, sizeof(constraints));
	constraints.display_id = 1U;
	constraints.generation = generation;
	error = device.ops.scanout->constraints(&device, open, &constraints);
	if (error != 0)
		return 0U;
	return constraints.flags;
}

/* Opens a session of the core and takes the display's lease.  Returns what the claim returned. */
static int
display_claim(
	void **open,
	uint64_t *lease,
	uint64_t *generation)
{
	struct gpu_display_info info;
	struct gpu_display_claim claim;
	int error;

	/* The open, and the output's generation. */
	*open = NULL;
	error = device.ops.open(&device, open);
	if (error != 0)
		return error;
	memset(&info, 0, sizeof(info));
	error = device.ops.display->query(&device, *open, &info);
	if (error != 0)
		return error;
	*generation = info.generation;

	/* The lease of plane 0. */
	memset(&claim, 0, sizeof(claim));
	claim.display_id = 1U;
	claim.generation = *generation;
	error = device.ops.display->claim(&device, *open, &claim);
	*lease = claim.lease;
	return error;
}

/* Makes ordinary storage of an open, filled with a pattern (each word its offset plus seed); NULL when it could not. */
static struct ghci_resource *
display_storage(
	void *open,
	uint64_t bytes,
	uint32_t seed)
{
	struct gpu_resource_create create;
	struct ghci_resource *storage;
	uint32_t *words;
	uint64_t index;
	void *object;
	int error;

	/* The storage. */
	memset(&create, 0, sizeof(create));
	create.bytes = bytes;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	error = device.ops.resource_create(&device, open, &create, &object);
	if (error != 0)
		return NULL;
	storage = object;

	/* The pattern. */
	words = (uint32_t *)(void *)storage->allocation->view;
	for (index = 0U; index < storage->allocation->bytes / 4U; index++)
		words[index] = (uint32_t)index * 4U + seed;
	return storage;
}

/* Presents storage of 640 x 480 pixels, its rows stride bytes apart, copied (FIFO).  Returns what the present returned. */
static int
display_send(
	void *open,
	uint64_t lease,
	uint64_t generation,
	struct ghci_resource *storage,
	uint32_t stride)
{
	struct gpu_display_present request;

	/* Ordinary storage is copied. */
	if (storage == NULL)
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.lease = lease;
	request.generation = generation;
	request.width = DISPLAY_WIDTH;
	request.height = DISPLAY_HEIGHT;
	request.stride = stride;
	request.format = GPU_PIXEL_BGRA8888;
	request.flags = GPU_DISPLAY_PRESENT_FIFO;
	return device.ops.display->present(&device, open, storage, &request);
}

/* Lets the display's lease go and closes the open. */
static void
display_end(
	void *open,
	uint64_t lease)
{
	struct gpu_display_release release;

	/* The release, then the close. */
	memset(&release, 0, sizeof(release));
	release.lease = lease;
	(void)device.ops.display->release(&device, open, &release);
	device.ops.close(&device, open);
}

/* Reports whether a frame holds the 640 x 480 pixels of storage, its rows stride bytes apart: 1 when it does. */
static int
display_holds(
	struct ghci_allocation *frame,
	struct ghci_resource *storage,
	uint32_t stride)
{
	uint8_t *target;
	uint32_t row;

	/* The frame as the GPU's memory holds it. */
	if (frame == NULL || storage == NULL)
		return 0;
	target = model_dma(frame->bus, (uint64_t)DISPLAY_FRAME_STRIDE * DISPLAY_HEIGHT);
	if (target == NULL)
		return 0;

	/* Each row. */
	for (row = 0U; row < DISPLAY_HEIGHT; row++) {
		if (memcmp(target + (uint64_t)row * DISPLAY_FRAME_STRIDE, storage->allocation->view + (uint64_t)row * stride,
			   DISPLAY_WIDTH * 4U) != 0)
			return 0;
	}
	return 1;
}

/*
 * Checks the copy program's packets after mark (in session 0: the head
 * barrier, the program's pipeline, its resource table, its constants and
 * one dispatch, the tail barrier), its table (the storage read only, the
 * frame) and constants (the rows of 640 words, stride bytes apart in the
 * storage and 2560 in the frame), and runs the dispatch by hand: each
 * invocation copies its word, from the storage's memory to the frame's
 * (the storage's mapping, whose address the table gives, is gone by
 * now).  Returns 1 when everything is as it should be.
 */
static int
display_dispatch_check(
	unsigned mark,
	uint32_t stride,
	const struct ghci_mapping *frame_map,
	const struct ghci_allocation *storage)
{
	const struct model_command *command;
	const struct ghci_vk_rows *rows;
	const uint32_t *push;
	const uint8_t *table;
	uint8_t *from;
	uint8_t *to;
	uint64_t source_base;
	uint64_t destination_base;
	uint64_t source_at;
	uint64_t destination_at;
	uint32_t source_bytes;
	uint32_t destination_bytes;
	uint8_t source_flags;
	uint32_t groups;
	uint32_t element;
	uint32_t column;
	uint32_t row;
	unsigned order[6];
	unsigned found;
	unsigned index;

	/* The packets in order, all in session 0. */
	rows = &((struct ghci_vk *)device.vk)->rows;
	order[0] = GHCI_OP_BARRIER;
	order[1] = GHCI_OP_BIND_PIPELINE;
	order[2] = GHCI_OP_BIND_DESCRIPTOR_TABLE;
	order[3] = GHCI_OP_LOAD_CONST;
	order[4] = GHCI_OP_DISPATCH;
	order[5] = GHCI_OP_BARRIER;
	found = 0U;
	push = NULL;
	groups = 0U;
	for (index = mark; index < model.command_count && found < 6U; index++) {
		command = &model.commands[index];
		if (command->opcode != order[found])
			continue;
		if (command->session != 0U)
			return 0;
		if (found == 1U && (command->words[1] | ((uint64_t)command->words[2] << 32)) != rows->pipeline.pso_va)
			return 0;
		if (found == 2U && (command->words[1] | ((uint64_t)command->words[2] << 32)) != rows->mapping->va + rows->table_at)
			return 0;
		if (found == 3U)
			push = &command->words[3];
		if (found == 4U)
			groups = command->words[1];
		found++;
	}
	if (found != 6U || push == NULL)
		return 0;

	/* The table: the storage read only from its first word, the frame. */
	table = rows->memory->view + rows->table_at;
	memcpy(&source_base, table + rows->pipeline.source_entry + GSI1_ABI_BUFFER_BASE, 8U);
	memcpy(&source_bytes, table + rows->pipeline.source_entry + GSI1_ABI_BUFFER_SIZE, 4U);
	source_flags = table[rows->pipeline.source_entry + GHCI_BUFFER_FLAGS];
	memcpy(&destination_base, table + rows->pipeline.destination_entry + GSI1_ABI_BUFFER_BASE, 8U);
	memcpy(&destination_bytes, table + rows->pipeline.destination_entry + GSI1_ABI_BUFFER_SIZE, 4U);
	if (source_base == 0U || frame_map == NULL || destination_base != frame_map->va || (source_flags & 1U) == 0U)
		return 0;
	if (source_bytes != stride * (DISPLAY_HEIGHT - 1U) + DISPLAY_WIDTH * 4U)
		return 0;
	if (destination_bytes != DISPLAY_FRAME_STRIDE * (DISPLAY_HEIGHT - 1U) + DISPLAY_WIDTH * 4U)
		return 0;
	/* The constants: the words, a row's, the rows, every byte, each side's steps, 4 bytes from element 0. */
	if (push[GHCI_VK_META_COPY_COUNT] != DISPLAY_WIDTH * DISPLAY_HEIGHT || push[GHCI_VK_META_COPY_PER_ROW] != DISPLAY_WIDTH ||
	    push[GHCI_VK_META_COPY_ROWS] != DISPLAY_HEIGHT || push[GHCI_VK_META_COPY_MASK] != 0xffffffffU)
		return 0;
	if (push[GHCI_VK_META_COPY_SOURCE] != 0U || push[GHCI_VK_META_COPY_SOURCE + 1U] != 4U ||
	    push[GHCI_VK_META_COPY_SOURCE + 2U] != stride || push[GHCI_VK_META_COPY_DESTINATION] != 0U ||
	    push[GHCI_VK_META_COPY_DESTINATION + 1U] != 4U || push[GHCI_VK_META_COPY_DESTINATION + 2U] != DISPLAY_FRAME_STRIDE ||
	    push[GHCI_VK_META_COPY_BYTES] != 4U || push[GHCI_VK_META_COPY_FIRST] != 0U)
		return 0;
	if ((uint64_t)groups * GHCI_VK_META_GROUP < DISPLAY_WIDTH * DISPLAY_HEIGHT)
		return 0;

	/* The dispatch by hand: each element's word from the storage to the frame. */
	from = model_dma(storage->bus, source_bytes);
	to = model_dma(frame_map->allocation->bus, destination_bytes);
	if (from == NULL || to == NULL)
		return 0;
	for (element = 0U; element < DISPLAY_WIDTH * DISPLAY_HEIGHT; element++) {
		column = element % DISPLAY_WIDTH;
		row = element / DISPLAY_WIDTH;
		source_at = (uint64_t)column * 4U + (uint64_t)row * stride;
		destination_at = (uint64_t)column * 4U + (uint64_t)row * DISPLAY_FRAME_STRIDE;
		memcpy(to + destination_at, from + source_at, 4U);
	}
	return 1;
}
