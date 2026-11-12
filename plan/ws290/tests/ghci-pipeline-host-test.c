/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the pipelines and descriptors of the GHCI 1.0
 * driver's Vulkan executor (vk/vk-pipeline.c, vk/vk-layout.c) against
 * the GPU model and the kernel stubs (design section 14, the scenes of
 * p008): representative pipelines (a triangle, blending, depth, stencil,
 * multisampling, compute) and their 128-byte pipeline state records, the
 * slot tables, the bytes of the hardware sets of a draw and a dispatch,
 * the descriptor sets' life, the pipelines refused on devices without
 * the flags they need, the specialization of constants, operations and
 * the workgroup size, Vulkan counts that are not their arrays', and the
 * session's budget.
 *
 * The executor is driven through the node's operations with frames of
 * the wire built by hand as libvulkan encodes them.  The shaders are
 * GLSL compiled by glslc (the vertex, fragment and compute shaders of
 * the directory pipeline); the script
 * passes the directory of the SPIR-V.  Every expected byte of a record or
 * a descriptor is written here by hand from the specification (ghci.md
 * d9252e4 sections 8.2 and 8.3) and the driver's design (section 11.4);
 * what only the compiler decides (the registers a program uses, which
 * slot a binding takes) is read from the pipeline's record and checked
 * to be where the specification puts it.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/gpu/gpu.h"
#include "drivers/gpu/ghci/ghci.h"
#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "drivers/gpu/ghci/compiler/gsi1_isa.h"
#include "drivers/gpu/ghci/vk/vk.h"
#include "kern/lock.h"
#include "uapi/errno.h"
#include "uapi/gpu.h"
#include "uapi/gpu-op.h"
#include "ghci-model.h"
#include "ghci-stubs.h"

/* The PCI IDs the scenes give the device (the KC705 board). */
#define TEST_PCI_VENDOR			0x10eeU
#define TEST_PCI_DEVICE			0x7024U

/* The reply blob: its bytes, and where the trailer (the version probe's 20 bytes) goes. */
#define REPLY_BYTES			4096U
#define TRAILER_AT			(REPLY_BYTES - 20U)

/* The most bytes of one hand-built frame, and of one SPIR-V module read from a file. */
#define FRAME_BYTES			(64U * 1024U)
#define MODULE_BYTES			(48U * 1024U)

/* The identities the scenes give their objects (any nonzero value). */
#define ID_PHYSICAL			0x101U
#define ID_DEVICE			0x200U
#define ID_MEMORY			0x400U
#define ID_BUFFER			0x500U
#define ID_IMAGE			0x600U
#define ID_SAMPLER			0x1000U
#define ID_IMAGE_VIEW			0x1100U
#define ID_BUFFER_VIEW			0x1200U
#define ID_MODULE			0x1300U
#define ID_CACHE			0x1400U
#define ID_SET_LAYOUT			0x1500U
#define ID_LAYOUT			0x1600U
#define ID_POOL				0x1700U
#define ID_SET				0x1800U
#define ID_PIPELINE			0x1900U

/* A count of a graphics pipeline's record one more than its array (make_graphics). */
#define SKEW_NONE			0U
#define SKEW_ATTRIBUTES			1U
#define SKEW_BLEND			2U
#define SKEW_DYNAMIC			3U

/* The modules the scenes make, by the index of their file. */
enum module_index {
	MODULE_TRI_VERT = 0,
	MODULE_TRI_FRAG,
	MODULE_MRT_FRAG,
	MODULE_TEX_FRAG,
	MODULE_VS8_VERT,
	MODULE_FS9_FRAG,
	MODULE_SAMPLES_FRAG,
	MODULE_DYN_COMP,
	MODULE_WIDE_COMP,
	MODULE_SPEC_COMP,
	MODULE_ATTRIB60_VERT,
	MODULE_SPILL_COMP,
	MODULES
};

/* The files of the modules, in the order of enum module_index (attrib60 and spill from the compiler's test). */
static const char *const module_files[MODULES] = {
	"tri.vert.spv", "tri.frag.spv", "mrt.frag.spv", "tex.frag.spv", "vs8.vert.spv", "fs9.frag.spv",
	"samples.frag.spv", "dyn.comp.spv", "wide.comp.spv", "spec.comp.spv", "attrib60.vert.spv", "spill.comp.spv"
};

/* One frame being built: the bytes and how many are used. */
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

/* One vertex attribute and binding of a graphics pipeline a scene makes. */
struct test_attribute {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * A graphics pipeline a scene makes: its modules (fragment 0 for none),
 * topology, raster state, samples, depth and stencil, blend attachments,
 * dynamic states and vertex input, and a count to skew (SKEW_*).
 */
struct test_graphics {
	uint64_t vertex;
	uint64_t fragment;
	uint32_t topology;
	uint32_t cull;
	uint32_t front_cw;
	uint32_t polygon;
	uint32_t samples;
	uint32_t sample_mask;
	uint32_t alpha_to_coverage;
	uint32_t depth;
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_op;
	uint32_t stencil;
	uint32_t front[7];
	uint32_t back[7];
	uint32_t attachments;
	uint32_t blend[8][8];
	uint32_t logic;
	uint32_t logic_op;
	uint32_t dynamic_count;
	uint32_t dynamic[9];
	uint32_t attribute_count;
	struct test_attribute attributes[16];
	uint32_t binding_count;
	uint32_t strides[4];
	uint64_t layout;
	uint32_t skew;
};

/*
 * A render pass a scene makes: its attachments (format, samples, load
 * and store operations, the same for stencil) and its subpasses (colour
 * attachments, their resolves when resolves is set, and the depth
 * attachment, VK_ATTACHMENT_UNUSED for none).
 */
struct test_pass {
	uint32_t attachment_count;
	uint32_t format[8];
	uint32_t samples[8];
	uint32_t load[8];
	uint32_t store[8];
	uint32_t subpass_count;
	uint32_t color_count[2];
	uint32_t color[2][8];
	uint32_t resolves[2];
	uint32_t resolve[2][8];
	uint32_t depth[2];
};

/* The render pass every graphics pipeline of the scenes is made for. */
#define ID_PIPELINE_PASS		0x7777U

/* The counts of the checks. */
static unsigned checks_passed;
static unsigned checks_failed;

/* The device of the scene; one at a time. */
static struct ghci_device device;

/* The frame the helpers build (one at a time; too large for the stack), and what the last run returned. */
static struct frame frame;
static int last_run;

/* The capability bits a scene takes away from its profile, and the ring's bytes it asks for (0: the driver's). */
static uint32_t test_caps_off;
static uint32_t test_ring_bytes;

/* The SPIR-V of each module and its bytes, read once at the start. */
static uint8_t modules[MODULES][MODULE_BYTES];
static size_t module_bytes[MODULES];

static void expect(int held, const char *name);
static int load_modules(const char *directory, const char *compiler_directory);
static int start(enum model_profile profile);
static void stop(const char *name);
static void model_irq(void *argument);
static int fake_interrupt_take(struct ghci_device *target);
static int fake_interrupt_release(struct ghci_device *target);
static int fake_bus_master(struct ghci_device *target, int on);
static int node_open(struct node *node);
static void node_close(struct node *node);
static void put32(uint32_t value);
static void put64(uint64_t value);
static void put_bytes(const void *data, size_t bytes);
static void begin(const struct node *node, uint32_t opcode);
static int run(struct node *node);
static void vertex_formats(void);
static uint32_t reply32(const struct node *node, size_t at);
static uint64_t reply64(const struct node *node, size_t at);
static void tail(uint64_t identity);
static struct ghci_vk_session *session_of(const struct node *node);
static int open_device(struct node *node, enum model_profile profile);
static int make_modules(struct node *node);
static uint32_t make_set_layout(struct node *node, uint64_t identity, uint32_t count, const uint32_t *types, const uint32_t *stages, uint64_t immutable);
static uint32_t make_layout(struct node *node, uint64_t identity, uint32_t sets, const uint64_t *set_layouts, uint32_t push_bytes);
static void put_stage(uint32_t stage, uint64_t module, const uint32_t *spec, uint32_t spec_count);
static uint32_t make_render_pass(struct node *node, uint64_t identity, const struct test_pass *pass);
static uint32_t make_graphics(struct node *node, const struct test_graphics *graphics, uint64_t identity);
static uint32_t make_compute(struct node *node, uint64_t module, uint64_t layout, const uint32_t *spec, uint32_t spec_count, uint64_t identity);
static void graphics_defaults(struct test_graphics *graphics);
static const struct ghci_vk_pipeline *pipeline_of(const struct node *node, uint64_t identity);
static uint32_t pso_word(const struct ghci_vk_pipeline *pipeline, uint32_t dword);
static void store_le(uint8_t *bytes, uint32_t offset, uint64_t value, uint32_t size);
static uint32_t blend_word_by_hand(uint32_t enable, uint32_t src_color, uint32_t dst_color, uint32_t color_op, uint32_t src_alpha, uint32_t dst_alpha, uint32_t alpha_op, uint32_t mask);
static void scene_triangle(void);
static void scene_blend(void);
static void scene_depth_stencil(void);
static void scene_msaa(void);
static void scene_compute(void);
static void scene_descriptors(void);
static void scene_refusals(void);
static void scene_caches(void);
static void scene_scratch(void);
static void scene_counts(void);
static void scene_budget(void);
static int execute_main(const char *directory, const char *compiler_directory);
static int meta_main(const char *directory, const char *compiler_directory, const char *tests, const char *tools, const char *out);

/* The operations of the fake bus; they live as long as the test. */
static const struct ghci_bus_ops fake_bus_ops = {
	fake_interrupt_take,
	fake_interrupt_release,
	fake_bus_master
};

/*
 * Runs the scenes and reports: argv[1] is the directory of the scenes'
 * SPIR-V, argv[2] the one of the compiler test's (attrib60, spill).
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/*
	 * The modules, and the scenes asked for (the pipelines', the execution's
	 * with "execute", or the transfer programs' with "meta" and the
	 * directories of the tests, the GSI1 tools and the model's files).
	 */
	if (argc == 7 && strcmp(argv[3], "meta") == 0)
		return meta_main(argv[1], argv[2], argv[4], argv[5], argv[6]);
	if (argc != 3 && (argc != 4 || strcmp(argv[3], "execute") != 0)) {
		printf("usage: ghci-pipeline-host-test spirv-directory compiler-spirv-directory [execute | meta tests tools out]\n");
		return 2;
	}
	if (argc == 4)
		return execute_main(argv[1], argv[2]);
	error = load_modules(argv[1], argv[2]);
	if (error != 0) {
		printf("FAIL ghci-pipeline-host-test: the SPIR-V modules are not there\n");
		return 1;
	}

	/* Keeps the driver's log out of the report. */
	stub.quiet = 1;

	/* The scenes. */
	scene_triangle();
	scene_blend();
	scene_depth_stencil();
	scene_msaa();
	scene_compute();
	scene_descriptors();
	scene_refusals();
	scene_caches();
	scene_scratch();
	scene_counts();
	scene_budget();
	model_fini();

	/* The counts. */
	printf("ghci-pipeline-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U) {
		printf("FAIL ghci-pipeline-host-test\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS ghci-pipeline-host-test\n");
	return 0;
}

/* The service thread is not started on the host. */
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

/* Reads the SPIR-V of every module.  Returns 0, or 1 for one that is missing or too large. */
static int
load_modules(
	const char *directory,
	const char *compiler_directory)
{
	char path[1024];
	FILE *file;
	unsigned index;

	/* Each file, from the scenes' directory or the compiler test's. */
	for (index = 0U; index < MODULES; index++) {
		if (index == MODULE_ATTRIB60_VERT || index == MODULE_SPILL_COMP)
			snprintf(path, sizeof(path), "%s/%s", compiler_directory, module_files[index]);
		else
			snprintf(path, sizeof(path), "%s/%s", directory, module_files[index]);
		file = fopen(path, "rb");
		if (file == NULL) {
			printf("missing %s\n", path);
			return 1;
		}
		module_bytes[index] = fread(modules[index], 1U, MODULE_BYTES, file);
		fclose(file);
		if (module_bytes[index] == 0U || module_bytes[index] == MODULE_BYTES || (module_bytes[index] & 3U) != 0U)
			return 1;
	}
	return 0;
}

/* Starts a device of a profile, with the driver's lock order and forbidden waits. */
static int
start(
	enum model_profile profile)
{
	static const char *const order[] = {
		"ghci device", "ghci display", "ghci ring", "ghci interrupt"
	};

	/* The model and the stubs, with the rules the driver must keep; the scene's bits taken away. */
	model_init(profile, NULL);
	model.window[GHCI_REG_DEVICE_CAPS / 4U] &= ~test_caps_off;
	stub_reset();
	stub_lock_order(order, 4U);
	stub_forbid_wait("ghci fence", "ghci device");
	stub_forbid_wait("ghci fence", "ghci ring");

	/* The device as the bus glue leaves it. */
	memset(&device, 0, sizeof(device));
	device.registers = stub_bar0();
	device.register_bytes = 4096U;
	device.bus_ops = &fake_bus_ops;
	device.pci_vendor = TEST_PCI_VENDOR;
	device.pci_device = TEST_PCI_DEVICE;
	device.ring_bytes = test_ring_bytes;
	return ghci_device_start(&device);
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
	snprintf(label, sizeof(label), "%s: lock order and waits", name);
	expect(stub.order_violations == 0U && stub.wait_violations == 0U, label);
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

/* Closes a node session as the core does: the drain, then the close. */
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
	uint32_t value)
{
	memcpy(frame.bytes + frame.used, &value, 4U);
	frame.used += 4U;
}

/* Appends a 64-bit word, little-endian. */
static void
put64(
	uint64_t value)
{
	memcpy(frame.bytes + frame.used, &value, 8U);
	frame.used += 8U;
}

/* Appends a run of bytes padded to four. */
static void
put_bytes(
	const void *data,
	size_t bytes)
{
	memset(frame.bytes + frame.used, 0, (bytes + 3U) & ~(size_t)3U);
	memcpy(frame.bytes + frame.used, data, bytes);
	frame.used += (bytes + 3U) & ~(size_t)3U;
}

/* Starts a frame: the reply stream (the whole blob), then the command's header with the reply flag. */
static void
begin(
	const struct node *node,
	uint32_t opcode)
{
	frame.used = 0U;
	put32(GPU_OP_SET_REPLY_STREAM);
	put32(0U);
	put64(1U);
	put32(node->reply_id);
	put64(0U);
	put64(REPLY_BYTES);
	put32(opcode);
	put32(GPU_OP_FLAG_REPLY);
}

/* Ends a frame with the trailer, clears the replies, and runs it.  Returns what the command returned. */
static int
run(
	struct node *node)
{
	put32(GPU_OP_SEEK_REPLY_STREAM);
	put32(0U);
	put64(TRAILER_AT);
	put32(GPU_OP_ENUMERATE_INSTANCE_VERSION);
	put32(GPU_OP_FLAG_REPLY);
	put64(1U);
	memset(node->reply, 0, REPLY_BYTES);
	last_run = device.ops.command(&device, node->session, frame.bytes, (uint32_t)frame.used);
	return last_run;
}

/*
 * Gives the vertex formats the scenes fetch VERTEX_BUFFER: the model's
 * FMT reports the features of a few colour formats alone, where a
 * device's reports those of every format of its table.
 */
static void
vertex_formats(void)
{
	static const uint32_t formats[4] = {
		VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT
	};
	struct ghci_vk *vk;
	unsigned index;

	vk = device.vk;
	for (index = 0U; index < 4U; index++)
		vk->formats.buffer[formats[index]] |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
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

/* The tail of a create: no allocator, the identity. */
static void
tail(
	uint64_t identity)
{
	put64(0U);
	put64(1U);
	put64(identity);
}

/* Reports the executor's side of a node session. */
static struct ghci_vk_session *
session_of(
	const struct node *node)
{
	return ((struct ghci_session *)node->session)->vk;
}

/*
 * Starts a device of a profile and opens a node session with a VkDevice,
 * 1 MiB of memory, a 2D image of 64 x 32 R8G8B8A8_UNORM texels with
 * three levels and two layers at offset 0, and a buffer of 8192 bytes at
 * offset 0x80000.  Returns 0, or what failed.
 */
static int
open_device(
	struct node *node,
	enum model_profile profile)
{
	int error;

	/* The device and the session, and the vertex formats the scenes fetch. */
	error = start(profile);
	if (error == 0)
		error = node_open(node);
	if (error != 0)
		return error;
	vertex_formats();

	/* vkCreateDevice: one queue of family 0, no feature. */
	begin(node, GPU_OP_CREATE_DEVICE);
	put64(ID_PHYSICAL);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(0x3f800000U);
	put32(0U);
	put64(0U);
	put32(0U);
	put64(0U);
	put64(0U);
	tail(ID_DEVICE);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* vkAllocateMemory: 1 MiB of type 0. */
	begin(node, GPU_OP_ALLOCATE_MEMORY);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	put64(0U);
	put64(1024U * 1024U);
	put32(0U);
	tail(ID_MEMORY);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* vkCreateImage: 2D, 64 x 32, R8G8B8A8_UNORM, three levels, two layers, sampled and storage. */
	begin(node, GPU_OP_CREATE_IMAGE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_IMAGE_TYPE_2D);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(64U);
	put32(32U);
	put32(1U);
	put32(3U);
	put32(2U);
	put32(VK_SAMPLE_COUNT_1_BIT);
	put32(VK_IMAGE_TILING_OPTIMAL);
	put32(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
	put32(VK_SHARING_MODE_EXCLUSIVE);
	put32(0U);
	put64(0U);
	put32(VK_IMAGE_LAYOUT_UNDEFINED);
	tail(ID_IMAGE);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_BIND_IMAGE_MEMORY);
	put64(ID_DEVICE);
	put64(ID_IMAGE);
	put64(ID_MEMORY);
	put64(0U);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* vkCreateBuffer of 8192 bytes, bound at 0x80000. */
	begin(node, GPU_OP_CREATE_BUFFER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(8192U);
	put32(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	put32(VK_SHARING_MODE_EXCLUSIVE);
	put32(0U);
	put64(0U);
	tail(ID_BUFFER);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;
	begin(node, GPU_OP_BIND_BUFFER_MEMORY);
	put64(ID_DEVICE);
	put64(ID_BUFFER);
	put64(ID_MEMORY);
	put64(0x80000U);
	error = run(node);
	if (error != 0 || reply32(node, 4U) != 0U)
		return 1;

	/* The modules. */
	return make_modules(node);
}

/* Makes a shader module of each file (identity ID_MODULE + index).  Returns 0, or 1. */
static int
make_modules(
	struct node *node)
{
	unsigned index;
	int error;

	/* vkCreateShaderModule: [device][u64 1][sType][u64 0][flags][u64 bytes][u64 words][words] and the tail. */
	for (index = 0U; index < MODULES; index++) {
		begin(node, GPU_OP_CREATE_SHADER_MODULE);
		put64(ID_DEVICE);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put64(module_bytes[index]);
		put64(module_bytes[index] / 4U);
		put_bytes(modules[index], module_bytes[index]);
		tail(ID_MODULE + index);
		error = run(node);
		if (error != 0 || reply32(node, 4U) != 0U)
			return 1;
	}
	return 0;
}

/*
 * vkCreateDescriptorSetLayout of bindings 0 to count - 1, one descriptor
 * each of the types, with one immutable sampler on a combined image
 * sampler binding when immutable is not 0.  Returns the result, or ~0
 * when the frame failed.
 */
static uint32_t
make_set_layout(
	struct node *node,
	uint64_t identity,
	uint32_t count,
	const uint32_t *types,
	const uint32_t *stages,
	uint64_t immutable)
{
	uint32_t index;
	int error;

	/* The create info and its bindings. */
	begin(node, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(count);
	put64(count);
	for (index = 0U; index < count; index++) {
		put32(index);
		put32(types[index]);
		put32(1U);
		put32(stages[index]);
		if (immutable != 0U && types[index] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			put64(1U);
			put64(immutable);
		} else {
			put64(0U);
		}
	}
	tail(identity);
	error = run(node);
	if (error != 0)
		return ~0U;
	return reply32(node, 4U);
}

/* vkCreatePipelineLayout of set layouts and one push-constant range.  Returns the result, or ~0. */
static uint32_t
make_layout(
	struct node *node,
	uint64_t identity,
	uint32_t sets,
	const uint64_t *set_layouts,
	uint32_t push_bytes)
{
	uint32_t index;
	int error;

	/* The create info. */
	begin(node, GPU_OP_CREATE_PIPELINE_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(sets);
	put64(sets);
	for (index = 0U; index < sets; index++)
		put64(set_layouts[index]);
	if (push_bytes != 0U) {
		put32(1U);
		put64(1U);
		put32(VK_SHADER_STAGE_ALL);
		put32(0U);
		put32(push_bytes);
	} else {
		put32(0U);
		put64(0U);
	}
	tail(identity);
	error = run(node);
	if (error != 0)
		return ~0U;
	return reply32(node, 4U);
}

/*
 * One VkPipelineShaderStageCreateInfo: the stage, the module, "main" and
 * the specialization (pairs of constant ID and 32-bit value).
 */
static void
put_stage(
	uint32_t stage,
	uint64_t module,
	const uint32_t *spec,
	uint32_t spec_count)
{
	uint32_t index;

	put32(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(stage);
	put64(module);
	put64(5U);
	put_bytes("main", 5U);
	if (spec_count == 0U) {
		put64(0U);
		return;
	}

	/* [u64 1][mapEntryCount][u64 n]{id, offset, u64 size}[u64 dataSize][u64 n][data]. */
	put64(1U);
	put32(spec_count);
	put64(spec_count);
	for (index = 0U; index < spec_count; index++) {
		put32(spec[2U * index]);
		put32(4U * index);
		put64(4U);
	}
	put64(4U * spec_count);
	put64(4U * spec_count);
	for (index = 0U; index < spec_count; index++)
		put32(spec[2U * index + 1U]);
}

/* Fills a graphics pipeline's defaults: the triangle of tri.vert and tri.frag, one opaque attachment. */
static void
graphics_defaults(
	struct test_graphics *graphics)
{
	memset(graphics, 0, sizeof(*graphics));
	graphics->vertex = ID_MODULE + MODULE_TRI_VERT;
	graphics->fragment = ID_MODULE + MODULE_TRI_FRAG;
	graphics->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	graphics->cull = VK_CULL_MODE_BACK_BIT;
	graphics->samples = VK_SAMPLE_COUNT_1_BIT;
	graphics->attachments = 1U;
	graphics->blend[0][7] = 0xfU;
	graphics->attribute_count = 2U;
	graphics->attributes[0].location = 0U;
	graphics->attributes[0].binding = 0U;
	graphics->attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	graphics->attributes[0].offset = 0U;
	graphics->attributes[1].location = 1U;
	graphics->attributes[1].binding = 0U;
	graphics->attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	graphics->attributes[1].offset = 8U;
	graphics->binding_count = 1U;
	graphics->strides[0] = 20U;
	graphics->layout = ID_LAYOUT;
}

/*
 * vkCreateRenderPass as libvulkan encodes it (the same pass made again
 * replaces it).  Returns the result (~0 when the frame failed).
 */
static uint32_t
make_render_pass(
	struct node *node,
	uint64_t identity,
	const struct test_pass *pass)
{
	uint32_t index;
	uint32_t at;
	int error;

	/* The old pass of the identity goes. */
	begin(node, GPU_OP_DESTROY_RENDER_PASS);
	put64(ID_DEVICE);
	put64(identity);
	put64(0U);
	(void)run(node);

	/* [device][u64 1], the head and the attachments. */
	begin(node, GPU_OP_CREATE_RENDER_PASS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(pass->attachment_count);
	put64(pass->attachment_count);
	for (index = 0U; index < pass->attachment_count; index++) {
		put32(0U);
		put32(pass->format[index]);
		put32(pass->samples[index]);
		put32(pass->load[index]);
		put32(pass->store[index]);
		put32(pass->load[index]);
		put32(pass->store[index]);
		put32(VK_IMAGE_LAYOUT_UNDEFINED);
		put32(VK_IMAGE_LAYOUT_GENERAL);
	}

	/* The subpasses: no inputs, the colours, the resolves, the depth, no preserves. */
	put32(pass->subpass_count);
	put64(pass->subpass_count);
	for (index = 0U; index < pass->subpass_count; index++) {
		put32(0U);
		put32(VK_PIPELINE_BIND_POINT_GRAPHICS);
		put32(0U);
		put64(0U);
		put32(pass->color_count[index]);
		put64(pass->color_count[index]);
		for (at = 0U; at < pass->color_count[index]; at++) {
			put32(pass->color[index][at]);
			put32(VK_IMAGE_LAYOUT_GENERAL);
		}
		put64(pass->resolves[index] != 0U ? pass->color_count[index] : 0U);
		for (at = 0U; pass->resolves[index] != 0U && at < pass->color_count[index]; at++) {
			put32(pass->resolve[index][at]);
			put32(VK_IMAGE_LAYOUT_GENERAL);
		}
		put64(pass->depth[index] != VK_ATTACHMENT_UNUSED ? 1U : 0U);
		if (pass->depth[index] != VK_ATTACHMENT_UNUSED) {
			put32(pass->depth[index]);
			put32(VK_IMAGE_LAYOUT_GENERAL);
		}
		put32(0U);
		put64(0U);
	}

	/* No dependencies, then the tail. */
	put32(0U);
	put64(0U);
	tail(identity);
	error = run(node);
	if (error != 0)
		return ~0U;

	/* Succeeded: the result. */
	return reply32(node, 4U);
}

/*
 * vkCreateGraphicsPipelines of one pipeline as libvulkan encodes it, for
 * subpass 0 of ID_PIPELINE_PASS, which is made first to match the
 * pipeline (its colour attachments R8G8B8A8_UNORM, a depth and stencil
 * attachment when the pipeline tests depth or stencil, its samples).
 * Returns the result (~0 when the frame failed); the reply holds the
 * identity or 0.
 */
static uint32_t
make_graphics(
	struct node *node,
	const struct test_graphics *graphics,
	uint64_t identity)
{
	struct test_pass pass;
	uint32_t index;
	uint32_t word;
	int error;

	/* The render pass of the pipeline. */
	memset(&pass, 0, sizeof(pass));
	pass.subpass_count = 1U;
	pass.color_count[0] = graphics->attachments;
	pass.depth[0] = VK_ATTACHMENT_UNUSED;
	for (index = 0U; index < graphics->attachments && index < 8U; index++) {
		pass.format[index] = VK_FORMAT_R8G8B8A8_UNORM;
		pass.samples[index] = graphics->samples != 0U ? graphics->samples : 1U;
		pass.color[0][index] = index;
	}
	pass.attachment_count = index;
	if (graphics->depth != 0U || graphics->stencil != 0U) {
		pass.format[index] = graphics->stencil != 0U ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
		pass.samples[index] = graphics->samples != 0U ? graphics->samples : 1U;
		pass.depth[0] = index;
		pass.attachment_count++;
	}
	(void)make_render_pass(node, ID_PIPELINE_PASS, &pass);

	/* [device][cache][u32 1][u64 1] and the head with the stages. */
	begin(node, GPU_OP_CREATE_GRAPHICS_PIPELINES);
	put64(ID_DEVICE);
	put64(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(graphics->fragment != 0U ? 2U : 1U);
	put64(graphics->fragment != 0U ? 2U : 1U);
	put_stage(VK_SHADER_STAGE_VERTEX_BIT, graphics->vertex, NULL, 0U);
	if (graphics->fragment != 0U)
		put_stage(VK_SHADER_STAGE_FRAGMENT_BIT, graphics->fragment, NULL, 0U);

	/* The vertex input: the bindings and the attributes. */
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(graphics->binding_count);
	put64(graphics->binding_count);
	for (index = 0U; index < graphics->binding_count; index++) {
		put32(index);
		put32(graphics->strides[index]);
		put32(VK_VERTEX_INPUT_RATE_VERTEX);
	}
	put32(graphics->attribute_count + (graphics->skew == SKEW_ATTRIBUTES ? 1U : 0U));
	put64(graphics->attribute_count);
	for (index = 0U; index < graphics->attribute_count; index++) {
		put32(graphics->attributes[index].location);
		put32(graphics->attributes[index].binding);
		put32(graphics->attributes[index].format);
		put32(graphics->attributes[index].offset);
	}

	/* The input assembly, no tessellation, a viewport of 64 x 32 and its scissor. */
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(graphics->topology);
	put32(0U);
	put64(0U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(0U);
	put32(0x42800000U);
	put32(0x42000000U);
	put32(0U);
	put32(0x3f800000U);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(0U);
	put32(64U);
	put32(32U);

	/* The rasterization: depth bias 1.0 constant, 0.5 clamp, 2.0 slope, line width 1.0. */
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	put32(0U);
	put32(graphics->polygon);
	put32(graphics->cull);
	put32(graphics->front_cw != 0U ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
	put32(0U);
	put32(0x3f800000U);
	put32(0x3f000000U);
	put32(0x40000000U);
	put32(0x3f800000U);

	/* The multisample state, with a sample mask when one is given. */
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(graphics->samples);
	put32(0U);
	put32(0U);
	if (graphics->sample_mask != 0U) {
		put64(1U);
		put32(graphics->sample_mask);
	} else {
		put64(0U);
	}
	put32(graphics->alpha_to_coverage);
	put32(0U);

	/* The depth and stencil state when the scene has one. */
	if (graphics->depth != 0U) {
		put64(1U);
		put32(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put32(graphics->depth_test);
		put32(graphics->depth_write);
		put32(graphics->depth_op);
		put32(0U);
		put32(graphics->stencil);
		for (index = 0U; index < 7U; index++)
			put32(graphics->front[index]);
		for (index = 0U; index < 7U; index++)
			put32(graphics->back[index]);
		put32(0U);
		put32(0x3f800000U);
	} else {
		put64(0U);
	}

	/* The colour blend state: the logic op, the attachments, constants 0.25, 0.5, 0.75, 1.0. */
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(graphics->logic);
	put32(graphics->logic_op);
	put32(graphics->attachments + (graphics->skew == SKEW_BLEND ? 1U : 0U));
	put64(graphics->attachments);
	for (index = 0U; index < graphics->attachments; index++) {
		for (word = 0U; word < 8U; word++)
			put32(graphics->blend[index][word]);
	}
	put64(4U);
	put32(0x3e800000U);
	put32(0x3f000000U);
	put32(0x3f400000U);
	put32(0x3f800000U);

	/* The dynamic states. */
	if (graphics->dynamic_count != 0U) {
		put64(1U);
		put32(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put32(graphics->dynamic_count + (graphics->skew == SKEW_DYNAMIC ? 1U : 0U));
		put64(graphics->dynamic_count);
		for (index = 0U; index < graphics->dynamic_count; index++)
			put32(graphics->dynamic[index]);
	} else {
		put64(0U);
	}

	/* The layout, the render pass, subpass 0, no base; then the identity. */
	put64(graphics->layout);
	put64(ID_PIPELINE_PASS);
	put32(0U);
	put64(0U);
	put32(0xffffffffU);
	put64(0U);
	put64(1U);
	put64(identity);
	error = run(node);
	if (error != 0)
		return ~0U;
	return reply32(node, 4U);
}

/* vkCreateComputePipelines of one pipeline with a specialization.  Returns the result, or ~0. */
static uint32_t
make_compute(
	struct node *node,
	uint64_t module,
	uint64_t layout,
	const uint32_t *spec,
	uint32_t spec_count,
	uint64_t identity)
{
	int error;

	/* [device][cache][u32 1][u64 1] and VkComputePipelineCreateInfo; then the identity. */
	begin(node, GPU_OP_CREATE_COMPUTE_PIPELINES);
	put64(ID_DEVICE);
	put64(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put_stage(VK_SHADER_STAGE_COMPUTE_BIT, module, spec, spec_count);
	put64(layout);
	put64(0U);
	put32(0xffffffffU);
	put64(0U);
	put64(1U);
	put64(identity);
	error = run(node);
	if (error != 0)
		return ~0U;
	return reply32(node, 4U);
}

/* Finds a pipeline's record. */
static const struct ghci_vk_pipeline *
pipeline_of(
	const struct node *node,
	uint64_t identity)
{
	struct ghci_vk_object *object;

	object = ghci_vk_object_find(session_of(node), GHCI_VK_PIPELINE, identity);
	if (object == NULL)
		return NULL;
	return object->as.pipeline;
}

/* Reads a dword of a pipeline's record. */
static uint32_t
pso_word(
	const struct ghci_vk_pipeline *pipeline,
	uint32_t dword)
{
	uint32_t value;

	memcpy(&value, &pipeline->pso[4U * dword], 4U);
	return value;
}

/* Stores a little-endian value of 1, 2, 4 or 8 bytes. */
static void
store_le(
	uint8_t *bytes,
	uint32_t offset,
	uint64_t value,
	uint32_t size)
{
	uint32_t index;

	for (index = 0U; index < size; index++)
		bytes[offset + index] = (uint8_t)(value >> (8U * index));
}

/*
 * The blend word (dw11) by hand: [0] enable, [5:1] src colour, [10:6]
 * dst colour, [13:11] colour op, [18:14] src alpha, [23:19] dst alpha,
 * [26:24] alpha op, [30:27] write mask (ghci.md section 8.2).
 */
static uint32_t
blend_word_by_hand(
	uint32_t enable,
	uint32_t src_color,
	uint32_t dst_color,
	uint32_t color_op,
	uint32_t src_alpha,
	uint32_t dst_alpha,
	uint32_t alpha_op,
	uint32_t mask)
{
	return enable | (src_color << 1) | (dst_color << 6) | (color_op << 11) | (src_alpha << 14) | (dst_alpha << 19) |
	    (alpha_op << 24) | (mask << 27);
}

/*
 * The triangle: the whole 128-byte record by hand from section 8.2 (the
 * code addresses and the register counts taken from the pipeline, the
 * counts checked against what the hardware writes before a warp), the
 * record and the code in the executor heap, the heap mapped executable,
 * the state the record does not keep, and a pipeline without a fragment
 * stage with the driver's three-instruction fragment program.
 */
static void
scene_triangle(void)
{
	static const uint32_t no_types[1] = { 0U };
	const struct model_command *command;
	const struct ghci_vk_pipeline *pipeline;
	struct test_graphics graphics;
	struct ghci_vk_heap_block *block;
	uint8_t expected[GHCI_PSO_BYTES];
	uint32_t code[6];
	uint64_t set_layouts[1];
	struct node node;
	uint32_t regs_vs;
	uint32_t regs_fs;
	uint32_t result;
	int error;

	/* A device and an empty pipeline layout. */
	error = open_device(&node, MODEL_FULL);
	expect(error == 0, "triangle: device and modules");
	result = make_set_layout(&node, ID_SET_LAYOUT, 0U, no_types, no_types, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "triangle: layouts");

	/* The pipeline: made, with its identity in the reply. */
	graphics_defaults(&graphics);
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && reply64(&node, 8U) == 1U && reply64(&node, 16U) == ID_PIPELINE && pipeline != NULL, "triangle: made");
	if (pipeline == NULL) {
		node_close(&node);
		stop("triangle");
		return;
	}

	/*
	 * The record by hand: dw0-3 the vertex and fragment code, dw8 the
	 * triangle list (3) with all four samples of the mask ([15:12]),
	 * dw9 back culling (2), counter-clockwise, fill, no bias, line width
	 * 16 << 6, dw10 0, dw11 no blend with the RGBA mask << 27, dw12
	 * binding 0's stride 20, dw13 the registers with one varying (a vec3
	 * is one vec4) << 24, dw16 R32G32_SFLOAT (103) at offset 0 of
	 * binding 0, dw17 R32G32B32_SFLOAT (106) at offset 8 (<< 13).
	 */
	regs_vs = pso_word(pipeline, 13U) & 0xffU;
	regs_fs = (pso_word(pipeline, 13U) >> 8) & 0xffU;
	memset(expected, 0, sizeof(expected));
	store_le(expected, 0x00U, pipeline->code_va[0], 8U);
	store_le(expected, 0x08U, pipeline->code_va[1], 8U);
	store_le(expected, 0x20U, 0x3U | (0xfU << 12), 4U);
	store_le(expected, 0x24U, 0x2U | (16U << 6), 4U);
	store_le(expected, 0x2cU, 0xfU << 27, 4U);
	store_le(expected, 0x30U, 20U, 4U);
	store_le(expected, 0x34U, regs_vs | (regs_fs << 8) | (1U << 24), 4U);
	store_le(expected, 0x40U, 103U, 4U);
	store_le(expected, 0x44U, 106U | (8U << 13), 4U);
	expect(memcmp(pipeline->pso, expected, GHCI_PSO_BYTES) == 0, "triangle: the 128 bytes of the record");
	expect(regs_vs >= 5U && regs_vs <= 63U && regs_fs >= 7U && regs_fs <= 63U, "triangle: registers cover the attributes and the fragment inputs");

	/* In the heap: the record at its address (128-byte aligned), the code after it, mapped executable. */
	block = pipeline->block;
	expect((pipeline->pso_va & 127U) == 0U && memcmp(block->allocation->view + pipeline->offset, pipeline->pso, GHCI_PSO_BYTES) == 0,
	       "triangle: the record in the heap");
	expect(pipeline->code_va[0] == pipeline->pso_va + 128U && (pipeline->code_va[1] & 63U) == 0U && pipeline->code_va[1] > pipeline->code_va[0],
	       "triangle: the code after the record");
	expect(pipeline->pso_va < (1ULL << 32), "triangle: below 4 GiB");
	command = model_last_command(GHCI_OP_VM_MAP);
	expect(command != NULL && model_lookup(command->words[1], pipeline->code_va[0]) != NULL &&
	       (model_lookup(command->words[1], pipeline->code_va[0])->flags & GHCI_VM_FLAG_EXEC) != 0U,
	       "triangle: the heap is mapped executable");

	/* What the record does not keep: viewport 64 x 32, scissor, depth bias, blend constants, strides, no tables. */
	expect(pipeline->state.viewport[2] == 0x42800000U && pipeline->state.viewport[3] == 0x42000000U && pipeline->state.viewport[5] == 0x3f800000U &&
	       pipeline->state.scissor_extent[0] == 64U && pipeline->state.scissor_extent[1] == 32U,
	       "triangle: viewport and scissor kept");
	expect(pipeline->state.depth_bias[0] == 0x3f800000U && pipeline->state.depth_bias[1] == 0x40000000U && pipeline->state.depth_bias[2] == 0x3f000000U,
	       "triangle: depth bias kept as constant, slope, clamp");
	expect(pipeline->state.blend_constants[0] == 0x3e800000U && pipeline->state.blend_constants[3] == 0x3f800000U && pipeline->state.line_width == 0x3f800000U,
	       "triangle: blend constants and line width kept");
	expect(pipeline->vertex_bindings == 1U && pipeline->strides[0] == 20U && pipeline->fan == 0U && pipeline->dynamic_count == 0U &&
	       pipeline->bindings.texture_count == 0U && pipeline->bindings.resource_count == 0U && pipeline->table_va == 0U,
	       "triangle: strides, no tables, no driver table");

	/* A fan is drawn as a triangle list (dw8 3) and kept as a fan. */
	graphics.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 1U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 1U);
	expect(result == 0U && pipeline != NULL && (pso_word(pipeline, 8U) & 7U) == 3U && pipeline->fan == 1U, "triangle: a fan as a list");

	/* Points with a front face clockwise: dw8 0, dw9 [2]. */
	graphics.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	graphics.front_cw = 1U;
	graphics.cull = VK_CULL_MODE_NONE;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 2U);
	expect(result == 0U && pipeline != NULL && (pso_word(pipeline, 8U) & 7U) == 0U && pso_word(pipeline, 9U) == ((1U << 2) | (16U << 6)),
	       "triangle: points, clockwise, no culling");

	/*
	 * Without a fragment stage: the driver's program of three
	 * instructions (addi r0 = -1 of OP-IMM 0x13, exp of CUSTOM1 0x2B to
	 * 0x105, end of CUSTOM0 0x0B) with the seven registers of the
	 * fragment inputs.
	 */
	graphics_defaults(&graphics);
	graphics.fragment = 0U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 3U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 3U);
	expect(result == 0U && pipeline != NULL, "triangle: no fragment stage");
	if (pipeline != NULL) {
		memcpy(code, pipeline->block->allocation->view + pipeline->offset + (uint32_t)(pipeline->code_va[1] - pipeline->pso_va), sizeof(code));
		expect((code[0] & 0x7fU) == 0x13U && (code[2] & 0x7fU) == 0x2bU && (code[4] & 0x7fU) == 0x0bU, "triangle: the empty fragment program");
		expect(((pso_word(pipeline, 13U) >> 8) & 0xffU) == 7U && pso_word(pipeline, 2U) != 0U, "triangle: its registers and address");
	}

	/* The destroy gives the run back: the same identity is made again. */
	begin(&node, GPU_OP_DESTROY_PIPELINE);
	put64(ID_DEVICE);
	put64(ID_PIPELINE + 3U);
	put64(0U);
	error = run(&node);
	expect(error == 0 && pipeline_of(&node, ID_PIPELINE + 3U) == NULL, "triangle: destroyed");

	/* The session closes with its pipelines. */
	node_close(&node);
	stop("triangle");
}

/*
 * Blending: alpha blending of one attachment (dw11 by hand), a logic op
 * (dw8 [5] and [9:6]), and two attachments blended differently on a
 * device with independent blending (each one's word kept for
 * SET_DYNAMIC_STATE 0x07 and 0x08).
 */
static void
scene_blend(void)
{
	static const uint32_t no_types[1] = { 0U };
	const struct ghci_vk_pipeline *pipeline;
	struct test_graphics graphics;
	uint64_t set_layouts[1];
	struct node node;
	uint32_t result;
	uint32_t word;
	int error;

	/* A device with LOGIC_OP and MRT_BLEND (the full profile). */
	error = open_device(&node, MODEL_FULL);
	device.caps.feature |= GHCI_FEATURE_LOGIC_OP | GHCI_FEATURE_MRT_BLEND;
	result = make_set_layout(&node, ID_SET_LAYOUT, 0U, no_types, no_types, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "blend: device");

	/* SRC_ALPHA (6), ONE_MINUS_SRC_ALPHA (7), ADD; alpha ONE (1), ZERO; RGBA. */
	graphics_defaults(&graphics);
	graphics.blend[0][0] = 1U;
	graphics.blend[0][1] = VK_BLEND_FACTOR_SRC_ALPHA;
	graphics.blend[0][2] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	graphics.blend[0][3] = VK_BLEND_OP_ADD;
	graphics.blend[0][4] = VK_BLEND_FACTOR_ONE;
	graphics.blend[0][5] = VK_BLEND_FACTOR_ZERO;
	graphics.blend[0][6] = VK_BLEND_OP_ADD;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	word = blend_word_by_hand(1U, 6U, 7U, 0U, 1U, 0U, 0U, 0xfU);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 11U) == word && word == 0x780041cdU, "blend: alpha blending dw11");

	/* A logic op XOR (6): dw8 [5] and 6 << 6. */
	graphics.logic = 1U;
	graphics.logic_op = VK_LOGIC_OP_XOR;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 1U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 1U);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 8U) == (3U | (1U << 5) | (6U << 6) | (0xfU << 12)), "blend: logic op dw8");

	/* Two attachments blended differently: dw11 is attachment 0's, both words kept. */
	graphics.logic = 0U;
	graphics.fragment = ID_MODULE + MODULE_MRT_FRAG;
	graphics.attachments = 2U;
	graphics.blend[1][0] = 1U;
	graphics.blend[1][1] = VK_BLEND_FACTOR_ONE;
	graphics.blend[1][2] = VK_BLEND_FACTOR_ONE;
	graphics.blend[1][3] = VK_BLEND_OP_MAX;
	graphics.blend[1][4] = VK_BLEND_FACTOR_ONE;
	graphics.blend[1][5] = VK_BLEND_FACTOR_ONE;
	graphics.blend[1][6] = VK_BLEND_OP_MIN;
	graphics.blend[1][7] = 0x3U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 2U);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 11U) == 0x780041cdU, "blend: MRT dw11 is attachment 0's");
	word = blend_word_by_hand(1U, 1U, 1U, 4U, 1U, 1U, 3U, 0x3U);
	expect(pipeline != NULL && pipeline->state.blend_count == 2U && pipeline->state.blend_attachment[0] == 0x780041cdU &&
	       pipeline->state.blend_attachment[1] == word, "blend: each attachment's word for 0x07 and 0x08");

	node_close(&node);
	stop("blend");
}

/*
 * Depth and stencil: a depth test LESS with writes (dw10 by hand), a
 * stencil test with different faces (separate, [30]), and the masks and
 * references kept as SET_DYNAMIC_STATE 0x03's words, unless dynamic.
 */
static void
scene_depth_stencil(void)
{
	static const uint32_t no_types[1] = { 0U };
	const struct ghci_vk_pipeline *pipeline;
	struct test_graphics graphics;
	uint64_t set_layouts[1];
	struct node node;
	uint32_t result;
	uint32_t word;
	int error;

	/* A device. */
	error = open_device(&node, MODEL_FULL);
	result = make_set_layout(&node, ID_SET_LAYOUT, 0U, no_types, no_types, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "depth: device");

	/* Depth LESS (1) with writes: dw10 = 1 | 2 | 1 << 2. */
	graphics_defaults(&graphics);
	graphics.depth = 1U;
	graphics.depth_test = 1U;
	graphics.depth_write = 1U;
	graphics.depth_op = VK_COMPARE_OP_LESS;
	graphics.front[3] = VK_COMPARE_OP_ALWAYS;
	graphics.back[3] = VK_COMPARE_OP_ALWAYS;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 10U) == (0x3U | (1U << 2) | (7U << 15) | (7U << 27)), "depth: dw10");

	/*
	 * Stencil: front KEEP (0), REPLACE (2), ZERO (1), ALWAYS (7), masks
	 * 0xf0, 0x0f, reference 0x55; back INVERT (5), INCR_WRAP (6),
	 * DECR_CLAMP (4), EQUAL (2), masks 0xff, 0x80, reference 0x01.
	 */
	graphics.depth_test = 0U;
	graphics.depth_write = 0U;
	graphics.depth_op = 0U;
	graphics.stencil = 1U;
	graphics.front[0] = VK_STENCIL_OP_KEEP;
	graphics.front[1] = VK_STENCIL_OP_REPLACE;
	graphics.front[2] = VK_STENCIL_OP_ZERO;
	graphics.front[3] = VK_COMPARE_OP_ALWAYS;
	graphics.front[4] = 0xf0U;
	graphics.front[5] = 0x0fU;
	graphics.front[6] = 0x55U;
	graphics.back[0] = VK_STENCIL_OP_INVERT;
	graphics.back[1] = VK_STENCIL_OP_INCREMENT_AND_WRAP;
	graphics.back[2] = VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	graphics.back[3] = VK_COMPARE_OP_EQUAL;
	graphics.back[4] = 0xffU;
	graphics.back[5] = 0x80U;
	graphics.back[6] = 0x01U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 1U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 1U);
	word = (1U << 5) | (0U << 6) | (2U << 9) | (1U << 12) | (7U << 15) | (5U << 18) | (6U << 21) | (4U << 24) | (2U << 27) | (1U << 30);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 10U) == word, "stencil: dw10 with separate faces");
	expect(pipeline != NULL && pipeline->state.stencil[0] == (0xf0U | (0x0fU << 8) | (0x55U << 16)) &&
	       pipeline->state.stencil[1] == (0xffU | (0x80U << 8) | (0x01U << 16)), "stencil: masks and references kept for 0x03");

	/* The same faces: no separate bit; the reference dynamic is marked so. */
	memcpy(graphics.back, graphics.front, sizeof(graphics.back));
	graphics.dynamic_count = 1U;
	graphics.dynamic[0] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 2U);
	expect(result == 0U && pipeline != NULL && (pso_word(pipeline, 10U) & (1U << 30)) == 0U &&
	       pipeline->state.dynamic == (1U << VK_DYNAMIC_STATE_STENCIL_REFERENCE), "stencil: one face, dynamic reference");

	node_close(&node);
	stop("depth");
}

/*
 * Multisampling: four samples with a sample mask 0x5 and alpha to
 * coverage (dw8 [10], [15:12]) on a device with CAP_MSAA; the same
 * pipeline refused without it; texsz.s refused without it.
 */
static void
scene_msaa(void)
{
	static const uint32_t types[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
	static const uint32_t stages[1] = { VK_SHADER_STAGE_FRAGMENT_BIT };
	const struct ghci_vk_pipeline *pipeline;
	struct test_graphics graphics;
	uint64_t set_layouts[1];
	struct node node;
	uint32_t result;
	int error;

	/* A device with multisampling. */
	error = open_device(&node, MODEL_FULL);
	device.caps.device_caps |= GHCI_CAP_MSAA;
	result = make_set_layout(&node, ID_SET_LAYOUT, 1U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "msaa: device");

	/* Four samples, mask 0x5, alpha to coverage. */
	graphics_defaults(&graphics);
	graphics.samples = VK_SAMPLE_COUNT_4_BIT;
	graphics.sample_mask = 0x5U;
	graphics.alpha_to_coverage = 1U;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 8U) == (3U | (1U << 10) | (0x5U << 12)) && pipeline->samples == 4U,
	       "msaa: dw8 with alpha to coverage and the mask");

	/* textureSamples (texsz.s) with multisampling. */
	graphics.fragment = ID_MODULE + MODULE_SAMPLES_FRAG;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 1U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 1U) != NULL, "msaa: texsz.s with MSAA");

	/* Without CAP_MSAA: four samples and texsz.s are refused. */
	device.caps.device_caps &= ~GHCI_CAP_MSAA;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && reply64(&node, 16U) == 0U && pipeline_of(&node, ID_PIPELINE + 2U) == NULL,
	       "msaa: four samples refused without MSAA");
	graphics.samples = VK_SAMPLE_COUNT_1_BIT;
	graphics.sample_mask = 0U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 3U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 3U) == NULL, "msaa: texsz.s refused without MSAA");

	node_close(&node);
	stop("msaa");
}

/*
 * Compute: a workgroup of 64 with shared memory (dw4-5 the code, dw13
 * [23:16] the registers, dw14 64 | 1 << 10 | 1 << 20, dw15 the shared
 * bytes 256), the dynamic storage buffer's offset in c40 and the push
 * constants in c0 and c33; a set not written and a binding of another
 * type fill no entry; a workgroup of 512 refused; shared memory past
 * LDS_KB refused; specialized pipelines (the workgroup's width, a folded
 * product and shared memory sized by an expression), and an operation
 * the driver does not fold refused.
 */
static void
scene_compute(void)
{
	static const uint32_t types[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC };
	static const uint32_t uniform_types[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
	static const uint32_t stages[1] = { VK_SHADER_STAGE_COMPUTE_BIT };
	static const uint32_t spec[4] = { 7U, 9U, 8U, 1U };
	static const uint32_t wide_spec[6] = { 3U, 16U, 7U, 9U, 8U, 1U };
	static const uint32_t past_spec[2] = { 3U, 129U };
	static struct ghci_vk_tables tables;
	static uint8_t patched[MODULE_BYTES];
	const struct ghci_vk_pipeline *pipeline;
	uint32_t at;
	uint32_t instruction;
	uint8_t push[GSI1_ABI_PUSH_BYTES];
	uint8_t expected[GHCI_BUFFER_DESC_BYTES];
	uint64_t set_layouts[1];
	uint64_t sets[GHCI_VK_SETS];
	uint32_t dynamic[1];
	uint32_t word;
	uint32_t index;
	struct node node;
	uint64_t va;
	uint32_t result;
	int error;

	/* A device, a set layout of a dynamic storage buffer, a layout with 104 bytes of push constants. */
	error = open_device(&node, MODEL_FULL);
	result = make_set_layout(&node, ID_SET_LAYOUT, 1U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 104U) == 0U, "compute: device");

	/* The pipeline's record by hand. */
	result = make_compute(&node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && pipeline != NULL, "compute: made");
	if (pipeline == NULL) {
		node_close(&node);
		stop("compute");
		return;
	}
	word = (pso_word(pipeline, 13U) >> 16) & 0xffU;
	expect(pso_word(pipeline, 0U) == 0U && pso_word(pipeline, 2U) == 0U && pso_word(pipeline, 4U) == (uint32_t)pipeline->code_va[0] &&
	       pso_word(pipeline, 5U) == (uint32_t)(pipeline->code_va[0] >> 32) && pipeline->code_va[0] == pipeline->pso_va + 128U,
	       "compute: dw4-5 the code, no graphics stage");
	expect(word >= 1U && word <= 63U && (pso_word(pipeline, 13U) & 0xff00ffffU) == 0U, "compute: dw13 used_regs_cs alone");
	expect(pso_word(pipeline, 14U) == (64U | (1U << 10) | (1U << 20)) && pso_word(pipeline, 15U) == 256U && pso_word(pipeline, 8U) == 0U,
	       "compute: dw14 workgroup, dw15 shared memory");
	expect(pipeline->dynamic_count == 1U && pipeline->push_bytes == 104U && pipeline->bindings.resource_count == 1U &&
	       pipeline->bindings.resource[0].kind == GHCI_GSI1_RESOURCE_STORAGE_BUFFER && pipeline->bindings.resource[0].set == 0U,
	       "compute: the resource table names the dynamic buffer");

	/* A pool and a set; the buffer written at offset 0 with its whole range. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put32(1U);
	put64(1U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put32(1U);
	tail(ID_POOL);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "compute: pool");
	begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL);
	put32(1U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(1U);
	put64(ID_SET);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U && reply64(&node, 8U) == 1U && reply64(&node, 16U) == ID_SET, "compute: set");
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put64(0U);
	put64(1U);
	put64(ID_BUFFER);
	put64(0U);
	put64(VK_WHOLE_SIZE);
	put64(0U);
	put32(0U);
	put64(0U);
	error = run(&node);
	expect(error == 0, "compute: written");

	/* The tables: set 2's entry is the buffer's descriptor (8192 bytes, read-write), c40 the offset, c0 and c33 the push words. */
	memset(sets, 0, sizeof(sets));
	sets[0] = ID_SET;
	memset(push, 0, sizeof(push));
	store_le(push, 0U, 0x1234U, 4U);
	store_le(push, 100U, 0xabcdU, 4U);
	dynamic[0] = 256U;
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 1U, push, 104U, &tables);
	(void)ghci_vk_bound_va(session_of(&node), ID_MEMORY, 0x80000U, &va);
	memset(expected, 0, sizeof(expected));
	store_le(expected, 0x00U, va, 8U);
	store_le(expected, 0x08U, 8192U, 4U);
	expect(error == 0 && memcmp(&tables.resources[pipeline->bindings.resource[0].offset], expected, sizeof(expected)) == 0,
	       "compute: set 2's buffer descriptor by hand");
	expect(tables.bank[40] == 256U && tables.bank[0] == 0x1234U && tables.bank[33] == 0xabcdU && tables.bank[24] == 0U &&
	       tables.bank_count == 41U, "compute: c40 dynamic offset, c0 and c33 push constants, c24 untouched");
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 0U, push, 104U, &tables);
	expect(error == EINVAL, "compute: the dynamic offsets must be the layout's");
	sets[0] = 0U;
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 1U, push, 104U, &tables);
	expect(error == ENOENT, "compute: an entry of a set not bound");

	/* Six push-constant bytes: the second word has two of them and zeros, nothing read past them. */
	sets[0] = ID_SET;
	store_le(push, 4U, 0xffffabcdU, 4U);
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 1U, push, 6U, &tables);
	expect(error == 0 && tables.bank[0] == 0x1234U && tables.bank[1] == 0xabcdU, "compute: a last partial push-constant word");

	/* A set of the layout not written, and a uniform buffer's binding where the program reads a storage buffer. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(2U);
	put32(2U);
	put64(2U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(1U);
	tail(ID_POOL + 1U);
	error = run(&node);
	result = make_set_layout(&node, ID_SET_LAYOUT + 1U, 1U, uniform_types, stages, 0U);
	for (index = 0U; index < 2U; index++) {
		begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
		put64(ID_DEVICE);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
		put64(0U);
		put64(ID_POOL + 1U);
		put32(1U);
		put64(1U);
		put64(ID_SET_LAYOUT + index);
		put64(1U);
		put64(ID_SET + 1U + index);
		error |= run(&node);
	}
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET + 2U);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put64(0U);
	put64(1U);
	put64(ID_BUFFER);
	put64(0U);
	put64(VK_WHOLE_SIZE);
	put64(0U);
	put32(0U);
	put64(0U);
	error |= run(&node);
	expect(error == 0 && result == 0U, "compute: a second set layout and two more sets");
	sets[0] = ID_SET + 1U;
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 1U, push, 104U, &tables);
	expect(error == ENOENT, "compute: an entry whose descriptor is not written");
	sets[0] = ID_SET + 2U;
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, dynamic, 1U, push, 104U, &tables);
	expect(error == ENOENT, "compute: an entry of a binding of another type");

	/* A workgroup of 512 is refused; shared memory past LDS_KB is refused. */
	result = make_compute(&node, ID_MODULE + MODULE_WIDE_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE + 1U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 1U) == NULL, "compute: 512 invocations refused");
	device.caps.lds_bytes = 128U;
	result = make_compute(&node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE + 2U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 2U) == NULL, "compute: shared memory past LDS_KB refused");

	/*
	 * Specialized: SCALE = 9, NEGATE = true, the width by default (1 x 2
	 * x 1), the shared array of SCALE * 2 + 1 words (an OpSpecConstantOp
	 * folded twice: 76 bytes); without the map (4: 36 bytes); and a width
	 * of 16 (dw14 16 | 2 << 10 | 1 << 20).
	 */
	device.caps.lds_bytes = 16384U;
	result = make_compute(&node, ID_MODULE + MODULE_SPEC_COMP, ID_LAYOUT, spec, 2U, ID_PIPELINE + 3U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 3U);
	expect(result == 0U && pipeline != NULL, "compute: specialized");
	expect(pipeline != NULL && pso_word(pipeline, 14U) == (1U | (2U << 10) | (1U << 20)) && pso_word(pipeline, 15U) == 76U,
	       "compute: specialized, the default width and SCALE * 2 + 1 words of shared memory");
	result = make_compute(&node, ID_MODULE + MODULE_SPEC_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE + 4U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 4U);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 15U) == 36U, "compute: default specialization");
	result = make_compute(&node, ID_MODULE + MODULE_SPEC_COMP, ID_LAYOUT, wide_spec, 3U, ID_PIPELINE + 5U);
	pipeline = pipeline_of(&node, ID_PIPELINE + 5U);
	expect(result == 0U && pipeline != NULL && pso_word(pipeline, 14U) == (16U | (2U << 10) | (1U << 20)),
	       "compute: local_size_x_id specialized to 16 (dw14)");
	result = make_compute(&node, ID_MODULE + MODULE_SPEC_COMP, ID_LAYOUT, past_spec, 1U, ID_PIPELINE + 6U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 6U) == NULL,
	       "compute: a specialized width past the limits refused");

	/* The module with its first OpSpecConstantOp made an OpFAdd (129), which the driver does not fold. */
	memcpy(patched, modules[MODULE_SPEC_COMP], module_bytes[MODULE_SPEC_COMP]);
	for (at = 5U; at < module_bytes[MODULE_SPEC_COMP] / 4U; at += instruction >> 16) {
		memcpy(&instruction, patched + 4U * at, 4U);
		if ((instruction >> 16) == 0U)
			break;
		if ((instruction & 0xffffU) == 52U) {
			store_le(patched, 4U * (at + 3U), 129U, 4U);
			break;
		}
	}
	begin(&node, GPU_OP_CREATE_SHADER_MODULE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(module_bytes[MODULE_SPEC_COMP]);
	put64(module_bytes[MODULE_SPEC_COMP] / 4U);
	put_bytes(patched, module_bytes[MODULE_SPEC_COMP]);
	tail(ID_MODULE + 0x80U);
	error = run(&node);
	result = make_compute(&node, ID_MODULE + 0x80U, ID_LAYOUT, NULL, 0U, ID_PIPELINE + 7U);
	expect(error == 0 && result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 7U) == NULL,
	       "compute: an OpSpecConstantOp the driver does not fold refused");

	node_close(&node);
	stop("compute");
}

/*
 * Descriptors: a sampler's 16 bytes, an image view's 32 bytes (a level
 * and layer within the image, a swizzle), a buffer view's descriptors, a
 * set of a uniform buffer, a combined image sampler (an immutable
 * sampler) and a uniform texel buffer written, and the hardware tables
 * of a draw with tex.frag: set 0 and 1 by slot, set 2 by entry, all by
 * hand from section 8.3.  Then the pool's room, free, copy and reset.
 */
static void
scene_descriptors(void)
{
	static const uint32_t types[3] = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
	};
	static const uint32_t stages[3] = { VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
	static struct ghci_vk_tables tables;
	static const uint8_t sampler_bytes[16] = { 1U, 1U, 1U, 0U, 2U, 3U, 0U, 0U, 0x80U, 0x01U, 8U, 255U, 1U, 2U, 0U, 0U };
	const struct ghci_vk_pipeline *pipeline;
	struct ghci_vk_object *object;
	struct test_graphics graphics;
	uint8_t image_bytes[32];
	uint8_t texel_bytes[32];
	uint8_t texel_buffer[16];
	uint8_t uniform[16];
	uint64_t set_layouts[1];
	uint64_t sets[GHCI_VK_SETS];
	uint64_t image_va;
	uint64_t buffer_va;
	uint32_t index;
	uint32_t found;
	struct node node;
	uint32_t result;
	int error;

	/* A device. */
	error = open_device(&node, MODEL_FULL);
	expect(error == 0, "descriptors: device");
	(void)ghci_vk_bound_va(session_of(&node), ID_MEMORY, 0U, &image_va);
	(void)ghci_vk_bound_va(session_of(&node), ID_MEMORY, 0x80000U, &buffer_va);

	/*
	 * A sampler: LINEAR, LINEAR, mip LINEAR (1, 1, 1), REPEAT,
	 * CLAMP_TO_EDGE, CLAMP_TO_BORDER (0, 2, 3), no compare, bias 1.5
	 * (8.8: 0x0180), min LOD 0.5 (4.4: 8), max LOD 1000 (255),
	 * anisotropy 1, INT_OPAQUE_WHITE (opaque white, 2), normalized.
	 */
	begin(&node, GPU_OP_CREATE_SAMPLER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_FILTER_LINEAR);
	put32(VK_FILTER_LINEAR);
	put32(VK_SAMPLER_MIPMAP_MODE_LINEAR);
	put32(VK_SAMPLER_ADDRESS_MODE_REPEAT);
	put32(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	put32(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
	put32(0x3fc00000U);
	put32(0U);
	put32(0x3f800000U);
	put32(0U);
	put32(VK_COMPARE_OP_NEVER);
	put32(0x3f000000U);
	put32(0x447a0000U);
	put32(VK_BORDER_COLOR_INT_OPAQUE_WHITE);
	put32(0U);
	tail(ID_SAMPLER);
	error = run(&node);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_SAMPLER, ID_SAMPLER);
	expect(error == 0 && reply32(&node, 4U) == 0U && object != NULL && memcmp(object->as.sampler.descriptor, sampler_bytes, 16U) == 0,
	       "descriptors: the sampler's 16 bytes");

	/* The same sampler on a texture unit without the bilinear filter: NEAREST, NEAREST, mip NEAREST. */
	session_of(&node)->device->caps.fmt0 &= ~GHCI_FMT0_TU_BILINEAR;
	begin(&node, GPU_OP_CREATE_SAMPLER);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(VK_FILTER_LINEAR);
	put32(VK_FILTER_LINEAR);
	put32(VK_SAMPLER_MIPMAP_MODE_LINEAR);
	put32(VK_SAMPLER_ADDRESS_MODE_REPEAT);
	put32(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	put32(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
	put32(0x3fc00000U);
	put32(0U);
	put32(0x3f800000U);
	put32(0U);
	put32(VK_COMPARE_OP_NEVER);
	put32(0x3f000000U);
	put32(0x447a0000U);
	put32(VK_BORDER_COLOR_INT_OPAQUE_WHITE);
	put32(0U);
	tail(ID_SAMPLER + 1U);
	error = run(&node);
	session_of(&node)->device->caps.fmt0 |= GHCI_FMT0_TU_BILINEAR;
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_SAMPLER, ID_SAMPLER + 1U);
	expect(error == 0 && reply32(&node, 4U) == 0U && object != NULL && object->as.sampler.descriptor[0] == 0U &&
	       object->as.sampler.descriptor[1] == 0U && object->as.sampler.descriptor[2] == 0U &&
	       memcmp(object->as.sampler.descriptor + 3U, sampler_bytes + 3U, 13U) == 0,
	       "descriptors: without the bilinear filter the sampler is the nearest one");

	/*
	 * An image view: 2D array of layer 1 alone, levels 1 and 2, swizzle
	 * B, G, R, ONE (4, 3, 2, 1: 0x29c).  Level 1 is 32 x 16 (rows of
	 * 128 bytes, 2048 a level), a layer holds levels of 8192 + 2048 +
	 * 512 = 10752 bytes, so the view starts at 10752 + 8192.
	 */
	begin(&node, GPU_OP_CREATE_IMAGE_VIEW);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_IMAGE);
	put32(VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	put32(VK_FORMAT_R8G8B8A8_UNORM);
	put32(VK_COMPONENT_SWIZZLE_B);
	put32(VK_COMPONENT_SWIZZLE_IDENTITY);
	put32(VK_COMPONENT_SWIZZLE_R);
	put32(VK_COMPONENT_SWIZZLE_ONE);
	put32(VK_IMAGE_ASPECT_COLOR_BIT);
	put32(1U);
	put32(2U);
	put32(1U);
	put32(1U);
	tail(ID_IMAGE_VIEW);
	error = run(&node);
	memset(image_bytes, 0, sizeof(image_bytes));
	store_le(image_bytes, 0x00U, image_va + 10752U + 8192U, 8U);
	store_le(image_bytes, 0x08U, 32U, 2U);
	store_le(image_bytes, 0x0aU, 16U, 2U);
	store_le(image_bytes, 0x0cU, 1U, 2U);
	store_le(image_bytes, 0x0eU, 37U, 1U);
	store_le(image_bytes, 0x0fU, 2U, 1U);
	store_le(image_bytes, 0x10U, 128U, 4U);
	store_le(image_bytes, 0x14U, 10752U, 4U);
	store_le(image_bytes, 0x18U, 1U, 1U);
	store_le(image_bytes, 0x19U, 1U, 1U);
	store_le(image_bytes, 0x1cU, 0x29cU, 4U);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_IMAGE_VIEW, ID_IMAGE_VIEW);
	expect(error == 0 && reply32(&node, 4U) == 0U && object != NULL && memcmp(object->as.image_view.sampled, image_bytes, 32U) == 0,
	       "descriptors: the image view's 32 bytes");
	store_le(image_bytes, 0x0fU, 1U, 1U);
	store_le(image_bytes, 0x1cU, 2U | (3U << 3) | (4U << 6) | (5U << 9), 4U);
	expect(object != NULL && memcmp(object->as.image_view.single, image_bytes, 32U) == 0 && object->as.image_view.va == image_va + 18944U,
	       "descriptors: the view's single level for storage and attachments");

	/*
	 * A buffer view of R32_SFLOAT (100) at offset 256, 4000 bytes: 1000
	 * texels, 4 bytes apart; as an image 256 wide, 4 rows of 1024 bytes.
	 */
	begin(&node, GPU_OP_CREATE_BUFFER_VIEW);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(ID_BUFFER);
	put32(VK_FORMAT_R32_SFLOAT);
	put64(256U);
	put64(4000U);
	tail(ID_BUFFER_VIEW);
	error = run(&node);
	memset(texel_buffer, 0, sizeof(texel_buffer));
	store_le(texel_buffer, 0x00U, buffer_va + 256U, 8U);
	store_le(texel_buffer, 0x08U, 4000U, 4U);
	store_le(texel_buffer, 0x0cU, 4U, 2U);
	store_le(texel_buffer, 0x0eU, 100U, 1U);
	memset(texel_bytes, 0, sizeof(texel_bytes));
	store_le(texel_bytes, 0x00U, buffer_va + 256U, 8U);
	store_le(texel_bytes, 0x08U, 256U, 2U);
	store_le(texel_bytes, 0x0aU, 4U, 2U);
	store_le(texel_bytes, 0x0cU, 1U, 2U);
	store_le(texel_bytes, 0x0eU, 100U, 1U);
	store_le(texel_bytes, 0x0fU, 1U, 1U);
	store_le(texel_bytes, 0x10U, 1024U, 4U);
	store_le(texel_bytes, 0x14U, 4096U, 4U);
	store_le(texel_bytes, 0x18U, 1U, 1U);
	store_le(texel_bytes, 0x19U, 1U, 1U);
	store_le(texel_bytes, 0x1cU, 2U | (3U << 3) | (4U << 6) | (5U << 9), 4U);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_BUFFER_VIEW, ID_BUFFER_VIEW);
	expect(error == 0 && reply32(&node, 4U) == 0U && object != NULL && memcmp(object->as.buffer_view.buffer_descriptor, texel_buffer, 16U) == 0 &&
	       memcmp(object->as.buffer_view.texture, texel_bytes, 32U) == 0, "descriptors: the buffer view's 16 and 32 bytes");

	/* The set layout (an immutable sampler on binding 1), the pipeline layout, the pipeline with tex.frag. */
	result = make_set_layout(&node, ID_SET_LAYOUT, 3U, types, stages, ID_SAMPLER);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "descriptors: layouts");
	graphics_defaults(&graphics);
	graphics.fragment = ID_MODULE + MODULE_TEX_FRAG;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && pipeline != NULL, "descriptors: pipeline");
	if (pipeline == NULL) {
		node_close(&node);
		stop("descriptors");
		return;
	}

	/* The slot tables: textures of bindings 1 (an image) and 2 (a texel buffer), the sampler of binding 1. */
	found = 0U;
	for (index = 0U; index < pipeline->bindings.texture_count; index++) {
		if (pipeline->bindings.texture[index].binding == 1U && pipeline->bindings.texture[index].kind == GHCI_GSI1_TEXTURE_IMAGE)
			found |= 1U;
		if (pipeline->bindings.texture[index].binding == 2U && pipeline->bindings.texture[index].kind == GHCI_GSI1_TEXTURE_TEXEL_BUFFER)
			found |= 2U;
	}
	expect(pipeline->bindings.texture_count == 2U && found == 3U && pipeline->bindings.sampler_count == 1U &&
	       pipeline->bindings.sampler[0].binding == 1U && pipeline->bindings.sampler[0].kind == GHCI_GSI1_SAMPLER_COMBINED,
	       "descriptors: the slot tables");
	expect(pipeline->bindings.resource_count == 2U, "descriptors: the uniform buffer and the texel buffer's entries");

	/* A pool of one set, the set, and the writes. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
	put32(1U);
	put32(3U);
	put64(3U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
	put32(1U);
	tail(ID_POOL);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "descriptors: pool");
	for (index = 0U; index < 2U; index++) {
		begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
		put64(ID_DEVICE);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
		put64(0U);
		put64(ID_POOL);
		put32(1U);
		put64(1U);
		put64(ID_SET_LAYOUT);
		put64(1U);
		put64(ID_SET + index);
		error = run(&node);
		if (index == 0U)
			expect(error == 0 && reply32(&node, 4U) == 0U, "descriptors: set");
		else
			expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_POOL_MEMORY, "descriptors: a second set past maxSets");
	}

	/* Writes: the uniform buffer (64 bytes at 512), the image (its sampler immutable), the texel buffer. */
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(3U);
	put64(3U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put64(0U);
	put64(1U);
	put64(ID_BUFFER);
	put64(512U);
	put64(64U);
	put64(0U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(1U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	put64(1U);
	put64(0U);
	put64(ID_IMAGE_VIEW);
	put32(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	put64(0U);
	put64(0U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(2U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
	put64(0U);
	put64(0U);
	put64(1U);
	put64(ID_BUFFER_VIEW);
	put32(0U);
	put64(0U);
	error = run(&node);
	expect(error == 0, "descriptors: written");

	/* The tables of a draw with the set bound at 0. */
	memset(sets, 0, sizeof(sets));
	sets[0] = ID_SET;
	error = ghci_vk_layout_tables(session_of(&node), pipeline, sets, NULL, 0U, NULL, 0U, &tables);
	expect(error == 0 && tables.texture_count == 2U && tables.sampler_count == 1U && tables.bank_count == 40U, "descriptors: tables made");
	for (index = 0U; index < pipeline->bindings.texture_count; index++) {
		if (pipeline->bindings.texture[index].binding == 1U) {
			store_le(image_bytes, 0x0fU, 2U, 1U);
			store_le(image_bytes, 0x1cU, 0x29cU, 4U);
			expect(memcmp(&tables.textures[32U * index], image_bytes, 32U) == 0, "descriptors: set 0, the image's slot");
		} else {
			expect(memcmp(&tables.textures[32U * index], texel_bytes, 32U) == 0, "descriptors: set 0, the texel buffer's slot");
		}
	}
	expect(memcmp(&tables.samplers[0], sampler_bytes, 16U) == 0, "descriptors: set 1, the immutable sampler");
	memset(uniform, 0, sizeof(uniform));
	store_le(uniform, 0x00U, buffer_va + 512U, 8U);
	store_le(uniform, 0x08U, 64U, 4U);
	store_le(uniform, 0x0fU, 1U, 1U);
	store_le(texel_buffer, 0x0fU, 1U, 1U);
	for (index = 0U; index < pipeline->bindings.resource_count; index++) {
		if (pipeline->bindings.resource[index].binding == 0U)
			expect(memcmp(&tables.resources[pipeline->bindings.resource[index].offset], uniform, 16U) == 0, "descriptors: set 2, the uniform buffer (read only)");
		else
			expect(memcmp(&tables.resources[pipeline->bindings.resource[index].offset], texel_buffer, 16U) == 0, "descriptors: set 2, the texel buffer (read only)");
	}

	/* A copy of three descriptors from binding 0 goes on into bindings 1 and 2 of a set of another pool. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put32(3U);
	put64(3U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
	put32(1U);
	tail(ID_POOL + 1U);
	error = run(&node);
	begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL + 1U);
	put32(1U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(1U);
	put64(ID_SET + 5U);
	error |= run(&node);
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(0U);
	put64(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put64(ID_SET + 5U);
	put32(0U);
	put32(0U);
	put32(3U);
	error |= run(&node);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_SET, ID_SET + 5U);
	expect(error == 0 && object != NULL && memcmp(object->as.set->descriptors[0].buffer, uniform, 16U) == 0 &&
	       memcmp(object->as.set->descriptors[1].texture, image_bytes, 32U) == 0 && memcmp(object->as.set->descriptors[2].buffer, texel_buffer, 16U) == 0,
	       "descriptors: a copy across three bindings");

	/* A write whose payload is not its type's ends the frame. */
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put64(1U);
	put64(0U);
	put64(ID_IMAGE_VIEW);
	put32(0U);
	put64(0U);
	put64(0U);
	error = run(&node);
	expect(error == EINVAL, "descriptors: an image payload for a uniform buffer");

	/* A write of two uniform buffers from binding 0 walks into binding 1 (a combined image sampler): refused. */
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put32(2U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put64(0U);
	put64(2U);
	for (index = 0U; index < 2U; index++) {
		put64(ID_BUFFER);
		put64(0U);
		put64(64U);
	}
	put64(0U);
	put32(0U);
	put64(0U);
	error = run(&node);
	expect(error == EINVAL, "descriptors: a write walking into a binding of another type");

	/* A copy from binding 0 (a uniform buffer) to binding 1 (a combined image sampler): refused. */
	begin(&node, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put32(0U);
	put64(0U);
	put32(1U);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
	put64(0U);
	put64(ID_SET);
	put32(0U);
	put32(0U);
	put64(ID_SET + 5U);
	put32(1U);
	put32(0U);
	put32(1U);
	error = run(&node);
	expect(error == EINVAL, "descriptors: a copy between bindings of other types");

	/* Free (FREE_DESCRIPTOR_SET_BIT) makes room again; the pool's reset takes the set with it. */
	begin(&node, GPU_OP_FREE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(ID_POOL);
	put32(1U);
	put64(1U);
	put64(ID_SET);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U && ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_SET, ID_SET) == NULL,
	       "descriptors: freed");
	begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL);
	put32(1U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(1U);
	put64(ID_SET + 2U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "descriptors: room again after the free");
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_SET, ID_SET + 2U);
	expect(object != NULL && memcmp(object->as.set->descriptors[1].sampler, sampler_bytes, 16U) == 0 && object->as.set->descriptors[0].written == 0U,
	       "descriptors: a new set has the immutable sampler and nothing written");

	/* The set layout destroyed while the set holds it, then the reset. */
	begin(&node, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(ID_SET_LAYOUT);
	put64(0U);
	error = run(&node);
	expect(error == 0 && object != NULL && object->as.set->layout->binding_count == 3U, "descriptors: the set keeps its layout");
	begin(&node, GPU_OP_RESET_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(ID_POOL);
	put32(0U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U && ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_SET, ID_SET + 2U) == NULL,
	       "descriptors: reset");

	node_close(&node);
	stop("descriptors");
}

/*
 * Pipelines refused on devices without the flags they need (U8): dual
 * source without CAP_DUAL_SOURCE, a logic op without FEATURE[11],
 * attachments blended differently without MRT_BLEND (the factors of one
 * that does not blend do not count), an attribute format without
 * VERTEX_BUFFER, more attachments than MAX_MRT, more than 16 texture
 * slots over the vertex and fragment stages, more than 60 attribute
 * registers; each with
 * VK_ERROR_OUT_OF_DEVICE_MEMORY and no identity, and the same pipelines
 * made with the flags.
 */
static void
scene_refusals(void)
{
	static const uint32_t types[9] = {
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
	};
	static const uint32_t stages[9] = {
		VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS,
		VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS,
		VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS, VK_SHADER_STAGE_ALL_GRAPHICS
	};
	struct test_graphics graphics;
	struct ghci_vk *vk;
	uint64_t set_layouts[3];
	struct node node;
	uint32_t result;
	uint32_t index;
	int error;

	/* A device; set layouts of 8 and 9 combined image samplers; a layout of sets 0 (empty), 1 and 2. */
	error = open_device(&node, MODEL_FULL);
	result = make_set_layout(&node, ID_SET_LAYOUT, 0U, types, stages, 0U);
	result |= make_set_layout(&node, ID_SET_LAYOUT + 1U, 8U, types, stages, 0U);
	result |= make_set_layout(&node, ID_SET_LAYOUT + 2U, 9U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	set_layouts[1] = ID_SET_LAYOUT + 1U;
	set_layouts[2] = ID_SET_LAYOUT + 2U;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 3U, set_layouts, 0U) == 0U, "refusals: device");

	/* Dual source: SRC1_COLOR (15) refused without CAP_DUAL_SOURCE, made with it. */
	graphics_defaults(&graphics);
	graphics.blend[0][0] = 1U;
	graphics.blend[0][1] = VK_BLEND_FACTOR_SRC1_COLOR;
	graphics.blend[0][2] = VK_BLEND_FACTOR_ZERO;
	device.caps.device_caps &= ~GHCI_CAP_DUAL_SOURCE;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && reply64(&node, 16U) == 0U, "refusals: dual source without DUAL_SOURCE");
	device.caps.device_caps |= GHCI_CAP_DUAL_SOURCE;
	result = make_graphics(&node, &graphics, ID_PIPELINE);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE) != NULL && (pso_word(pipeline_of(&node, ID_PIPELINE), 11U) & 0x3eU) == (15U << 1),
	       "refusals: dual source with DUAL_SOURCE");

	/* A logic op refused without FEATURE[11]. */
	graphics_defaults(&graphics);
	graphics.logic = 1U;
	graphics.logic_op = VK_LOGIC_OP_AND;
	device.caps.feature &= ~GHCI_FEATURE_LOGIC_OP;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 1U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 1U) == NULL, "refusals: logic op without LOGIC_OP");

	/* Attachments blended differently refused without MRT_BLEND; the same blend twice is made. */
	graphics_defaults(&graphics);
	graphics.fragment = ID_MODULE + MODULE_MRT_FRAG;
	graphics.attachments = 2U;
	graphics.blend[1][7] = 0x1U;
	device.caps.feature &= ~GHCI_FEATURE_MRT_BLEND;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 2U) == NULL, "refusals: independent blend without MRT_BLEND");
	graphics.blend[1][7] = 0xfU;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 2U) != NULL && pipeline_of(&node, ID_PIPELINE + 2U)->state.blend_count == 0U,
	       "refusals: the same blend twice without MRT_BLEND");

	/* Two attachments that do not blend, with factors of their own, are blended the same; blending one is not. */
	graphics.blend[1][1] = VK_BLEND_FACTOR_SRC_ALPHA;
	graphics.blend[1][2] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 7U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 7U) != NULL, "refusals: factors of attachments that do not blend do not count");
	graphics.blend[1][0] = 1U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 8U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 8U) == NULL,
	       "refusals: one attachment blending, the other not, without MRT_BLEND");


	/* More attachments than MAX_MRT (1). */
	device.caps.max_mrt = 1U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 3U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 3U) == NULL, "refusals: past MAX_MRT");
	device.caps.max_mrt = 8U;

	/* An attribute format without VERTEX_BUFFER. */
	graphics_defaults(&graphics);
	vk = device.vk;
	vk->formats.buffer[VK_FORMAT_R32G32_SFLOAT] &= ~(uint32_t)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 9U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 9U) == NULL,
	       "refusals: an attribute format without VERTEX_BUFFER");
	vertex_formats();

	/* Eight textures in the vertex stage and nine in the fragment stage: 17 slots. */
	graphics_defaults(&graphics);
	graphics.vertex = ID_MODULE + MODULE_VS8_VERT;
	graphics.fragment = ID_MODULE + MODULE_FS9_FRAG;
	graphics.attribute_count = 1U;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 4U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 4U) == NULL, "refusals: 17 texture slots over VS and FS");
	graphics.fragment = ID_MODULE + MODULE_TRI_FRAG;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 4U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 4U) != NULL && pipeline_of(&node, ID_PIPELINE + 4U)->bindings.texture_count == 8U,
	       "refusals: the vertex stage's 8 slots alone");

	/* Sixteen vec4 attributes (64 registers) past the 60. */
	graphics_defaults(&graphics);
	graphics.vertex = ID_MODULE + MODULE_ATTRIB60_VERT;
	graphics.fragment = 0U;
	graphics.attribute_count = 16U;
	graphics.strides[0] = 0U;
	for (index = 0U; index < 16U; index++) {
		graphics.attributes[index].location = index;
		graphics.attributes[index].binding = 0U;
		graphics.attributes[index].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		graphics.attributes[index].offset = 16U * index;
	}
	result = make_graphics(&node, &graphics, ID_PIPELINE + 5U);
	expect(result == (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY && pipeline_of(&node, ID_PIPELINE + 5U) == NULL, "refusals: 64 attribute registers");

	/* The first four of one component and twelve of four: 52 registers are made. */
	for (index = 0U; index < 4U; index++)
		graphics.attributes[index].format = VK_FORMAT_R32_SFLOAT;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 6U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 6U) != NULL &&
	       (pso_word(pipeline_of(&node, ID_PIPELINE + 6U), 16U) & 0xffU) == 100U && (pso_word(pipeline_of(&node, ID_PIPELINE + 6U), 31U) >> 13) == 240U,
	       "refusals: 52 attribute registers made, slots in location order");

	node_close(&node);
	stop("refusals");
}

/*
 * Pipeline caches: the data is the 32-byte header (its size, version 1,
 * the PCI IDs and the pipelineCacheUUID), VK_INCOMPLETE for a small
 * capacity, and a merge.
 */
static void
scene_caches(void)
{
	struct node node;
	uint8_t header[32];
	int error;

	/* A device and two caches. */
	error = open_device(&node, MODEL_FULL);
	begin(&node, GPU_OP_CREATE_PIPELINE_CACHE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(0U);
	put64(0U);
	tail(ID_CACHE);
	error |= run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "caches: made");
	begin(&node, GPU_OP_CREATE_PIPELINE_CACHE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(0U);
	put64(0U);
	tail(ID_CACHE + 1U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "caches: second");

	/* The size, then the data. */
	begin(&node, GPU_OP_GET_PIPELINE_CACHE_DATA);
	put64(ID_DEVICE);
	put64(ID_CACHE);
	put64(1U);
	put64(0U);
	put64(0U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U && reply64(&node, 8U) == 1U && reply64(&node, 16U) == 32U && reply64(&node, 24U) == 0U,
	       "caches: the size");
	begin(&node, GPU_OP_GET_PIPELINE_CACHE_DATA);
	put64(ID_DEVICE);
	put64(ID_CACHE);
	put64(1U);
	put64(64U);
	put64(64U);
	error = run(&node);
	memset(header, 0, sizeof(header));
	store_le(header, 0U, 32U, 4U);
	store_le(header, 4U, 1U, 4U);
	store_le(header, 8U, TEST_PCI_VENDOR, 4U);
	store_le(header, 12U, TEST_PCI_DEVICE, 4U);
	memcpy(&header[16], "zedBSD-ghci-pc10", 16U);
	expect(error == 0 && reply32(&node, 4U) == 0U && reply64(&node, 16U) == 32U && reply64(&node, 24U) == 32U && memcmp(node.reply + 32U, header, 32U) == 0,
	       "caches: the header");
	begin(&node, GPU_OP_GET_PIPELINE_CACHE_DATA);
	put64(ID_DEVICE);
	put64(ID_CACHE);
	put64(1U);
	put64(16U);
	put64(16U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_INCOMPLETE && reply64(&node, 16U) == 0U, "caches: a small capacity");

	/* A merge of the second into the first. */
	begin(&node, GPU_OP_MERGE_PIPELINE_CACHES);
	put64(ID_DEVICE);
	put64(ID_CACHE);
	put32(1U);
	put64(1U);
	put64(ID_CACHE + 1U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == 0U, "caches: merged");

	node_close(&node);
	stop("caches");
}

/*
 * Scratch memory: a compute program that spills gets the driver's table
 * of set 3 after its code (64-byte aligned), whose word at 0x10 (the
 * compute warps') is the address of RESIDENT_WARPS[15:0] warps of its
 * bytes per warp; the vertex and fragment words stay 0.
 */
static void
scene_scratch(void)
{
	static const uint32_t types[2] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
	static const uint32_t stages[2] = { VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_COMPUTE_BIT };
	const struct ghci_vk_pipeline *pipeline;
	const uint8_t *table;
	uint64_t set_layouts[1];
	uint64_t scratch;
	uint64_t word;
	struct node node;
	uint32_t result;
	int error;

	/* A device and the layout of spill.comp's two buffers. */
	error = open_device(&node, MODEL_FULL);
	result = make_set_layout(&node, ID_SET_LAYOUT, 2U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	expect(error == 0 && result == 0U && make_layout(&node, ID_LAYOUT, 1U, set_layouts, 0U) == 0U, "scratch: device");

	/* A lane of 32 registers (RESIDENT_WARPS[23:16]) makes the program spill. */
	device.caps.lane_registers = 32U;

	/* The pipeline, its table after the code, and the compute warps' address in it. */
	result = make_compute(&node, ID_MODULE + MODULE_SPILL_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE);
	pipeline = pipeline_of(&node, ID_PIPELINE);
	expect(result == 0U && pipeline != NULL && pipeline->table_va != 0U && (pipeline->table_va & 63U) == 0U &&
	       pipeline->table_va > pipeline->code_va[0] && pipeline->scratch_block != NULL, "scratch: the driver's table");
	if (pipeline != NULL && pipeline->scratch_block != NULL) {
		table = pipeline->block->allocation->view + pipeline->offset + (uint32_t)(pipeline->table_va - pipeline->pso_va);
		scratch = pipeline->scratch_block->mapping->va + pipeline->scratch_offset;
		memcpy(&word, table + 0x10U, 8U);
		expect(word == scratch && scratch < (1ULL << 32), "scratch: 0x10 is the compute warps' memory");
		memcpy(&word, table + 0x00U, 8U);
		expect(word == 0U, "scratch: no vertex warps' memory");
		memcpy(&word, table + 0x08U, 8U);
		expect(word == 0U, "scratch: no fragment warps' memory");
	}

	node_close(&node);
	stop("scratch");
}

/*
 * Counts that are not their arrays': the shared codec sizes each array by
 * its own count on the wire, so a Vulkan count field one past it ends the
 * frame (a set layout's bindings and immutable samplers, a pipeline
 * layout's set layouts and push-constant ranges, a pool's sizes, the set
 * layouts of an allocation, a module's words, a specialization's entries
 * and data, a graphics pipeline's attributes, attachments and dynamic
 * states); descriptor counts that wrap; a pool's sizes saturated; and a
 * batch of pipelines whose second cannot be kept takes the first with
 * it.
 */
static void
scene_counts(void)
{
	static const uint32_t types[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC };
	static const uint32_t stages[1] = { VK_SHADER_STAGE_COMPUTE_BIT };
	static const char *const skew_names[3] = {
		"counts: vertexAttributeDescriptionCount past the attributes", "counts: attachmentCount past the attachments",
		"counts: dynamicStateCount past the states"
	};
	struct test_graphics graphics;
	struct ghci_vk_object *object;
	uint64_t set_layouts[1];
	struct node node;
	uint32_t result;
	uint32_t index;
	int error;

	/* A device, the set layout and layout of dyn.comp, and its pipeline. */
	error = open_device(&node, MODEL_FULL);
	result = make_set_layout(&node, ID_SET_LAYOUT, 1U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	result |= make_layout(&node, ID_LAYOUT, 1U, set_layouts, 104U);
	result |= make_compute(&node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE);
	expect(error == 0 && result == 0U && pipeline_of(&node, ID_PIPELINE) != NULL, "counts: device");

	/* Bindings of 2 and 0xffffffff descriptors: the sum wraps. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(2U);
	put64(2U);
	put32(0U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(2U);
	put32(VK_SHADER_STAGE_ALL);
	put64(0U);
	put32(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(0xffffffffU);
	put32(VK_SHADER_STAGE_ALL);
	put64(0U);
	tail(ID_SET_LAYOUT + 1U);
	error = run(&node);
	expect(error == EINVAL && ghci_vk_object_find(session_of(&node), GHCI_VK_SET_LAYOUT, ID_SET_LAYOUT + 1U) == NULL,
	       "counts: bindings of 2 and 0xffffffff descriptors refused");

	/* bindingCount 2 over one binding; descriptorCount 2 over one immutable sampler. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(2U);
	put64(1U);
	put32(0U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(1U);
	put32(VK_SHADER_STAGE_ALL);
	put64(0U);
	tail(ID_SET_LAYOUT + 2U);
	error = run(&node);
	expect(error == EINVAL, "counts: bindingCount past the bindings");
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put64(1U);
	put32(0U);
	put32(VK_DESCRIPTOR_TYPE_SAMPLER);
	put32(2U);
	put32(VK_SHADER_STAGE_ALL);
	put64(1U);
	put64(ID_SAMPLER);
	tail(ID_SET_LAYOUT + 3U);
	error = run(&node);
	expect(error == EINVAL, "counts: descriptorCount past the immutable samplers");

	/* setLayoutCount 2 over one set layout; two push-constant ranges over one; 65 ranges. */
	begin(&node, GPU_OP_CREATE_PIPELINE_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(2U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put32(0U);
	put64(0U);
	tail(ID_LAYOUT + 1U);
	error = run(&node);
	expect(error == EINVAL, "counts: setLayoutCount past the set layouts");
	begin(&node, GPU_OP_CREATE_PIPELINE_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	put64(0U);
	put32(2U);
	put64(1U);
	put32(VK_SHADER_STAGE_ALL);
	put32(0U);
	put32(16U);
	tail(ID_LAYOUT + 2U);
	error = run(&node);
	expect(error == EINVAL, "counts: pushConstantRangeCount past the ranges");
	begin(&node, GPU_OP_CREATE_PIPELINE_LAYOUT);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(0U);
	put64(0U);
	put32(65U);
	put64(65U);
	for (index = 0U; index < 65U; index++) {
		put32(VK_SHADER_STAGE_ALL);
		put32(0U);
		put32(4U);
	}
	tail(ID_LAYOUT + 3U);
	error = run(&node);
	expect(error == EINVAL, "counts: more than 64 push-constant ranges");

	/* poolSizeCount 2 over one size; two sizes of 0xffffffff saturate. */
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put32(2U);
	put64(1U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(1U);
	tail(ID_POOL);
	error = run(&node);
	expect(error == EINVAL, "counts: poolSizeCount past the sizes");
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(2U);
	put32(3U);
	put64(3U);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(0xffffffffU);
	put32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	put32(0xffffffffU);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put32(2U);
	tail(ID_POOL + 1U);
	error = run(&node);
	object = ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_POOL, ID_POOL + 1U);
	expect(error == 0 && reply32(&node, 4U) == 0U && object != NULL && object->as.pool_of_sets.capacity[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] == 0xffffffffU,
	       "counts: a pool's sizes saturate");

	/* descriptorSetCount 2 over one set layout (two identities). */
	begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL + 1U);
	put32(2U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(2U);
	put64(ID_SET);
	put64(ID_SET + 1U);
	error = run(&node);
	expect(error == EINVAL && ghci_vk_object_find(session_of(&node), GHCI_VK_DESCRIPTOR_SET, ID_SET) == NULL,
	       "counts: descriptorSetCount past the set layouts");

	/* codeSize twice the words given. */
	begin(&node, GPU_OP_CREATE_SHADER_MODULE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(2U * module_bytes[MODULE_DYN_COMP]);
	put64(module_bytes[MODULE_DYN_COMP] / 4U);
	put_bytes(modules[MODULE_DYN_COMP], module_bytes[MODULE_DYN_COMP]);
	tail(ID_MODULE + 0x81U);
	error = run(&node);
	expect(error == EINVAL && ghci_vk_object_find(session_of(&node), GHCI_VK_SHADER_MODULE, ID_MODULE + 0x81U) == NULL,
	       "counts: codeSize past the words");

	/* A specialization of mapEntryCount 2 over one entry, then of dataSize 8 over 4 bytes. */
	for (index = 0U; index < 2U; index++) {
		begin(&node, GPU_OP_CREATE_COMPUTE_PIPELINES);
		put64(ID_DEVICE);
		put64(0U);
		put32(1U);
		put64(1U);
		put32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put32(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put32(VK_SHADER_STAGE_COMPUTE_BIT);
		put64(ID_MODULE + MODULE_SPEC_COMP);
		put64(5U);
		put_bytes("main", 5U);
		put64(1U);
		put32(index == 0U ? 2U : 1U);
		put64(1U);
		put32(7U);
		put32(0U);
		put64(4U);
		put64(index == 0U ? 4U : 8U);
		put64(4U);
		put32(9U);
		put64(ID_LAYOUT);
		put64(0U);
		put32(0xffffffffU);
		put64(0U);
		put64(1U);
		put64(ID_PIPELINE + 1U);
		error = run(&node);
		expect(error == EINVAL && pipeline_of(&node, ID_PIPELINE + 1U) == NULL,
		       index == 0U ? "counts: mapEntryCount past the entries" : "counts: dataSize past the data");
	}

	/* A graphics pipeline's attribute, attachment and dynamic state counts one past their arrays. */
	for (index = SKEW_ATTRIBUTES; index <= SKEW_DYNAMIC; index++) {
		graphics_defaults(&graphics);
		graphics.layout = ID_LAYOUT;
		graphics.dynamic_count = 1U;
		graphics.dynamic[0] = VK_DYNAMIC_STATE_VIEWPORT;
		graphics.skew = index;
		result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
		expect(result == ~0U && last_run == EINVAL && pipeline_of(&node, ID_PIPELINE + 2U) == NULL, skew_names[index - SKEW_ATTRIBUTES]);
	}
	graphics.skew = SKEW_NONE;
	result = make_graphics(&node, &graphics, ID_PIPELINE + 2U);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE + 2U) != NULL, "counts: the same pipeline with its counts made");

	/* Two compute pipelines whose second identity is taken: neither is made. */
	begin(&node, GPU_OP_CREATE_COMPUTE_PIPELINES);
	put64(ID_DEVICE);
	put64(0U);
	put32(2U);
	put64(2U);
	for (index = 0U; index < 2U; index++) {
		put32(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
		put64(0U);
		put32(0U);
		put_stage(VK_SHADER_STAGE_COMPUTE_BIT, ID_MODULE + MODULE_DYN_COMP, NULL, 0U);
		put64(ID_LAYOUT);
		put64(0U);
		put32(0xffffffffU);
	}
	put64(0U);
	put64(2U);
	put64(ID_PIPELINE + 3U);
	put64(ID_PIPELINE);
	error = run(&node);
	expect(error == EINVAL && pipeline_of(&node, ID_PIPELINE + 3U) == NULL && pipeline_of(&node, ID_PIPELINE) != NULL,
	       "counts: a batch whose second pipeline cannot be kept makes none");

	/* The pipeline and layouts keep each set's place of dynamic offsets (set 0: offsets 0 to 0). */
	expect(pipeline_of(&node, ID_PIPELINE)->dynamic_base[0] == 0U && pipeline_of(&node, ID_PIPELINE)->dynamic_sets[0] == 1U &&
	       pipeline_of(&node, ID_PIPELINE)->dynamic_base[1] == 1U && pipeline_of(&node, ID_PIPELINE)->dynamic_sets[1] == 0U,
	       "counts: each set's dynamic offsets");

	node_close(&node);
	stop("counts");
}

/*
 * The session's budget: the objects' bytes are taken and given back, and
 * a module, a set layout, a pipeline layout, a set and a pipeline past
 * it are answered VK_ERROR_OUT_OF_HOST_MEMORY.
 */
static void
scene_budget(void)
{
	static const uint32_t types[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC };
	static const uint32_t stages[1] = { VK_SHADER_STAGE_COMPUTE_BIT };
	struct ghci_vk_session *session;
	uint64_t set_layouts[1];
	uint64_t before;
	uint64_t saved;
	struct node node;
	uint32_t result;
	int error;

	/* A device: its modules are charged. */
	error = open_device(&node, MODEL_FULL);
	session = session_of(&node);
	before = session->budget_used;
	expect(error == 0 && before >= module_bytes[MODULE_TRI_VERT] + module_bytes[MODULE_SPILL_COMP], "budget: the modules are charged");

	/* A set layout takes bytes and its destroy gives them back. */
	result = make_set_layout(&node, ID_SET_LAYOUT + 1U, 1U, types, stages, 0U);
	expect(result == 0U && session->budget_used > before, "budget: a set layout is charged");
	begin(&node, GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT);
	put64(ID_DEVICE);
	put64(ID_SET_LAYOUT + 1U);
	put64(0U);
	error = run(&node);
	expect(error == 0 && session->budget_used == before, "budget: given back at its destroy");

	/* The objects a set and a pipeline need, then a budget with 16 bytes left. */
	result = make_set_layout(&node, ID_SET_LAYOUT, 1U, types, stages, 0U);
	set_layouts[0] = ID_SET_LAYOUT;
	result |= make_layout(&node, ID_LAYOUT, 1U, set_layouts, 104U);
	begin(&node, GPU_OP_CREATE_DESCRIPTOR_POOL);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
	put64(0U);
	put32(0U);
	put32(1U);
	put32(1U);
	put64(1U);
	put32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
	put32(1U);
	tail(ID_POOL);
	error = run(&node);
	expect(error == 0 && result == 0U, "budget: layouts and a pool");
	saved = session->budget_used;
	session->budget_used = GHCI_VK_BUDGET_BYTES - 16U;

	/* A module, a set layout, a pipeline layout, a set and a pipeline past the budget. */
	begin(&node, GPU_OP_CREATE_SHADER_MODULE);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	put64(0U);
	put32(0U);
	put64(module_bytes[MODULE_DYN_COMP]);
	put64(module_bytes[MODULE_DYN_COMP] / 4U);
	put_bytes(modules[MODULE_DYN_COMP], module_bytes[MODULE_DYN_COMP]);
	tail(ID_MODULE + 0x82U);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY &&
	       ghci_vk_object_find(session, GHCI_VK_SHADER_MODULE, ID_MODULE + 0x82U) == NULL, "budget: a module past it");
	result = make_set_layout(&node, ID_SET_LAYOUT + 2U, 1U, types, stages, 0U);
	expect(last_run == 0 && result == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY &&
	       ghci_vk_object_find(session, GHCI_VK_SET_LAYOUT, ID_SET_LAYOUT + 2U) == NULL, "budget: a set layout past it");
	result = make_layout(&node, ID_LAYOUT + 1U, 1U, set_layouts, 0U);
	expect(last_run == 0 && result == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY &&
	       ghci_vk_object_find(session, GHCI_VK_PIPELINE_LAYOUT, ID_LAYOUT + 1U) == NULL, "budget: a pipeline layout past it");
	begin(&node, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	put64(ID_DEVICE);
	put64(1U);
	put32(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	put64(0U);
	put64(ID_POOL);
	put32(1U);
	put64(1U);
	put64(ID_SET_LAYOUT);
	put64(1U);
	put64(ID_SET);
	error = run(&node);
	expect(error == 0 && reply32(&node, 4U) == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY &&
	       ghci_vk_object_find(session, GHCI_VK_DESCRIPTOR_SET, ID_SET) == NULL, "budget: a set past it");
	result = make_compute(&node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE);
	expect(last_run == 0 && result == (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY && pipeline_of(&node, ID_PIPELINE) == NULL,
	       "budget: a pipeline past it");
	expect(session->budget_used == GHCI_VK_BUDGET_BYTES - 16U, "budget: nothing taken by the refused");

	/* The budget as it was: the pipeline is made and charged. */
	session->budget_used = saved;
	result = make_compute(&node, ID_MODULE + MODULE_DYN_COMP, ID_LAYOUT, NULL, 0U, ID_PIPELINE);
	expect(result == 0U && pipeline_of(&node, ID_PIPELINE) != NULL && session->budget_used > saved, "budget: a pipeline within it");

	node_close(&node);
	stop("budget");
}

/* The scenes of the execution of recorded commands (compiled with these helpers). */
#include "ghci-execute-host-test.c"
#include "ghci-meta-host-test.c"
