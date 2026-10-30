/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU's cost of mview's --draws=each frames in the nvrtx Vulkan
 * executor on the host, and each draw's GPU state for comparing two
 * executors (plan/ws291/tests/nvrtx-draws-bench.sh).  Built from the vk-d3
 * host test (plan/tools/nvrtx/nvrtx-vk-d3-host-test.c), whose checks still
 * run with one argument; the fake GPU runs each push buffer through a
 * model of the 3D class's state (the methods' values, the loads into
 * constant buffers, the groups' bindings) and keeps a hash of the state at
 * each draw.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "userland/desktop/libvulkan/internal.h"
#include "userland/desktop/libvulkan/codec.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"
#include "userland/desktop/wayland/shaders.h"

/* The reply blob's id, where its replies start, and the device's identity. */
#define TEST_REPLY_BLOB		1U
#define TEST_REPLY_AT		16U
#define TEST_DEVICE		0x102U

static unsigned failures;
static unsigned checks;
static uint32_t reply_words[2048];
static unsigned allocations;
static unsigned detached;
static void *detached_backing;
static uint8_t storage[0x20000];
static uint8_t scene[0x40000];
static int backing_token;
static int import_token;

static void check(int ok, const char *what);
static void frame_begin(struct vulkan_writer *writer);
static int frame_run(struct nvrtx_vk_session *session, struct vulkan_writer *writer);
static uint32_t reply_u32(unsigned *at);
static uint64_t reply_u64(unsigned *at);
static void command_begin(struct vulkan_writer *writer, uint32_t opcode);
static void write_tail(struct vulkan_writer *writer, uint64_t identity);
static void write_allocate(struct vulkan_writer *writer, uint64_t identity, uint64_t bytes, uint32_t imported);
static void write_image(struct vulkan_writer *writer, uint64_t identity, uint32_t format, uint32_t width, uint32_t height, uint32_t levels);
static void write_buffer(struct vulkan_writer *writer, uint64_t identity, uint64_t bytes);
static void write_pair(struct vulkan_writer *writer, uint32_t opcode, uint64_t first, uint64_t second);
static void write_bind(struct vulkan_writer *writer, uint32_t opcode, uint64_t resource, uint64_t memory, uint64_t offset);
static void test_memory(void);
static void test_views(void);
static uint32_t *read_spirv(const char *directory, const char *name, size_t *bytes);
static void write_create(struct vulkan_writer *writer, uint32_t opcode);
static void test_pipeline(const char *directory);
static void write_quad_pipeline(struct vulkan_writer *client, struct vulkan_object *handles, uint64_t identity, unsigned vertex, unsigned fragment, uint64_t layout, uint64_t pass);
static void record_begin(struct vulkan_writer *writer, uint32_t opcode, uint64_t command);
static void test_submit(const char *directory);
static void test_compositor_shaders(void);

void *
kern_malloc(size_t bytes)
{
	allocations++;
	return malloc(bytes);
}

void *
kern_calloc(size_t count, size_t bytes)
{
	allocations++;
	return calloc(count, bytes);
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		allocations--;
	free(pointer);
}

void
kern_logf(const char *format, ...)
{
	(void)format;
}

/* libvulkan's handles are its objects' addresses (objects.c). */
uint64_t
vulkan_object_wire_id(const struct vulkan_object *object)
{
	if (object == NULL)
		return 0;
	return object->wire_id;
}

struct vulkan_object *
vulkan_nondispatchable_object(uint64_t handle)
{
	return (struct vulkan_object *)(uintptr_t)handle;
}

/* The one blob: the reply array. */
int
nvrtx_vk_blob(void *owner, uint32_t resource_id, uint8_t **cpu, uint64_t *bytes)
{
	(void)owner;
	if (resource_id != TEST_REPLY_BLOB)
		return ENOENT;
	*cpu = (uint8_t *)reply_words;
	*bytes = sizeof(reply_words);
	return 0;
}

uint32_t
nvrtx_vk_device_id(void *owner)
{
	(void)owner;
	return 0x1f02U;
}

uint64_t
nvrtx_vk_heap_bytes(void *owner)
{
	(void)owner;
	return 240ULL << 20;
}

/* The generation the fake owner reports: Turing first, then Ampere and Ada. */
static uint32_t test_shader_model = 75U;
static uint32_t test_threed_class = 0xc597U;

uint32_t
nvrtx_vk_shader_model(void *owner)
{
	(void)owner;
	return test_shader_model;
}

uint32_t
nvrtx_vk_threed_class(void *owner)
{
	(void)owner;
	return test_threed_class;
}

void
nvrtx_vk_memory_detach(void *owner, void *backing, uint64_t va, uint64_t bytes)
{
	(void)owner;
	(void)va;
	(void)bytes;
	detached++;
	detached_backing = backing;
}

int
nvrtx_vk_memory_kind(void *owner, void *backing, uint64_t offset, uint64_t va, uint64_t bytes, uint32_t kind)
{
	(void)owner;
	(void)backing;
	(void)offset;
	(void)va;
	(void)bytes;
	(void)kind;
	return 0;
}

/* Resource 7 is a blob of 0x10000 bytes the session can import. */
int
nvrtx_vk_memory_import(void *owner, uint32_t resource_id, uint64_t bytes, uint8_t **cpu, uint64_t *va, void **backing)
{
	(void)owner;
	if (resource_id != 7U)
		return ENOENT;
	if (bytes > 0x10000U)
		return EINVAL;
	*cpu = storage + 0x10000U;
	*va = 0x31000000ULL;
	*backing = &import_token;
	return 0;
}

/* The executor's GPU memory: each run calloc'd, at its own GPU address. */
struct test_run {
	uint64_t va;
	uint8_t *cpu;
	uint64_t bytes;
};

static struct test_run runs[64];
static unsigned run_count;
static unsigned gpu_runs;
static unsigned gpu_draws;
static unsigned gpu_quads;
static unsigned gpu_copies;
static uint64_t gpu_copied_bytes;

/* The methods the runs wrote, in order: a method's byte address and its word. */
static uint32_t method_log[65536][2];
static unsigned method_count;

int
nvrtx_vk_gpu_alloc(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va)
{
	(void)owner;
	if (run_count >= 64U)
		return ENOMEM;
	*cpu = calloc(1U, (size_t)bytes);
	if (*cpu == NULL)
		return ENOMEM;
	runs[run_count].va = 0x40000000ULL + (uint64_t)run_count * 0x01000000ULL;
	runs[run_count].cpu = *cpu;
	runs[run_count].bytes = bytes;
	*token = *cpu;
	*va = runs[run_count].va;
	run_count++;
	return 0;
}

/* System memory for the executor looks the same to the fakes. */
int
nvrtx_vk_gpu_alloc_system(void *owner, uint64_t bytes, void **token, uint8_t **cpu, uint64_t *va)
{
	return nvrtx_vk_gpu_alloc(owner, bytes, token, cpu, va);
}

/* Every memory the tests attach or import comes with its CPU view. */
int
nvrtx_vk_memory_cpu(void *owner, void *backing, uint8_t **cpu)
{
	(void)owner;
	(void)backing;
	(void)cpu;
	return ENOSPC;
}

void
nvrtx_vk_gpu_free(void *owner, void *token)
{
	unsigned index;

	(void)owner;
	for (index = 0; index < run_count; index++) {
		if (runs[index].cpu == token)
			runs[index].cpu = NULL;
	}
	free(token);
}

/* Counts the CPU's copies the executor reports. */
void
nvrtx_vk_gpu_copied(void *owner, uint64_t bytes)
{
	(void)owner;
	gpu_copies++;
	gpu_copied_bytes += bytes;
}

/* Counts the draws and the executor's own quads of the runs. */
void
nvrtx_vk_gpu_drawn(void *owner, uint32_t draws, uint32_t quads)
{
	(void)owner;
	gpu_draws += draws;
	gpu_quads += quads;
}

/* Copies a command stream out of a blob, as the node does through its window. */
void
nvrtx_vk_blob_read(void *owner, uint8_t *to, const uint8_t *from, size_t length)
{
	(void)owner;
	memcpy(to, from, length);
}

/* The fence of the newest run the fakes ran (each submission runs at once). */
static uint32_t gpu_fence;

static uint8_t *gpu_cpu(uint64_t va);

/*
 * The GPU's state as a run's methods leave it, for the draws' snapshots:
 * the methods' last values, constant buffer 0's loads into memory, and
 * each group's constant buffers.
 */
#define SIM_SAMPLER_POOL 0x155cU
static uint64_t sim_texture(uint64_t hash, const uint8_t *handle_bytes);
static uint32_t sim_regs[0x1000];
static uint64_t sim_bound_va[5][8];
static uint32_t sim_bound_size[5][8];
static uint64_t *sim_snapshots;
static size_t sim_snapshot_count;
static size_t sim_snapshot_capacity;
static int sim_on = 1;

static uint64_t
sim_fnv(uint64_t hash, const void *bytes, size_t length)
{
	const uint8_t *at;
	size_t index;

	at = bytes;
	for (index = 0; index < length; index++) {
		hash ^= at[index];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* Whether a method is an action or names the selected buffer (not part of a draw's state). */
static int
sim_transient(uint32_t method)
{
	if (method >= 0x2380U && method <= 0x23ccU)
		return 1;
	if (method >= 0x2400U && method < 0x2500U)
		return 1;
	if (method == 0x1614U || method == 0x0da4U || method == 0x1288U || method == 0x0110U)
		return 1;
	if (method == 0x1424U || method == 0x1428U)
		return 1;
	if ((method >= 0x155cU && method <= 0x1564U) || (method >= 0x1574U && method <= 0x157cU))
		return 1;
	return 0;
}

/* Adds a texture handle's header and sampler words (from the pools the methods name) to a hash. */
static uint64_t
sim_texture(uint64_t hash, const uint8_t *handle_bytes)
{
	uint32_t handle;
	uint64_t headers;
	uint64_t samplers;
	uint8_t *cpu;

	memcpy(&handle, handle_bytes, 4);
	headers = ((uint64_t)sim_regs[0x1574U >> 2] << 32) | sim_regs[0x1578U >> 2];
	samplers = ((uint64_t)sim_regs[SIM_SAMPLER_POOL >> 2] << 32) | sim_regs[(SIM_SAMPLER_POOL + 4U) >> 2];
	cpu = gpu_cpu(headers + (uint64_t)(handle & 0xfffffU) * 32U);
	if (cpu != NULL)
		hash = sim_fnv(hash, cpu, 32);
	cpu = gpu_cpu(samplers + (uint64_t)(handle >> 20) * 32U);
	if (cpu != NULL)
		hash = sim_fnv(hash, cpu, 32);
	return hash;
}

/* A program's address (SET_PIPELINE_PROGRAM_ADDRESS_A and _B of the six program types): hashed by the program's bytes instead. */
static int
sim_program_address(uint32_t method)
{
	if (method < 0x2000U || method >= 0x2000U + 6U * 0x40U)
		return 0;
	return ((method & 0x3fU) == 0x14U || (method & 0x3fU) == 0x18U);
}

/*
 * One draw's state: the methods' values (a program's address by the
 * program's first bytes), then each group's bound buffers (buffer 0 by its
 * bytes, the others by place).
 */
static void
sim_snapshot(void)
{
	uint64_t program;
	uint64_t hash;
	uint32_t method;
	unsigned group;
	unsigned slot;
	uint8_t *cpu;

	hash = 0xcbf29ce484222325ULL;
	for (method = 0; method < 0x1000U; method++) {
		if (sim_transient(method << 2))
			continue;
		if (sim_program_address(method << 2))
			continue;
		hash = sim_fnv(hash, &sim_regs[method], 4);
	}

	/* Each enabled program by its first bytes (its header and the start of its code), not by where it lies. */
	for (group = 0; group < 6; group++) {
		program = ((uint64_t)sim_regs[(0x2014U + group * 0x40U) >> 2] << 32) | sim_regs[(0x2018U + group * 0x40U) >> 2];
		if ((sim_regs[(0x2000U + group * 0x40U) >> 2] & 1U) == 0U || program == 0U)
			continue;
		cpu = gpu_cpu(program);
		if (cpu != NULL)
			hash = sim_fnv(hash, cpu, 256);
		else
			hash = sim_fnv(hash, &program, 8);
	}
	for (group = 0; group < 5; group++) {
		for (slot = 0; slot < 8; slot++) {
			if (sim_bound_va[group][slot] == 0)
				continue;
			if (slot == 0) {
				cpu = gpu_cpu(sim_bound_va[group][slot]);
				if (cpu != NULL) {
					/* The bench's one texture by its header and sampler (their places in the pools differ), the rest as they are. */
					hash = sim_fnv(hash, cpu, 0x400);
					hash = sim_fnv(hash, cpu + 0x404, sim_bound_size[group][slot] - 0x404);
					hash = sim_texture(hash, cpu + 0x400);
				}
			} else {
				hash = sim_fnv(hash, &sim_bound_va[group][slot], 8);
				hash = sim_fnv(hash, &sim_bound_size[group][slot], 4);
			}
		}
	}
	if (getenv("BENCH_DUMP") != NULL && (size_t)atoi(getenv("BENCH_DUMP")) == sim_snapshot_count) {
		for (method = 0; method < 0x1000U; method++) {
			if (!sim_transient(method << 2) && sim_regs[method] != 0)
				printf("dump reg %04x = %08x\n", method << 2, sim_regs[method]);
		}
		for (group = 0; group < 5; group++) {
			for (slot = 0; slot < 8; slot++) {
				if (sim_bound_va[group][slot] == 0)
					continue;
				printf("dump bind %u.%u %llx %x\n", group, slot, (unsigned long long)sim_bound_va[group][slot], sim_bound_size[group][slot]);
				cpu = gpu_cpu(sim_bound_va[group][slot]);
				if (slot == 0 && cpu != NULL) {
					for (method = 0; method < sim_bound_size[group][slot]; method += 4) {
						if (cpu[method] | cpu[method + 1] | cpu[method + 2] | cpu[method + 3])
							printf("dump cb %u %03x = %08x\n", group, method, *(uint32_t *)(cpu + method));
					}
				}
			}
		}
	}
	if (sim_snapshot_count == sim_snapshot_capacity) {
		sim_snapshot_capacity = sim_snapshot_capacity == 0 ? 4096 : sim_snapshot_capacity * 2;
		sim_snapshots = realloc(sim_snapshots, sim_snapshot_capacity * sizeof(*sim_snapshots));
	}
	sim_snapshots[sim_snapshot_count++] = hash;
}

/* One method's word: kept, or the action it starts. */
static void
sim_method(uint32_t method, uint32_t value)
{
	uint64_t selected;
	uint32_t offset;
	unsigned group;
	unsigned slot;
	uint8_t *cpu;

	if (method_count < 65536U) {
		method_log[method_count][0] = method;
		method_log[method_count][1] = value;
		method_count++;
	}
	if (!sim_on) {
		sim_regs[(method >> 2) & 0xfffU] = value;
		return;
	}
	selected = ((uint64_t)sim_regs[0x2384U >> 2] << 32) | sim_regs[0x2388U >> 2];
	if (method >= 0x2390U && method <= 0x23ccU) {
		offset = sim_regs[0x238cU >> 2];
		cpu = gpu_cpu(selected + offset);
		if (cpu != NULL)
			memcpy(cpu, &value, 4);
		sim_regs[0x238cU >> 2] = offset + 4U;
		return;
	}
	if (method >= 0x2410U && method < 0x2410U + 5U * 0x20U && ((method - 0x2410U) % 0x20U) == 0U) {
		group = (method - 0x2410U) / 0x20U;
		slot = (value >> 4) & 0x1fU;
		if (slot < 8) {
			sim_bound_va[group][slot] = 0;
			if ((value & 1U) != 0U) {
				sim_bound_va[group][slot] = selected;
				sim_bound_size[group][slot] = sim_regs[0x2380U >> 2];
			}
		}
		return;
	}
	sim_regs[(method >> 2) & 0xfffU] = value;
	if (getenv("BENCH_TRACE") != NULL) {
		if ((size_t)atoi(getenv("BENCH_TRACE")) <= sim_snapshot_count && sim_snapshot_count < (size_t)atoi(getenv("BENCH_TRACE")) + 3U)
			printf("trace %zu %04x %08x\n", sim_snapshot_count, method, value);
	}
	if (method == 0x1614U)
		sim_snapshot();
}

/* Runs a push buffer through the state model: incrementing, one-incrementing, non-incrementing and immediate headers of the 3D subchannel. */
static int
gpu_run(uint64_t va, uint32_t bytes)
{
	const uint8_t *push;
	uint32_t header;
	uint32_t count;
	uint32_t method;
	uint32_t operation;
	uint32_t at;
	uint32_t word;
	uint32_t value;
	unsigned index;

	push = NULL;
	for (index = 0; index < run_count; index++) {
		if (runs[index].cpu != NULL && va >= runs[index].va && va + bytes <= runs[index].va + runs[index].bytes)
			push = runs[index].cpu + (va - runs[index].va);
	}
	if (push == NULL)
		return EIO;
	gpu_runs++;
	if (!sim_on)
		return 0;
	at = 0;
	while (at < bytes) {
		memcpy(&header, push + at, 4);
		at += 4;
		count = (header >> 16) & 0x1fffU;
		method = (header & 0xfffU) << 2;
		operation = header >> 29;
		if (((header >> 13) & 7U) != 0U)
			return EIO;
		if (operation == 4U) {
			sim_method(method, count);
			continue;
		}
		if (operation != 1U && operation != 3U && operation != 5U)
			return EIO;
		for (word = 0; word < count; word++) {
			memcpy(&value, push + at, 4);
			sim_method(method, value);
			if (operation == 1U || (operation == 5U && word == 0U))
				method += 4U;
			at += 4;
		}
	}
	return 0;
}

/* A submission runs at once and is named by the next fence. */
int
nvrtx_vk_gpu_submit(void *owner, uint64_t va, uint32_t bytes, struct nvrtx_vk_ticket *ticket)
{
	int error;

	(void)owner;
	error = gpu_run(va, bytes);
	if (error != 0)
		return error;
	gpu_fence++;
	ticket->fence = gpu_fence;
	ticket->submitted_us = 0U;
	return 0;
}

int
nvrtx_vk_gpu_wait(void *owner, const struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	(void)ticket;
	return 0;
}

bool
nvrtx_vk_gpu_passed(void *owner, const struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	(void)ticket;
	return true;
}

void
nvrtx_vk_gpu_retire(void *owner, struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	ticket->fence = 0U;
}

void
nvrtx_vk_gpu_last(void *owner, struct nvrtx_vk_ticket *ticket)
{
	(void)owner;
	ticket->fence = gpu_fence;
	ticket->submitted_us = 0U;
}

int
nvrtx_vk_gpu_idle(void *owner)
{
	(void)owner;
	return 0;
}

bool
nvrtx_vk_gpu_state_kept(void *owner)
{
	(void)owner;
	return false;
}

uint64_t
nvrtx_vk_gpu_quiet(void *owner, void *backing)
{
	(void)owner;
	(void)backing;
	return 0U;
}

/* The node decodes no video here. */
bool
nvrtx_vk_video_available(
	void *owner)
{
	(void)owner;

	/* Reports no decoder. */
	return false;
}

/* Refuses a decode, which the executor under test never reaches without a decoder. */
int
nvrtx_vk_video_decode(
	void *owner,
	uint64_t va,
	uint32_t bytes)
{
	(void)owner;
	(void)va;
	(void)bytes;

	/* Refuses the decode. */
	return EIO;
}

/* Reports the cpu of a GPU address the fakes handed out. */
static uint8_t *
gpu_cpu(uint64_t va)
{
	unsigned index;

	for (index = 0; index < run_count; index++) {
		if (runs[index].cpu != NULL && va >= runs[index].va && va < runs[index].va + runs[index].bytes)
			return runs[index].cpu + (va - runs[index].va);
	}
	return NULL;
}

/* Reports how many times a method was written with a value (any value when every is set). */
static unsigned
methods_with(uint32_t method, uint32_t value, int every)
{
	unsigned index;
	unsigned found;

	found = 0;
	for (index = 0; index < method_count; index++) {
		if (method_log[index][0] == method && (every || method_log[index][1] == value))
			found++;
	}
	return found;
}

/* Reports the value a method was last written with, or 0xdeadbeef. */
static uint32_t
method_last(uint32_t method)
{
	unsigned index;
	uint32_t value;

	value = 0xdeadbeefU;
	for (index = 0; index < method_count; index++) {
		if (method_log[index][0] == method)
			value = method_log[index][1];
	}
	return value;
}


static double
bench_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6;
}

/* Allocates a descriptor set of the layout 0x50b from the pool 0x50d and points it at a view and the sampler. */
static void
bench_set(struct vulkan_writer *client, struct vulkan_object *handles, uint64_t set, uint64_t view)
{
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[1];

	layouts[0] = (VkDescriptorSetLayout)(uintptr_t)&handles[11];
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = (VkDescriptorPool)(uintptr_t)&handles[13];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = layouts;
	command_begin(client, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	vulkan_write_u64(client, TEST_DEVICE);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkDescriptorSetAllocateInfo(client, &allocate);
	vulkan_write_u64(client, 1);
	vulkan_write_u64(client, set);
	command_begin(client, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	vulkan_write_u64(client, TEST_DEVICE);
	vulkan_write_u32(client, 1);
	vulkan_write_u64(client, 1);
	vulkan_write_u32(client, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	vulkan_write_u64(client, 0);
	vulkan_write_u64(client, set);
	vulkan_write_u32(client, 0);
	vulkan_write_u32(client, 0);
	vulkan_write_u32(client, 1);
	vulkan_write_u32(client, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	vulkan_write_u64(client, 1);
	vulkan_write_u64(client, 0x508);
	vulkan_write_u64(client, view);
	vulkan_write_u32(client, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vulkan_write_u64(client, 0);
	vulkan_write_u64(client, 0);
	vulkan_write_u32(client, 0);
	vulkan_write_u64(client, 0);
}

/*
 * mview's --draws=each frame: per copy sixteen groups, each with its
 * material's pipeline (two kinds, bound when it changes), its texture's set
 * (two), its push constants (the colour per group) and an indexed draw of
 * the copy's instance; submits parts of the copies (submits >= 1).  Times
 * the Begin (the reset), the recording and the submission over frames, and
 * writes each draw's GPU state snapshot of the first frame to path.
 */
static void
bench_draws(const char *directory, unsigned copies, unsigned frames, unsigned submits, const char *path)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct vulkan_object handles[40];
	VkShaderModuleCreateInfo module;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo pipeline_layout;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo render_pass;
	VkFramebufferCreateInfo framebuffer_info;
	VkCommandPoolCreateInfo command_pool;
	VkCommandBufferAllocateInfo command_allocate;
	VkCommandBufferBeginInfo begin;
	VkViewport vp;
	VkRect2D rect;
	VkImageView attachments[1];
	VkDescriptorSetLayout layouts[1];
	uint32_t *vertex_words;
	uint32_t *fragment_words;
	uint32_t corners[12] = { 0, 0, 0x3f800000, 0, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000 };
	uint32_t indices[6] = { 0, 1, 2, 3, 4, 5 };
	uint32_t push[8];
	size_t vertex_bytes;
	size_t fragment_bytes;
	double reset_ms;
	double record_ms;
	double submit_ms;
	double t0;
	double t1;
	unsigned frame;
	unsigned copy;
	unsigned group;
	unsigned part;
	unsigned first;
	unsigned last;
	unsigned kind;
	unsigned bound;
	unsigned index;
	uint64_t command;
	FILE *file;
	int error;

	vertex_words = read_spirv(directory, "quad-vert", &vertex_bytes);
	fragment_words = read_spirv(directory, "quad-frag", &fragment_bytes);
	if (vertex_words == NULL || fragment_words == NULL) {
		printf("bench: no SPIR-V\n");
		return;
	}
	if (nvrtx_vk_session_open(NULL, &session) != 0) {
		printf("bench: no session\n");
		return;
	}
	memset(handles, 0, sizeof(handles));
	for (index = 0; index < 40; index++)
		handles[index].wire_id = 0x500 + index;

	/* Memory, the target and texture images, the corners, the indices. */
	frame_begin(&client);
	write_allocate(&client, 0x501, 0x40000, 0);
	write_image(&client, 0x502, VK_FORMAT_B8G8R8A8_UNORM, 64, 64, 1);
	write_image(&client, 0x503, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1);
	write_buffer(&client, 0x504, sizeof(corners));
	write_buffer(&client, 0x505, sizeof(indices));
	write_image(&client, 0x51a, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1);
	error = frame_run(session, &client);
	if (error != 0 || nvrtx_vk_memory_attach(session, 0x501, 0x40000, scene, 0x30000000ULL, &backing_token) != 0) {
		printf("bench: memory %d\n", error);
		return;
	}
	frame_begin(&client);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x502, 0x501, 0);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x503, 0x501, 0x4000);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x504, 0x501, 0x6000);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x505, 0x501, 0x8000);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x51a, 0x501, 0xa000);
	error = frame_run(session, &client);
	memcpy(scene + 0x6000, corners, sizeof(corners));
	memcpy(scene + 0x8000, indices, sizeof(indices));

	/* Views, sampler, modules, layouts, pool and two sets. */
	frame_begin(&client);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	view.image = (VkImage)(uintptr_t)&handles[2];
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x506);
	view.image = (VkImage)(uintptr_t)&handles[3];
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x507);
	view.image = (VkImage)(uintptr_t)&handles[26];
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x51b);
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	write_create(&client, GPU_OP_CREATE_SAMPLER);
	vulkan_encode_VkSamplerCreateInfo(&client, &sampler);
	write_tail(&client, 0x508);
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = vertex_bytes;
	module.pCode = vertex_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x509);
	module.codeSize = fragment_bytes;
	module.pCode = fragment_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x50a);
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1;
	set_layout.pBindings = &binding;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&client, &set_layout);
	write_tail(&client, 0x50b);
	layouts[0] = (VkDescriptorSetLayout)(uintptr_t)&handles[11];
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 32;
	memset(&pipeline_layout, 0, sizeof(pipeline_layout));
	pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout.setLayoutCount = 1;
	pipeline_layout.pSetLayouts = layouts;
	pipeline_layout.pushConstantRangeCount = 1;
	pipeline_layout.pPushConstantRanges = &range;
	write_create(&client, GPU_OP_CREATE_PIPELINE_LAYOUT);
	vulkan_encode_VkPipelineLayoutCreateInfo(&client, &pipeline_layout);
	write_tail(&client, 0x50c);
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 14;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 14;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_POOL);
	vulkan_encode_VkDescriptorPoolCreateInfo(&client, &pool);
	write_tail(&client, 0x50d);
	bench_set(&client, handles, 0x50e, 0x507);
	bench_set(&client, handles, 0x51c, 0x51b);

	/* Twelve more views of the second texture, a set each (mview's groups each sample their own texture). */
	view.image = (VkImage)(uintptr_t)&handles[3];
	for (index = 0; index < 12; index++) {
		write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
		vulkan_encode_VkImageViewCreateInfo(&client, &view);
		write_tail(&client, 0x540 + index);
		bench_set(&client, handles, 0x560 + index, 0x540 + index);
	}

	/* Render pass, framebuffer, two pipelines, command pool and buffer, fence. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	memset(&reference, 0, sizeof(reference));
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	memset(&render_pass, 0, sizeof(render_pass));
	render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass.attachmentCount = 1;
	render_pass.pAttachments = &attachment;
	render_pass.subpassCount = 1;
	render_pass.pSubpasses = &subpass;
	write_create(&client, GPU_OP_CREATE_RENDER_PASS);
	vulkan_encode_VkRenderPassCreateInfo(&client, &render_pass);
	write_tail(&client, 0x50f);
	attachments[0] = (VkImageView)(uintptr_t)&handles[6];
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = (VkRenderPass)(uintptr_t)&handles[15];
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = attachments;
	framebuffer_info.width = 64;
	framebuffer_info.height = 64;
	framebuffer_info.layers = 1;
	write_create(&client, GPU_OP_CREATE_FRAMEBUFFER);
	vulkan_encode_VkFramebufferCreateInfo(&client, &framebuffer_info);
	write_tail(&client, 0x510);
	write_quad_pipeline(&client, handles, 0x511, 9, 10, 0x50c, 0x50f);
	write_quad_pipeline(&client, handles, 0x516, 9, 10, 0x50c, 0x50f);
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	write_create(&client, GPU_OP_CREATE_COMMAND_POOL);
	vulkan_encode_VkCommandPoolCreateInfo(&client, &command_pool);
	write_tail(&client, 0x512);
	memset(&command_allocate, 0, sizeof(command_allocate));
	command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_allocate.commandPool = (VkCommandPool)(uintptr_t)&handles[18];
	command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_allocate.commandBufferCount = 8;
	for (part = 0; part < 1000; part += 8) {
		if (part != 0) {
			error = frame_run(session, &client);
			if (error != 0) {
				printf("bench: command buffers %d\n", error);
				return;
			}
			frame_begin(&client);
		}
		command_begin(&client, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
		vulkan_write_u64(&client, TEST_DEVICE);
		vulkan_write_u64(&client, 1);
		vulkan_encode_VkCommandBufferAllocateInfo(&client, &command_allocate);
		vulkan_write_u64(&client, 8);
		for (index = 0; index < 8; index++)
			vulkan_write_u64(&client, 0x1000 + part + index);
	}
	command_begin(&client, GPU_OP_CREATE_FENCE);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	write_tail(&client, 0x514);
	error = frame_run(session, &client);
	if (error != 0 || nvrtx_vk_payload(session, I915_VK_OBJ_PIPELINE, 0x516) == NULL ||
	    nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, 0x1000 + 999) == NULL) {
		printf("bench: objects %d\n", error);
		return;
	}
	if (submits == 0 || submits > 1000)
		submits = 1;

	/* The frames. */
	reset_ms = 0;
	record_ms = 0;
	submit_ms = 0;
	sim_on = 1;
	for (frame = 0; frame < frames; frame++) {
		if (frame == 1)
			sim_on = 0;
		for (part = 0; part < submits; part++) {
			command = 0x1000 + part;
			first = copies * part / submits;
			last = copies * (part + 1) / submits;

			/* Begin: the command buffer's records of the last frame let go. */
			frame_begin(&client);
			memset(&begin, 0, sizeof(begin));
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			command_begin(&client, GPU_OP_BEGIN_COMMAND_BUFFER);
			vulkan_write_u64(&client, command);
			vulkan_write_u64(&client, 1);
			vulkan_encode_VkCommandBufferBeginInfo(&client, &begin);
			t0 = bench_now();
			error = frame_run(session, &client);
			t1 = bench_now();
			if (frame != 0)
				reset_ms += t1 - t0;

			/* The recording. */
			frame_begin(&client);
			record_begin(&client, GPU_OP_CMD_BEGIN_RENDER_PASS, command);
			vulkan_write_u64(&client, 1);
			vulkan_write_u32(&client, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
			vulkan_write_u64(&client, 0);
			vulkan_write_u64(&client, 0x50f);
			vulkan_write_u64(&client, 0x510);
			memset(&rect, 0, sizeof(rect));
			rect.extent.width = 64;
			rect.extent.height = 64;
			vulkan_encode_VkRect2D(&client, &rect);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 2);
			vulkan_write_u64(&client, 4);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 0x3f000000);
			vulkan_write_u32(&client, 0x3f800000);
			vulkan_write_u32(&client, VK_SUBPASS_CONTENTS_INLINE);
			record_begin(&client, GPU_OP_CMD_SET_VIEWPORT, command);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			memset(&vp, 0, sizeof(vp));
			vp.width = 64.0f;
			vp.height = 64.0f;
			vp.maxDepth = 1.0f;
			vulkan_encode_VkViewport(&client, &vp);
			record_begin(&client, GPU_OP_CMD_SET_SCISSOR, command);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			vulkan_encode_VkRect2D(&client, &rect);
			record_begin(&client, GPU_OP_CMD_BIND_VERTEX_BUFFERS, command);
			vulkan_write_u32(&client, 0);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			vulkan_write_u64(&client, 0x504);
			vulkan_write_u64(&client, 1);
			vulkan_write_u64(&client, 0);
			record_begin(&client, GPU_OP_CMD_BIND_INDEX_BUFFER, command);
			vulkan_write_u64(&client, 0x505);
			vulkan_write_u64(&client, 0);
			vulkan_write_u32(&client, VK_INDEX_TYPE_UINT32);
			bound = 2;
			for (copy = first; copy < last; copy++) {
				for (group = 0; group < 16; group++) {
					kind = (group % 5 == 3) ? 1U : 0U;
					if (kind != bound) {
						bound = kind;
						record_begin(&client, GPU_OP_CMD_BIND_PIPELINE, command);
						vulkan_write_u32(&client, VK_PIPELINE_BIND_POINT_GRAPHICS);
						vulkan_write_u64(&client, kind != 0 ? 0x516 : 0x511);
					}
					record_begin(&client, GPU_OP_CMD_BIND_DESCRIPTOR_SETS, command);
					vulkan_write_u32(&client, VK_PIPELINE_BIND_POINT_GRAPHICS);
					vulkan_write_u64(&client, 0x50c);
					vulkan_write_u32(&client, 0);
					vulkan_write_u32(&client, 1);
					vulkan_write_u64(&client, 1);
					vulkan_write_u64(&client, group < 12 ? 0x560 + group : ((group % 2 == 0) ? 0x51c : 0x50e));
					vulkan_write_u32(&client, 0);
					vulkan_write_u64(&client, 0);
					push[0] = 0xbf800000;
					push[1] = 0xbf800000;
					push[2] = 0x3f800000;
					push[3] = 0x3f800000;
					push[4] = 0;
					push[5] = 0;
					push[6] = 0x3f800000 + group;
					push[7] = 0x3f800000 + (group & 3U);
					record_begin(&client, GPU_OP_CMD_PUSH_CONSTANTS, command);
					vulkan_write_u64(&client, 0x50c);
					vulkan_write_u32(&client, VK_SHADER_STAGE_VERTEX_BIT);
					vulkan_write_u32(&client, 0);
					vulkan_write_u32(&client, sizeof(push));
					vulkan_write_u64(&client, sizeof(push));
					vulkan_write_bytes(&client, push, sizeof(push));
					record_begin(&client, GPU_OP_CMD_DRAW_INDEXED, command);
					vulkan_write_u32(&client, 6);
					vulkan_write_u32(&client, 1);
					vulkan_write_u32(&client, 0);
					vulkan_write_u32(&client, 0);
					vulkan_write_u32(&client, copy);
				}
			}
			record_begin(&client, GPU_OP_CMD_END_RENDER_PASS, command);
			command_begin(&client, GPU_OP_END_COMMAND_BUFFER);
			vulkan_write_u64(&client, command);
			t0 = bench_now();
			error = frame_run(session, &client);
			t1 = bench_now();
			if (frame != 0)
				record_ms += t1 - t0;
			if (error != 0) {
				printf("bench: recording %d\n", error);
				return;
			}

			/* The submission. */
			frame_begin(&client);
			command_begin(&client, GPU_OP_QUEUE_SUBMIT);
			vulkan_write_u64(&client, 0x103);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			vulkan_write_u32(&client, VK_STRUCTURE_TYPE_SUBMIT_INFO);
			vulkan_write_u64(&client, 0);
			vulkan_write_u32(&client, 0);
			vulkan_write_u64(&client, 0);
			vulkan_write_u64(&client, 0);
			vulkan_write_u32(&client, 1);
			vulkan_write_u64(&client, 1);
			vulkan_write_u64(&client, command);
			vulkan_write_u32(&client, 0);
			vulkan_write_u64(&client, 0);
			vulkan_write_u64(&client, 0);
			method_count = 0;
			t0 = bench_now();
			error = frame_run(session, &client);
			t1 = bench_now();
			if (frame != 0)
				submit_ms += t1 - t0;
			if (error != 0) {
				printf("bench: submit %d\n", error);
				return;
			}
		}
	}
	if (frames < 2)
		frames = 2;
	printf("bench copies %u submits %u frames %u: reset %.3f ms, record %.3f ms, submit %.3f ms a frame (the first not counted); runs %u, snapshots %zu\n",
	    copies, submits, frames, reset_ms / (frames - 1), record_ms / (frames - 1), submit_ms / (frames - 1), gpu_runs, sim_snapshot_count);
	if (path != NULL) {
		file = fopen(path, "w");
		if (file != NULL) {
			for (index = 0; index < sim_snapshot_count; index++)
				fprintf(file, "%016llx\n", (unsigned long long)sim_snapshots[index]);
			fclose(file);
		}
	}
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc >= 6) {
		bench_draws(argv[1], (unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), argv[5][0] != 0 ? argv[5] : NULL);
		return 0;
	}
	if (argc != 2) {
		printf("usage: vk-d3-test spirv-directory\n");
		return 2;
	}
	test_memory();
	test_views();
	test_pipeline(argv[1]);
	test_submit(argv[1]);
	check(methods_with(0x0000, 0xc597U, 0) >= 1, "submit SM75: the push buffers bind Turing's 3D class");

	/* The same commands for Ampere (SM86) and Ada (SM89): their shader model and their 3D class. */
	test_shader_model = 86U;
	test_threed_class = 0xc797U;
	test_submit(argv[1]);
	check(methods_with(0x0000, 0xc797U, 0) >= 1 && methods_with(0x0000, 0xc597U, 0) == 0, "submit SM86: the push buffers bind Ampere's 3D class");
	test_shader_model = 89U;
	test_threed_class = 0xc997U;
	test_submit(argv[1]);
	check(methods_with(0x0000, 0xc997U, 0) >= 1 && methods_with(0x0000, 0xc797U, 0) == 0, "submit SM89: the push buffers bind Ada's 3D class");
	test_shader_model = 75U;
	test_threed_class = 0xc597U;
	test_compositor_shaders();
	printf("%s nvrtx vk d3: %u checks, %u failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
	return failures == 0 ? 0 : 1;
}

static void
check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Starts a frame: the reply stream is the reply blob from TEST_REPLY_AT. */
static void
frame_begin(struct vulkan_writer *writer)
{
	vulkan_writer_init(writer);
	memset(reply_words, 0, sizeof(reply_words));
	vulkan_write_u32(writer, GPU_OP_SET_REPLY_STREAM);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, TEST_REPLY_BLOB);
	vulkan_write_u64(writer, TEST_REPLY_AT);
	vulkan_write_u64(writer, sizeof(reply_words) - TEST_REPLY_AT);
}

/* Runs the frame through the transport and the real dispatcher. */
static int
frame_run(struct nvrtx_vk_session *session, struct vulkan_writer *writer)
{
	int error;

	error = nvrtx_vk_stream_execute(session, writer->data, (uint32_t)writer->bytes, nvrtx_vk_dispatch);
	vulkan_writer_finish(writer);
	return error;
}

static uint32_t
reply_u32(unsigned *at)
{
	uint32_t word;

	word = reply_words[*at];
	(*at)++;
	return word;
}

static uint64_t
reply_u64(unsigned *at)
{
	uint64_t low;
	uint64_t high;

	low = reply_u32(at);
	high = reply_u32(at);
	return low | (high << 32);
}

/* A command's header as vulkan_command_begin() writes it, appended to the frame (that one starts the writer over). */
static void
command_begin(struct vulkan_writer *writer, uint32_t opcode)
{
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, GPU_OP_FLAG_REPLY);
}

/* The create's tail (vulkan_object_create_complete): no allocator, the output present, the identity. */
static void
write_tail(struct vulkan_writer *writer, uint64_t identity)
{
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, identity);
}

/* vkAllocateMemory as memory.c writes it, with an import when imported is nonzero. */
static void
write_allocate(struct vulkan_writer *writer, uint64_t identity, uint64_t bytes, uint32_t imported)
{
	command_begin(writer, GPU_OP_ALLOCATE_MEMORY);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, 1);
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	if (imported != 0U) {
		vulkan_write_u64(writer, 1);
		vulkan_write_u32(writer, GPU_OP_RECORD_IMPORT_RESOURCE);
		vulkan_write_u64(writer, 0);
		vulkan_write_u32(writer, imported);
	} else {
		vulkan_write_u64(writer, 0);
	}
	vulkan_write_u64(writer, bytes);
	vulkan_write_u32(writer, 0);
	write_tail(writer, identity);
}

/* vkCreateImage as resources-generated.inc writes it. */
static void
write_image(struct vulkan_writer *writer, uint64_t identity, uint32_t format, uint32_t width, uint32_t height, uint32_t levels)
{
	VkImageCreateInfo info;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = (VkFormat)format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	command_begin(writer, GPU_OP_CREATE_IMAGE);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, 1);
	vulkan_encode_VkImageCreateInfo(writer, &info);
	write_tail(writer, identity);
}

/* vkCreateBuffer as resources-generated.inc writes it. */
static void
write_buffer(struct vulkan_writer *writer, uint64_t identity, uint64_t bytes)
{
	VkBufferCreateInfo info;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = bytes;
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	command_begin(writer, GPU_OP_CREATE_BUFFER);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, 1);
	vulkan_encode_VkBufferCreateInfo(writer, &info);
	write_tail(writer, identity);
}

/* A command of [device][first][second]: a destroy (second the allocator) or a requirements query (second the output). */
static void
write_pair(struct vulkan_writer *writer, uint32_t opcode, uint64_t first, uint64_t second)
{
	command_begin(writer, opcode);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, first);
	vulkan_write_u64(writer, second);
}

/* vkBindBufferMemory and vkBindImageMemory: [device][resource][memory][offset]. */
static void
write_bind(struct vulkan_writer *writer, uint32_t opcode, uint64_t resource, uint64_t memory, uint64_t offset)
{
	command_begin(writer, opcode);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, resource);
	vulkan_write_u64(writer, memory);
	vulkan_write_u64(writer, offset);
}

/*
 * Memory, its attached storage, images and buffers bound to it, their
 * requirements and layout, and the storage going back once the last
 * holder lets go.
 */
static void
test_memory(void)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct nvrtx_vk_image *image;
	struct nvrtx_vk_buffer *buffer;
	uint64_t va;
	uint8_t *cpu;
	unsigned before;
	unsigned at;
	int error;

	before = allocations;
	check(nvrtx_vk_session_open(NULL, &session) == 0, "memory: session opened");

	/* An allocation, an image of 100x10 (rows of 512 bytes), its requirements, a buffer of 1000 bytes and its requirements. */
	frame_begin(&client);
	write_allocate(&client, 0x200, 0x10000, 0);
	write_image(&client, 0x201, VK_FORMAT_B8G8R8A8_UNORM, 100, 10, 1);
	write_pair(&client, GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS, 0x201, 1);
	write_buffer(&client, 0x202, 1000);
	write_pair(&client, GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS, 0x202, 1);
	write_image(&client, 0x203, VK_FORMAT_B8G8R8A8_UNORM, 100, 10, 8);
	write_image(&client, 0x204, VK_FORMAT_R16G16B16A16_SFLOAT, 100, 10, 1);
	error = frame_run(session, &client);
	check(error == 0, "memory: the creates ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_ALLOCATE_MEMORY && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x200,
	    "memory: allocation reply");
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x201,
	    "memory: image reply");
	check(reply_u32(&at) == GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS && reply_u64(&at) == 1 && reply_u64(&at) == 5120 &&
	    reply_u64(&at) == 256 && reply_u32(&at) == 1, "memory: image requirements (rows of 512, 256 aligned)");
	check(reply_u32(&at) == GPU_OP_CREATE_BUFFER && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x202,
	    "memory: buffer reply");
	check(reply_u32(&at) == GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS && reply_u64(&at) == 1 && reply_u64(&at) == 1024 &&
	    reply_u64(&at) == 256 && reply_u32(&at) == 1, "memory: buffer requirements (rounded to 256)");
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE && reply_u32(&at) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT,
	    "memory: an image of more levels than halving its sides gives (8 of 100x10) is refused by name");
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE && reply_u32(&at) == (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT,
	    "memory: an image of a float format is refused by name");
	image = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE, 0x201);
	check(image != NULL && image->pitch == 512 && image->bytes == 5120, "memory: the image's layout");

	/* The node attaches the blob's storage once, of the allocation's size. */
	check(nvrtx_vk_memory_attach(session, 0x299, 0x10000, storage, 0x30000000ULL, &backing_token) == EINVAL,
	    "memory: no attach to an unknown allocation");
	check(nvrtx_vk_memory_attach(session, 0x200, 0x8000, storage, 0x30000000ULL, &backing_token) == EINVAL,
	    "memory: no attach of another size");
	check(nvrtx_vk_memory_attach(session, 0x200, 0x10000, storage, 0x30000000ULL, &backing_token) == 0,
	    "memory: the blob's storage attached");
	check(nvrtx_vk_memory_attach(session, 0x200, 0x10000, storage, 0x30000000ULL, &backing_token) == EINVAL,
	    "memory: attached once");

	/* Binds: the image at 0, the buffer at 5120; again, off the boundary, past the end, and to unknown memory fail. */
	frame_begin(&client);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x201, 0x200, 0);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x201, 0x200, 0);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x202, 0x200, 100);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x202, 0x200, 0x10000 - 512);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x202, 0x299, 0);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x202, 0x200, 5120);
	command_begin(&client, GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 0x201);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_IMAGE_ASPECT_COLOR_BIT);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 1);
	command_begin(&client, GPU_OP_GET_DEVICE_MEMORY_COMMITMENT);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 0x200);
	vulkan_write_u64(&client, 1);
	error = frame_run(session, &client);
	check(error == 0, "memory: the binds ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_BIND_IMAGE_MEMORY && reply_u32(&at) == VK_SUCCESS, "memory: image bound");
	check(reply_u32(&at) == GPU_OP_BIND_IMAGE_MEMORY && reply_u32(&at) != VK_SUCCESS, "memory: an image is bound once");
	check(reply_u32(&at) == GPU_OP_BIND_BUFFER_MEMORY && reply_u32(&at) != VK_SUCCESS, "memory: off the boundary");
	check(reply_u32(&at) == GPU_OP_BIND_BUFFER_MEMORY && reply_u32(&at) != VK_SUCCESS, "memory: past the end");
	check(reply_u32(&at) == GPU_OP_BIND_BUFFER_MEMORY && reply_u32(&at) != VK_SUCCESS, "memory: unknown memory");
	check(reply_u32(&at) == GPU_OP_BIND_BUFFER_MEMORY && reply_u32(&at) == VK_SUCCESS, "memory: buffer bound");
	check(reply_u32(&at) == GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT && reply_u64(&at) == 1 && reply_u64(&at) == 0 &&
	    reply_u64(&at) == 5120 && reply_u64(&at) == 512, "memory: the image's layout (offset, size, row pitch)");
	at += 4U;
	check(reply_u32(&at) == GPU_OP_GET_DEVICE_MEMORY_COMMITMENT && reply_u64(&at) == 1 && reply_u64(&at) == 0x10000,
	    "memory: committed once attached");
	buffer = nvrtx_vk_payload(session, I915_VK_OBJ_BUFFER, 0x202);
	check(buffer != NULL && nvrtx_vk_buffer_address(buffer, &va, &cpu) == 0 && va == 0x30000000ULL + 5120 && cpu == storage + 5120,
	    "memory: the buffer's addresses");
	check(image != NULL && nvrtx_vk_image_address(image, &va, &cpu) == 0 && va == 0x30000000ULL && cpu == storage,
	    "memory: the image's addresses");

	/* The allocation freed first lives on while the image and the buffer hold it. */
	frame_begin(&client);
	write_pair(&client, GPU_OP_FREE_MEMORY, 0x200, 0);
	write_pair(&client, GPU_OP_DESTROY_IMAGE, 0x201, 0);
	error = frame_run(session, &client);
	check(error == 0 && detached == 0, "memory: freed memory still held by the buffer");
	frame_begin(&client);
	write_pair(&client, GPU_OP_DESTROY_BUFFER, 0x202, 0);
	error = frame_run(session, &client);
	check(error == 0 && detached == 1 && detached_backing == &backing_token, "memory: the storage goes back with the last holder");

	/* An import takes the blob's storage at once; an unknown resource is refused. */
	frame_begin(&client);
	write_allocate(&client, 0x210, 0x10000, 7);
	write_allocate(&client, 0x211, 0x10000, 9);
	error = frame_run(session, &client);
	check(error == 0, "memory: the imports ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_ALLOCATE_MEMORY && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x210,
	    "memory: import accepted");
	check(reply_u32(&at) == GPU_OP_ALLOCATE_MEMORY && reply_u32(&at) != VK_SUCCESS, "memory: import of an unknown resource refused");
	check(nvrtx_vk_memory_attach(session, 0x210, 0x10000, storage, 0x30000000ULL, &backing_token) == EINVAL,
	    "memory: an import has its storage already");

	/* Closing the session gives the import's storage back and frees everything. */
	check(nvrtx_vk_session_close(&session) == 0 && detached == 2 && detached_backing == &import_token, "memory: session closed");
	check(allocations == before, "memory: nothing leaked");
}

/* Views of images and samplers. */
static void
test_views(void)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct vulkan_object image;
	struct vulkan_object unknown;
	struct nvrtx_vk_view *view;
	VkImageViewCreateInfo info;
	VkSamplerCreateInfo sampler;
	unsigned before;
	unsigned at;
	int error;

	before = allocations;
	check(nvrtx_vk_session_open(NULL, &session) == 0, "views: session opened");
	memset(&image, 0, sizeof(image));
	image.wire_id = 0x301;
	memset(&unknown, 0, sizeof(unknown));
	unknown.wire_id = 0x399;

	/* An image, a view of it (blue and red swapped), a view of an unknown image, a sampler. */
	frame_begin(&client);
	write_image(&client, 0x301, VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = (VkImage)(uintptr_t)&image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.components.r = VK_COMPONENT_SWIZZLE_B;
	info.components.b = VK_COMPONENT_SWIZZLE_R;
	info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.layerCount = 1;
	command_begin(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkImageViewCreateInfo(&client, &info);
	write_tail(&client, 0x302);
	info.image = (VkImage)(uintptr_t)&unknown;
	command_begin(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkImageViewCreateInfo(&client, &info);
	write_tail(&client, 0x303);
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	command_begin(&client, GPU_OP_CREATE_SAMPLER);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkSamplerCreateInfo(&client, &sampler);
	write_tail(&client, 0x304);
	error = frame_run(session, &client);
	check(error == 0, "views: the creates ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE && reply_u32(&at) == VK_SUCCESS, "views: image");
	at += 4U;
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE_VIEW && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x302,
	    "views: view");
	check(reply_u32(&at) == GPU_OP_CREATE_IMAGE_VIEW && reply_u32(&at) != VK_SUCCESS, "views: view of an unknown image refused");
	check(reply_u32(&at) == GPU_OP_CREATE_SAMPLER && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x304,
	    "views: sampler");
	view = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE_VIEW, 0x302);
	check(view != NULL && view->swizzle[0] == VK_COMPONENT_SWIZZLE_B && view->swizzle[1] == VK_COMPONENT_SWIZZLE_G &&
	    view->swizzle[2] == VK_COMPONENT_SWIZZLE_R && view->swizzle[3] == VK_COMPONENT_SWIZZLE_A, "views: the swizzle, identity resolved");

	/* A depth image with a stencil (D24_UNORM_S8_UINT, a depth attachment only), a view of it, and a D32 view of it refused. */
	{
		struct vulkan_object stencil_image;
		VkImageCreateInfo depth_info;
		struct nvrtx_vk_image *depth_image;

		memset(&stencil_image, 0, sizeof(stencil_image));
		stencil_image.wire_id = 0x311;
		memset(&depth_info, 0, sizeof(depth_info));
		depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		depth_info.imageType = VK_IMAGE_TYPE_2D;
		depth_info.format = VK_FORMAT_D24_UNORM_S8_UINT;
		depth_info.extent.width = 100;
		depth_info.extent.height = 10;
		depth_info.extent.depth = 1;
		depth_info.mipLevels = 1;
		depth_info.arrayLayers = 1;
		depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
		depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		frame_begin(&client);
		command_begin(&client, GPU_OP_CREATE_IMAGE);
		vulkan_write_u64(&client, TEST_DEVICE);
		vulkan_write_u64(&client, 1);
		vulkan_encode_VkImageCreateInfo(&client, &depth_info);
		write_tail(&client, 0x311);
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = (VkImage)(uintptr_t)&stencil_image;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_D24_UNORM_S8_UINT;
		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		info.subresourceRange.levelCount = 1;
		info.subresourceRange.layerCount = 1;
		command_begin(&client, GPU_OP_CREATE_IMAGE_VIEW);
		vulkan_write_u64(&client, TEST_DEVICE);
		vulkan_write_u64(&client, 1);
		vulkan_encode_VkImageViewCreateInfo(&client, &info);
		write_tail(&client, 0x312);
		info.format = VK_FORMAT_D32_SFLOAT;
		command_begin(&client, GPU_OP_CREATE_IMAGE_VIEW);
		vulkan_write_u64(&client, TEST_DEVICE);
		vulkan_write_u64(&client, 1);
		vulkan_encode_VkImageViewCreateInfo(&client, &info);
		write_tail(&client, 0x313);
		error = frame_run(session, &client);
		check(error == 0, "views: the stencil creates ran");
		at = TEST_REPLY_AT / 4U;
		check(reply_u32(&at) == GPU_OP_CREATE_IMAGE && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x311,
		    "views: a D24_UNORM_S8_UINT image");
		check(reply_u32(&at) == GPU_OP_CREATE_IMAGE_VIEW && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x312,
		    "views: its depth and stencil view");
		check(reply_u32(&at) == GPU_OP_CREATE_IMAGE_VIEW && reply_u32(&at) != VK_SUCCESS, "views: a view in another depth format refused");
		depth_image = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE, 0x311);
		check(depth_image != NULL && depth_image->depth && depth_image->stencil && depth_image->pitch == 448 && depth_image->bytes == 8192U,
		    "views: the stencil image's layout (four bytes a pixel in whole GOBs, whole pages)");
		frame_begin(&client);
		write_pair(&client, GPU_OP_DESTROY_IMAGE_VIEW, 0x312, 0);
		write_pair(&client, GPU_OP_DESTROY_IMAGE, 0x311, 0);
		error = frame_run(session, &client);
		check(error == 0, "views: the stencil image and view destroyed");
	}

	/* The image destroyed first lives on in its view. */
	frame_begin(&client);
	write_pair(&client, GPU_OP_DESTROY_IMAGE, 0x301, 0);
	error = frame_run(session, &client);
	check(error == 0 && view->image != NULL && ((struct nvrtx_vk_image *)view->image->payload)->width == 64, "views: the view holds its image");
	frame_begin(&client);
	write_pair(&client, GPU_OP_DESTROY_IMAGE_VIEW, 0x302, 0);
	write_pair(&client, GPU_OP_DESTROY_SAMPLER, 0x304, 0);
	write_pair(&client, GPU_OP_DESTROY_SAMPLER, 0x399, 0);
	error = frame_run(session, &client);
	check(error == 0, "views: destroyed (an unknown identity too)");

	check(nvrtx_vk_session_close(&session) == 0, "views: session closed");
	check(allocations == before, "views: nothing leaked");
}

/* Reads a SPIR-V module glslc wrote into the directory. */
static uint32_t *
read_spirv(const char *directory, const char *name, size_t *bytes)
{
	char path[512];
	FILE *file;
	uint32_t *words;
	long length;

	snprintf(path, sizeof(path), "%s/%s.spv", directory, name);
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	words = malloc((size_t)length);
	if (words != NULL && fread(words, 1, (size_t)length, file) != (size_t)length) {
		free(words);
		words = NULL;
	}
	fclose(file);
	*bytes = (size_t)length;
	return words;
}

/* A create's head: [device][present]. */
static void
write_create(struct vulkan_writer *writer, uint32_t opcode)
{
	command_begin(writer, opcode);
	vulkan_write_u64(writer, TEST_DEVICE);
	vulkan_write_u64(writer, 1);
}

/*
 * The compositor's quad as its objects: shader modules of quad.vert and
 * quad.frag, a set layout of one combined image sampler, a pipeline layout
 * with push constants, a pool and a set updated with a view and a sampler,
 * a render pass and a framebuffer, and the graphics pipeline, compiled.
 */
static void
test_pipeline(const char *directory)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct vulkan_object handles[16];
	struct nvrtx_vk_pipeline *pipeline;
	struct nvrtx_vk_descriptor_set *set;
	struct nvrtx_vk_render_pass *pass;
	struct nvrtx_vk_framebuffer *framebuffer;
	VkShaderModuleCreateInfo module;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo pipeline_layout;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo render_pass;
	VkFramebufferCreateInfo framebuffer_info;
	VkImageView attachments[1];
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription vertex_attribute;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth_stencil;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamic_states[2];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDescriptorSetLayout layouts[1];
	uint32_t *vertex_words;
	uint32_t *fragment_words;
	size_t vertex_bytes;
	size_t fragment_bytes;
	unsigned before;
	unsigned at;
	unsigned index;
	int error;

	vertex_words = read_spirv(directory, "quad-vert", &vertex_bytes);
	fragment_words = read_spirv(directory, "quad-frag", &fragment_bytes);
	check(vertex_words != NULL && fragment_words != NULL, "pipeline: the quad's SPIR-V");
	if (vertex_words == NULL || fragment_words == NULL)
		return;

	before = allocations;
	check(nvrtx_vk_session_open(NULL, &session) == 0, "pipeline: session opened");
	memset(handles, 0, sizeof(handles));
	for (index = 0; index < 16; index++)
		handles[index].wire_id = 0x400 + index;

	/* 0x401 0x402 the modules, 0x403 the set layout, 0x404 the pipeline layout, 0x405 the pool, 0x406 the set. */
	frame_begin(&client);
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = vertex_bytes;
	module.pCode = vertex_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x401);
	module.codeSize = fragment_bytes;
	module.pCode = fragment_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x402);
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1;
	set_layout.pBindings = &binding;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&client, &set_layout);
	write_tail(&client, 0x403);
	layouts[0] = (VkDescriptorSetLayout)(uintptr_t)&handles[3];
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 32;
	memset(&pipeline_layout, 0, sizeof(pipeline_layout));
	pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout.setLayoutCount = 1;
	pipeline_layout.pSetLayouts = layouts;
	pipeline_layout.pushConstantRangeCount = 1;
	pipeline_layout.pPushConstantRanges = &range;
	write_create(&client, GPU_OP_CREATE_PIPELINE_LAYOUT);
	vulkan_encode_VkPipelineLayoutCreateInfo(&client, &pipeline_layout);
	write_tail(&client, 0x404);
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 4;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 4;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_POOL);
	vulkan_encode_VkDescriptorPoolCreateInfo(&client, &pool);
	write_tail(&client, 0x405);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = (VkDescriptorPool)(uintptr_t)&handles[5];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = layouts;
	command_begin(&client, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkDescriptorSetAllocateInfo(&client, &allocate);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x406);
	error = frame_run(session, &client);
	check(error == 0, "pipeline: modules, layouts, pool and set made");
	at = TEST_REPLY_AT / 4U;
	for (index = 1; index <= 5; index++) {
		at++;
		check(reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x400 + index, "pipeline: an object made");
	}
	check(reply_u32(&at) == GPU_OP_ALLOCATE_DESCRIPTOR_SETS && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x406,
	    "pipeline: the set allocated");

	/* 0x407 an image, 0x408 its view, 0x409 a sampler; the set's binding 0 takes the view and the sampler. */
	frame_begin(&client);
	write_image(&client, 0x407, VK_FORMAT_B8G8R8A8_UNORM, 64, 64, 1);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = (VkImage)(uintptr_t)&handles[7];
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x408);
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	write_create(&client, GPU_OP_CREATE_SAMPLER);
	vulkan_encode_VkSamplerCreateInfo(&client, &sampler);
	write_tail(&client, 0x409);
	command_begin(&client, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0x406);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u32(&client, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x409);
	vulkan_write_u64(&client, 0x408);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);

	/* 0x40a a render pass of one B8G8R8A8 attachment cleared, 0x40b a framebuffer of the view. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&reference, 0, sizeof(reference));
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	memset(&render_pass, 0, sizeof(render_pass));
	render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass.attachmentCount = 1;
	render_pass.pAttachments = &attachment;
	render_pass.subpassCount = 1;
	render_pass.pSubpasses = &subpass;
	write_create(&client, GPU_OP_CREATE_RENDER_PASS);
	vulkan_encode_VkRenderPassCreateInfo(&client, &render_pass);
	write_tail(&client, 0x40a);
	attachments[0] = (VkImageView)(uintptr_t)&handles[8];
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = (VkRenderPass)(uintptr_t)&handles[10];
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = attachments;
	framebuffer_info.width = 64;
	framebuffer_info.height = 64;
	framebuffer_info.layers = 1;
	write_create(&client, GPU_OP_CREATE_FRAMEBUFFER);
	vulkan_encode_VkFramebufferCreateInfo(&client, &framebuffer_info);
	write_tail(&client, 0x40b);
	error = frame_run(session, &client);
	check(error == 0, "pipeline: image, view, sampler, update, pass and framebuffer made");
	set = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, 0x406);
	check(set != NULL && set->slots[0].view != NULL && set->slots[0].view->identity == 0x408 && set->slots[0].sampler != NULL &&
	    set->slots[0].sampler->identity == 0x409, "pipeline: the set's binding 0 names the view and the sampler");
	pass = nvrtx_vk_payload(session, I915_VK_OBJ_RENDER_PASS, 0x40a);
	check(pass != NULL && pass->colour == 1 && pass->format == VK_FORMAT_B8G8R8A8_UNORM && pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR,
	    "pipeline: the render pass");
	framebuffer = nvrtx_vk_payload(session, I915_VK_OBJ_FRAMEBUFFER, 0x40b);
	check(framebuffer != NULL && framebuffer->width == 64 && framebuffer->view_count == 1 && framebuffer->views[0] != NULL && framebuffer->views[0]->identity == 0x408,
	    "pipeline: the framebuffer");

	/* 0x40c the graphics pipeline as pipeline_encode_graphics() writes it. */
	frame_begin(&client);
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = (VkShaderModule)(uintptr_t)&handles[1];
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = (VkShaderModule)(uintptr_t)&handles[2];
	stages[1].pName = "main";
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = 8;
	memset(&vertex_attribute, 0, sizeof(vertex_attribute));
	vertex_attribute.format = VK_FORMAT_R32G32_SFLOAT;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 1;
	vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorWriteMask = 0xf;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;
	command_begin(&client, GPU_OP_CREATE_GRAPHICS_PIPELINES);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 2);
	vulkan_write_u64(&client, 2);
	vulkan_encode_VkPipelineShaderStageCreateInfo(&client, &stages[0]);
	vulkan_encode_VkPipelineShaderStageCreateInfo(&client, &stages[1]);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineVertexInputStateCreateInfo(&client, &vertex_input);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(&client, &assembly);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineViewportStateCreateInfo(&client, &viewport);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineRasterizationStateCreateInfo(&client, &raster);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineMultisampleStateCreateInfo(&client, &multisample);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineColorBlendStateCreateInfo(&client, &blend);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineDynamicStateCreateInfo(&client, &dynamic);
	vulkan_write_u64(&client, 0x404);
	vulkan_write_u64(&client, 0x40a);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, (uint32_t)-1);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x40c);
	error = frame_run(session, &client);
	check(error == 0, "pipeline: the create ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_CREATE_GRAPHICS_PIPELINES && reply_u32(&at) == VK_SUCCESS && reply_u64(&at) == 1 && reply_u64(&at) == 0x40c,
	    "pipeline: compiled and published");
	pipeline = nvrtx_vk_payload(session, I915_VK_OBJ_PIPELINE, 0x40c);
	check(pipeline != NULL && pipeline->vertex.gprs != 0 && pipeline->fragment.gprs != 0 && pipeline->fragment.texture_count == 1 &&
	    pipeline->fragment.textures[0].set == 0 && pipeline->fragment.textures[0].binding == 0 && pipeline->vertex.texture_count == 0,
	    "pipeline: two programs, the fragment's texture from set 0 binding 0");
	check(pipeline != NULL && pipeline->fragment.va > pipeline->vertex.va && (pipeline->fragment.va % 256) == 0 && pipeline->push_bytes == 32,
	    "pipeline: the programs' places and the push bytes");
	check(pipeline != NULL && pipeline->binding_count == 1 && pipeline->bindings[0].stride == 8 && pipeline->attribute_count == 1 &&
	    pipeline->attributes[0].format == VK_FORMAT_R32G32_SFLOAT && pipeline->blend_enable == 1 &&
	    pipeline->blend_dst_colour == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && pipeline->dynamic_viewport == 1 && pipeline->dynamic_scissor == 1 &&
	    pipeline->write_mask == 0xf, "pipeline: the vertex input, blend and dynamic state");

	/* The same pipeline with a two-sided stencil test (a fill's winding count: the front faces add, the back ones take away). */
	memset(&depth_stencil, 0, sizeof(depth_stencil));
	depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil.stencilTestEnable = VK_TRUE;
	depth_stencil.front.failOp = VK_STENCIL_OP_KEEP;
	depth_stencil.front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
	depth_stencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
	depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
	depth_stencil.front.compareMask = 0x1ff;
	depth_stencil.front.writeMask = 0xff;
	depth_stencil.front.reference = 3;
	depth_stencil.back = depth_stencil.front;
	depth_stencil.back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
	depth_stencil.back.compareOp = VK_COMPARE_OP_NOT_EQUAL;
	frame_begin(&client);
	command_begin(&client, GPU_OP_CREATE_GRAPHICS_PIPELINES);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 2);
	vulkan_write_u64(&client, 2);
	vulkan_encode_VkPipelineShaderStageCreateInfo(&client, &stages[0]);
	vulkan_encode_VkPipelineShaderStageCreateInfo(&client, &stages[1]);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineVertexInputStateCreateInfo(&client, &vertex_input);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(&client, &assembly);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineViewportStateCreateInfo(&client, &viewport);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineRasterizationStateCreateInfo(&client, &raster);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineMultisampleStateCreateInfo(&client, &multisample);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineDepthStencilStateCreateInfo(&client, &depth_stencil);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineColorBlendStateCreateInfo(&client, &blend);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkPipelineDynamicStateCreateInfo(&client, &dynamic);
	vulkan_write_u64(&client, 0x404);
	vulkan_write_u64(&client, 0x40a);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, (uint32_t)-1);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x40d);
	error = frame_run(session, &client);
	at = TEST_REPLY_AT / 4U;
	check(error == 0 && reply_u32(&at) == GPU_OP_CREATE_GRAPHICS_PIPELINES && reply_u32(&at) == VK_SUCCESS,
	    "pipeline: a stencil test is taken");
	pipeline = nvrtx_vk_payload(session, I915_VK_OBJ_PIPELINE, 0x40d);
	check(pipeline != NULL && pipeline->stencil_test == 1 && pipeline->depth_test == 0 &&
	    pipeline->stencil_front.pass == VK_STENCIL_OP_INCREMENT_AND_WRAP && pipeline->stencil_front.compare == VK_COMPARE_OP_ALWAYS &&
	    pipeline->stencil_back.pass == VK_STENCIL_OP_DECREMENT_AND_WRAP && pipeline->stencil_back.compare == VK_COMPARE_OP_NOT_EQUAL,
	    "pipeline: both faces' operations and functions");
	check(pipeline != NULL && pipeline->stencil_front.compare_mask == 0xff && pipeline->stencil_front.write_mask == 0xff &&
	    pipeline->stencil_front.reference == 3 && pipeline->stencil_back.fail == VK_STENCIL_OP_KEEP,
	    "pipeline: the masks and reference, of 8 bits");

	/* The pool's destruction takes its set; closing frees everything. */
	frame_begin(&client);
	write_pair(&client, GPU_OP_DESTROY_DESCRIPTOR_POOL, 0x405, 0);
	error = frame_run(session, &client);
	check(error == 0 && nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, 0x406) == NULL, "pipeline: the pool took its set");
	check(nvrtx_vk_session_close(&session) == 0, "pipeline: session closed");
	check(allocations == before, "pipeline: nothing leaked");
	free(vertex_words);
	free(fragment_words);
}

/* A recorded command's header as libvulkan's command_record_begin() writes it: no reply asked for. */
static void
record_begin(struct vulkan_writer *writer, uint32_t opcode, uint64_t command)
{
	vulkan_write_u32(writer, opcode);
	vulkan_write_u32(writer, 0);
	vulkan_write_u64(writer, command);
}

/* The compositor's quad pipeline as pipeline_encode_graphics() writes it: modules handles[vertex] and handles[fragment]. */
static void
write_quad_pipeline(struct vulkan_writer *client, struct vulkan_object *handles, uint64_t identity, unsigned vertex, unsigned fragment, uint64_t layout, uint64_t pass)
{
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription vertex_attribute;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamic_states[2];
	VkPipelineDynamicStateCreateInfo dynamic;

	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = (VkShaderModule)(uintptr_t)&handles[vertex];
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = (VkShaderModule)(uintptr_t)&handles[fragment];
	stages[1].pName = "main";
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = 8;
	memset(&vertex_attribute, 0, sizeof(vertex_attribute));
	vertex_attribute.format = VK_FORMAT_R32G32_SFLOAT;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 1;
	vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorWriteMask = 0xf;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;
	command_begin(client, GPU_OP_CREATE_GRAPHICS_PIPELINES);
	vulkan_write_u64(client, TEST_DEVICE);
	vulkan_write_u64(client, 0);
	vulkan_write_u32(client, 1);
	vulkan_write_u64(client, 1);
	vulkan_write_u32(client, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	vulkan_write_u64(client, 0);
	vulkan_write_u32(client, 0);
	vulkan_write_u32(client, 2);
	vulkan_write_u64(client, 2);
	vulkan_encode_VkPipelineShaderStageCreateInfo(client, &stages[0]);
	vulkan_encode_VkPipelineShaderStageCreateInfo(client, &stages[1]);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineVertexInputStateCreateInfo(client, &vertex_input);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(client, &assembly);
	vulkan_write_u64(client, 0);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineViewportStateCreateInfo(client, &viewport);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineRasterizationStateCreateInfo(client, &raster);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineMultisampleStateCreateInfo(client, &multisample);
	vulkan_write_u64(client, 0);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineColorBlendStateCreateInfo(client, &blend);
	vulkan_write_u64(client, 1);
	vulkan_encode_VkPipelineDynamicStateCreateInfo(client, &dynamic);
	vulkan_write_u64(client, layout);
	vulkan_write_u64(client, pass);
	vulkan_write_u32(client, 0);
	vulkan_write_u64(client, 0);
	vulkan_write_u32(client, (uint32_t)-1);
	vulkan_write_u64(client, 0);
	vulkan_write_u64(client, 1);
	vulkan_write_u64(client, identity);
}

/*
 * A command buffer as the compositor records one, submitted: a pass that
 * clears and draws the textured quad, an image copy, a copy from a buffer
 * drawn and one done by the CPU, a clear; the push buffers checked.
 */
static void
test_submit(const char *directory)
{
	struct nvrtx_vk_session *session;
	struct vulkan_writer client;
	struct vulkan_object handles[32];
	struct nvrtx_vk_pipeline *pipeline;
	VkShaderModuleCreateInfo module;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo pipeline_layout;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo render_pass;
	VkFramebufferCreateInfo framebuffer_info;
	VkCommandPoolCreateInfo command_pool;
	VkCommandBufferAllocateInfo command_allocate;
	VkCommandBufferBeginInfo begin;
	VkViewport vp;
	VkRect2D rect;
	VkImageCopy copy;
	VkBufferImageCopy upload;
	VkBufferImageCopy skewed;
	VkImageSubresourceRange whole;
	VkImageView attachments[1];
	VkDescriptorSetLayout layouts[1];
	uint32_t *vertex_words;
	uint32_t *fragment_words;
	uint32_t corners[12] = { 0, 0, 0x3f800000, 0, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000 };
	uint32_t push[8] = { 0xbf800000, 0xbf800000, 0x3f800000, 0x3f800000, 0, 0, 0x3f800000, 0x3f800000 };
	const uint8_t *tic;
	uint8_t *constants;
	uint64_t execute_va;
	size_t vertex_bytes;
	size_t fragment_bytes;
	unsigned before;
	unsigned at;
	unsigned index;
	uint32_t word;
	int error;

	vertex_words = read_spirv(directory, "quad-vert", &vertex_bytes);
	fragment_words = read_spirv(directory, "quad-frag", &fragment_bytes);
	if (vertex_words == NULL || fragment_words == NULL) {
		check(0, "submit: the quad's SPIR-V");
		return;
	}

	before = allocations;
	check(nvrtx_vk_session_open(NULL, &session) == 0, "submit: session opened");
	memset(handles, 0, sizeof(handles));
	for (index = 0; index < 32; index++)
		handles[index].wire_id = 0x500 + index;

	/* Memory 0x501 at 0x30000000; images 0x502 (64x64 BGRA, rows 256) and 0x503 (32x32 RGBA); buffers 0x504 (corners) and 0x505 (staging). */
	frame_begin(&client);
	write_allocate(&client, 0x501, 0x40000, 0);
	write_image(&client, 0x502, VK_FORMAT_B8G8R8A8_UNORM, 64, 64, 1);
	write_image(&client, 0x503, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1);
	write_buffer(&client, 0x504, sizeof(corners));
	write_buffer(&client, 0x505, 0x4000);
	error = frame_run(session, &client);
	check(error == 0, "submit: memory, images and buffers made");
	check(nvrtx_vk_memory_attach(session, 0x501, 0x40000, scene, 0x30000000ULL, &backing_token) == 0, "submit: storage attached");
	frame_begin(&client);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x502, 0x501, 0);
	write_bind(&client, GPU_OP_BIND_IMAGE_MEMORY, 0x503, 0x501, 0x4000);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x504, 0x501, 0x6000);
	write_bind(&client, GPU_OP_BIND_BUFFER_MEMORY, 0x505, 0x501, 0x8000);
	error = frame_run(session, &client);
	check(error == 0, "submit: bound");
	memcpy(scene + 0x6000, corners, sizeof(corners));

	/* Views 0x506 (of 0x502) and 0x507 (of 0x503), sampler 0x508, modules 0x509 0x50a, layouts 0x50b 0x50c, pool 0x50d, set 0x50e. */
	frame_begin(&client);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	view.image = (VkImage)(uintptr_t)&handles[2];
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x506);
	view.image = (VkImage)(uintptr_t)&handles[3];
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	write_create(&client, GPU_OP_CREATE_IMAGE_VIEW);
	vulkan_encode_VkImageViewCreateInfo(&client, &view);
	write_tail(&client, 0x507);
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	write_create(&client, GPU_OP_CREATE_SAMPLER);
	vulkan_encode_VkSamplerCreateInfo(&client, &sampler);
	write_tail(&client, 0x508);
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = vertex_bytes;
	module.pCode = vertex_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x509);
	module.codeSize = fragment_bytes;
	module.pCode = fragment_words;
	write_create(&client, GPU_OP_CREATE_SHADER_MODULE);
	vulkan_encode_VkShaderModuleCreateInfo(&client, &module);
	write_tail(&client, 0x50a);
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1;
	set_layout.pBindings = &binding;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&client, &set_layout);
	write_tail(&client, 0x50b);
	layouts[0] = (VkDescriptorSetLayout)(uintptr_t)&handles[11];
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 32;
	memset(&pipeline_layout, 0, sizeof(pipeline_layout));
	pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout.setLayoutCount = 1;
	pipeline_layout.pSetLayouts = layouts;
	pipeline_layout.pushConstantRangeCount = 1;
	pipeline_layout.pPushConstantRanges = &range;
	write_create(&client, GPU_OP_CREATE_PIPELINE_LAYOUT);
	vulkan_encode_VkPipelineLayoutCreateInfo(&client, &pipeline_layout);
	write_tail(&client, 0x50c);
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 1;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	write_create(&client, GPU_OP_CREATE_DESCRIPTOR_POOL);
	vulkan_encode_VkDescriptorPoolCreateInfo(&client, &pool);
	write_tail(&client, 0x50d);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = (VkDescriptorPool)(uintptr_t)&handles[13];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = layouts;
	command_begin(&client, GPU_OP_ALLOCATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkDescriptorSetAllocateInfo(&client, &allocate);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x50e);
	command_begin(&client, GPU_OP_UPDATE_DESCRIPTOR_SETS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0x50e);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u32(&client, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x508);
	vulkan_write_u64(&client, 0x507);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);

	/* Render pass 0x50f (clear), framebuffer 0x510 of 0x506, pipeline 0x511, command pool 0x512 and buffer 0x513, fence 0x514. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	memset(&reference, 0, sizeof(reference));
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	memset(&render_pass, 0, sizeof(render_pass));
	render_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass.attachmentCount = 1;
	render_pass.pAttachments = &attachment;
	render_pass.subpassCount = 1;
	render_pass.pSubpasses = &subpass;
	write_create(&client, GPU_OP_CREATE_RENDER_PASS);
	vulkan_encode_VkRenderPassCreateInfo(&client, &render_pass);
	write_tail(&client, 0x50f);
	attachments[0] = (VkImageView)(uintptr_t)&handles[6];
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = (VkRenderPass)(uintptr_t)&handles[15];
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = attachments;
	framebuffer_info.width = 64;
	framebuffer_info.height = 64;
	framebuffer_info.layers = 1;
	write_create(&client, GPU_OP_CREATE_FRAMEBUFFER);
	vulkan_encode_VkFramebufferCreateInfo(&client, &framebuffer_info);
	write_tail(&client, 0x510);
	write_quad_pipeline(&client, handles, 0x511, 9, 10, 0x50c, 0x50f);
	memset(&command_pool, 0, sizeof(command_pool));
	command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	write_create(&client, GPU_OP_CREATE_COMMAND_POOL);
	vulkan_encode_VkCommandPoolCreateInfo(&client, &command_pool);
	write_tail(&client, 0x512);
	memset(&command_allocate, 0, sizeof(command_allocate));
	command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_allocate.commandPool = (VkCommandPool)(uintptr_t)&handles[18];
	command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_allocate.commandBufferCount = 1;
	command_begin(&client, GPU_OP_ALLOCATE_COMMAND_BUFFERS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkCommandBufferAllocateInfo(&client, &command_allocate);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x513);
	command_begin(&client, GPU_OP_CREATE_FENCE);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	write_tail(&client, 0x514);
	error = frame_run(session, &client);
	check(error == 0, "submit: the scene's objects made");
	pipeline = nvrtx_vk_payload(session, I915_VK_OBJ_PIPELINE, 0x511);
	check(pipeline != NULL && nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, 0x513) != NULL &&
	    nvrtx_vk_payload(session, I915_VK_OBJ_FENCE, 0x514) != NULL && nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, 0x50e) != NULL,
	    "submit: pipeline, command buffer, fence and set exist");
	if (pipeline == NULL)
		return;

	/* The recording: Begin, the pass with its draw, a barrier, the copies and the clear, End. */
	frame_begin(&client);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	command_begin(&client, GPU_OP_BEGIN_COMMAND_BUFFER);
	vulkan_write_u64(&client, 0x513);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkCommandBufferBeginInfo(&client, &begin);
	record_begin(&client, GPU_OP_CMD_BEGIN_RENDER_PASS, 0x513);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0x50f);
	vulkan_write_u64(&client, 0x510);
	memset(&rect, 0, sizeof(rect));
	rect.extent.width = 64;
	rect.extent.height = 64;
	vulkan_encode_VkRect2D(&client, &rect);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 2);
	vulkan_write_u64(&client, 4);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0x3f000000);
	vulkan_write_u32(&client, 0x3f800000);
	vulkan_write_u32(&client, VK_SUBPASS_CONTENTS_INLINE);
	record_begin(&client, GPU_OP_CMD_BIND_PIPELINE, 0x513);
	vulkan_write_u32(&client, VK_PIPELINE_BIND_POINT_GRAPHICS);
	vulkan_write_u64(&client, 0x511);
	record_begin(&client, GPU_OP_CMD_SET_VIEWPORT, 0x513);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	memset(&vp, 0, sizeof(vp));
	vp.width = 64.0f;
	vp.height = 64.0f;
	vp.maxDepth = 1.0f;
	vulkan_encode_VkViewport(&client, &vp);
	record_begin(&client, GPU_OP_CMD_SET_SCISSOR, 0x513);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkRect2D(&client, &rect);
	record_begin(&client, GPU_OP_CMD_BIND_DESCRIPTOR_SETS, 0x513);
	vulkan_write_u32(&client, VK_PIPELINE_BIND_POINT_GRAPHICS);
	vulkan_write_u64(&client, 0x50c);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x50e);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	record_begin(&client, GPU_OP_CMD_BIND_VERTEX_BUFFERS, 0x513);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x504);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0);
	record_begin(&client, GPU_OP_CMD_PUSH_CONSTANTS, 0x513);
	vulkan_write_u64(&client, 0x50c);
	vulkan_write_u32(&client, VK_SHADER_STAGE_VERTEX_BIT);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, sizeof(push));
	vulkan_write_u64(&client, sizeof(push));
	vulkan_write_bytes(&client, push, sizeof(push));
	record_begin(&client, GPU_OP_CMD_DRAW, 0x513);
	vulkan_write_u32(&client, 6);
	vulkan_write_u32(&client, 1);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	record_begin(&client, GPU_OP_CMD_END_RENDER_PASS, 0x513);
	record_begin(&client, GPU_OP_CMD_PIPELINE_BARRIER, 0x513);
	vulkan_write_u32(&client, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	vulkan_write_u32(&client, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = 1;
	copy.dstSubresource = copy.srcSubresource;
	copy.dstOffset.x = 10;
	copy.dstOffset.y = 20;
	copy.extent.width = 16;
	copy.extent.height = 8;
	copy.extent.depth = 1;
	record_begin(&client, GPU_OP_CMD_COPY_IMAGE, 0x513);
	vulkan_write_u64(&client, 0x503);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u64(&client, 0x502);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkImageCopy(&client, &copy);
	memset(&upload, 0, sizeof(upload));
	upload.bufferRowLength = 32;
	upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	upload.imageSubresource.layerCount = 1;
	upload.imageExtent.width = 32;
	upload.imageExtent.height = 8;
	upload.imageExtent.depth = 1;
	record_begin(&client, GPU_OP_CMD_COPY_BUFFER_TO_IMAGE, 0x513);
	vulkan_write_u64(&client, 0x505);
	vulkan_write_u64(&client, 0x502);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkBufferImageCopy(&client, &upload);
	/*
	 * Two regions: 4x3 pixels from offset 0x44 with rows of 64 bytes (drawn:
	 * the texture from 0x40, one pixel in), then 3x2 pixels from offset 4
	 * with rows of 12 bytes (copied by the CPU).
	 */
	skewed = upload;
	skewed.bufferOffset = 0x44;
	skewed.bufferRowLength = 16;
	skewed.imageOffset.x = 10;
	skewed.imageOffset.y = 50;
	skewed.imageExtent.width = 4;
	skewed.imageExtent.height = 3;
	upload.bufferOffset = 4;
	upload.bufferRowLength = 3;
	upload.imageOffset.x = 5;
	upload.imageOffset.y = 40;
	upload.imageExtent.width = 3;
	upload.imageExtent.height = 2;
	record_begin(&client, GPU_OP_CMD_COPY_BUFFER_TO_IMAGE, 0x513);
	vulkan_write_u64(&client, 0x505);
	vulkan_write_u64(&client, 0x502);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u32(&client, 2);
	vulkan_write_u64(&client, 2);
	vulkan_encode_VkBufferImageCopy(&client, &skewed);
	vulkan_encode_VkBufferImageCopy(&client, &upload);
	memset(&whole, 0, sizeof(whole));
	whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	whole.levelCount = 1;
	whole.layerCount = 1;
	record_begin(&client, GPU_OP_CMD_CLEAR_COLOR_IMAGE, 0x513);
	vulkan_write_u64(&client, 0x503);
	vulkan_write_u32(&client, VK_IMAGE_LAYOUT_GENERAL);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, 2);
	vulkan_write_u64(&client, 4);
	vulkan_write_u32(&client, 0x3f800000);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u32(&client, 0x3f800000);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_encode_VkImageSubresourceRange(&client, &whole);
	command_begin(&client, GPU_OP_END_COMMAND_BUFFER);
	vulkan_write_u64(&client, 0x513);
	error = frame_run(session, &client);
	check(error == 0, "submit: the recording ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_BEGIN_COMMAND_BUFFER && reply_u32(&at) == VK_SUCCESS, "submit: begun");
	check(reply_u32(&at) == GPU_OP_END_COMMAND_BUFFER && reply_u32(&at) == VK_SUCCESS, "submit: ended executable");
	check(((struct nvrtx_vk_command_buffer *)nvrtx_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, 0x513))->record_count == 13,
	    "submit: thirteen records kept (the barrier is not)");

	/* The staging buffer's bytes the CPU copies: 3x2 pixels from offset 4, rows of 12 bytes. */
	for (index = 0; index < 24; index++)
		scene[0x8000 + 4 + index] = (uint8_t)(0x40 + index);

	/* The submission with the fence. */
	gpu_runs = 0;
	gpu_draws = 0;
	gpu_quads = 0;
	gpu_copies = 0;
	gpu_copied_bytes = 0;
	method_count = 0;
	frame_begin(&client);
	command_begin(&client, GPU_OP_QUEUE_SUBMIT);
	vulkan_write_u64(&client, 0x103);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u32(&client, VK_STRUCTURE_TYPE_SUBMIT_INFO);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u32(&client, 1);
	vulkan_write_u64(&client, 1);
	vulkan_write_u64(&client, 0x513);
	vulkan_write_u32(&client, 0);
	vulkan_write_u64(&client, 0);
	vulkan_write_u64(&client, 0x514);
	command_begin(&client, GPU_OP_GET_FENCE_STATUS);
	vulkan_write_u64(&client, TEST_DEVICE);
	vulkan_write_u64(&client, 0x514);
	error = frame_run(session, &client);
	check(error == 0, "submit: the submission ran");
	at = TEST_REPLY_AT / 4U;
	check(reply_u32(&at) == GPU_OP_QUEUE_SUBMIT && reply_u32(&at) == VK_SUCCESS, "submit: success");
	check(reply_u32(&at) == GPU_OP_GET_FENCE_STATUS && reply_u32(&at) == VK_SUCCESS, "submit: the fence is signaled");
	check(gpu_runs == 2, "submit: two runs (the CPU copy between them)");

	/* The CPU copy: image 0x502 (at 0, rows of 256) at (5, 40). */
	check(memcmp(scene + 40 * 256 + 5 * 4, scene + 0x8004, 12) == 0 && memcmp(scene + 41 * 256 + 5 * 4, scene + 0x8004 + 12, 12) == 0,
	    "submit: the unaligned region copied by the CPU");
	check(gpu_copies == 1 && gpu_copied_bytes == 24, "submit: the CPU's copy counted with its 24 bytes");

	/* The pass's target: image 0x502 at 0x30000000, pitch 256, A8R8G8B8; cleared, and the image 0x503 cleared too. */
	check(methods_with(0x0804, 0x30000000U, 0) >= 1 && methods_with(0x0808, 256, 0) >= 1 && methods_with(0x0810, 0xcf, 0) >= 1,
	    "submit: colour target 0 is the pass's image");
	check(methods_with(0x0804, 0x30004000U, 0) >= 1 && methods_with(0x0810, 0xd5, 0) >= 1, "submit: the cleared RGBA image is a target");
	check(methods_with(0x19d0, 0x3c, 0) == 2, "submit: two clears");
	check(methods_with(0x0d88, 0x3f000000U, 0) == 1, "submit: the pass's clear colour");

	/* Four draws of six vertices: the quad, the image copy and the two buffer uploads drawn. */
	check(methods_with(0x1618, 0, 1) == 4 && methods_with(0x0d78, 6, 0) == 4, "submit: four draws of six vertices");
	check(methods_with(0x2014 + 0x40, (uint32_t)(pipeline->vertex.va >> 32), 0) >= 1 &&
	    methods_with(0x2018 + 0x40, (uint32_t)pipeline->vertex.va, 0) >= 1, "submit: the pipeline's vertex program");
	check(methods_with(0x1360, 1, 0) == 1 && methods_with(0x1e0c, 0x4303, 0) >= 1, "submit: the quad blends one, one minus source alpha");
	check(methods_with(0x0a00, 0x42000000U, 0) >= 1, "submit: the viewport's x scale 32");
	check(methods_with(0x0a00, 0x41000000U, 0) >= 1 && methods_with(0x0a0c, 0x41900000U, 0) >= 1,
	    "submit: the copy's viewport (x scale 8, x offset 18)");
	check(methods_with(0x0e04, (10U | (26U << 16)), 0) == 1 && methods_with(0x0e08, (20U | (28U << 16)), 0) == 1,
	    "submit: the copy's scissor");
	check(methods_with(0x0110, 0, 1) >= 4, "submit: barriers between the pass and the transfers");
	check(methods_with(0x021c, 0, 1) == 0, "submit: no draw waits for idle to invalidate the shader caches (ws241-p001)");
	check(methods_with(0x0da4, 0x1011U, 0) == gpu_runs, "submit: the shader caches invalidated once at each push buffer's start");
	check(gpu_draws + gpu_quads == 4, "submit: the four draws counted for the node");

	/* The draw's texture: header 0 is image 0x503 (at 0x30004000, rows of 256, 32x32); its handle in the draw's constants. */
	execute_va = 0;
	for (index = 0; index < run_count; index++) {
		if (runs[index].cpu != NULL && runs[index].bytes == 2U * 1024U * 1024U)
			execute_va = runs[index].va;
	}
	check(execute_va != 0, "submit: the session's run");
	if (execute_va == 0)
		return;
	tic = gpu_cpu(execute_va + (1536U * 1024U));
	memcpy(&word, tic + 4, 4);
	check(tic != NULL && word == 0x30004000U, "submit: header 0's address");
	memcpy(&word, tic + 12, 4);
	check((word & 0xffffU) == (256U >> 5), "submit: header 0's pitch");
	constants = gpu_cpu(execute_va + (1024U * 1024U));
	memcpy(&word, constants + 0x400, 4);
	check(word == 0U, "submit: the draw's handle names header 0 and sampler 0");
	memcpy(&word, constants + 0, 4);
	check(word == 0xbf800000U, "submit: the draw's push constants");

	/* The upload off the 256-byte boundary: header 3 from the buffer's 0x40, rows of 64; its quad samples from one pixel in. */
	memcpy(&word, tic + 3 * 32 + 4, 4);
	check(word == 0x30008040U, "submit: the skewed upload's texture starts on the 32-byte boundary before it");
	memcpy(&word, tic + 3 * 32 + 12, 4);
	check((word & 0xffffU) == (64U >> 5), "submit: the skewed upload's texture rows of 64 bytes");
	memcpy(&word, constants + 3 * 0x500 + 0x10, 4);
	check(word == 0x3f800000U, "submit: the skewed upload samples from u 1");
	memcpy(&word, constants + 3 * 0x500 + 0x18, 4);
	check(word == 0x40a00000U, "submit: the skewed upload samples to u 5");
	check(methods_with(0x0e04, (10U | (14U << 16)), 0) == 1 && methods_with(0x0e08, (50U | (53U << 16)), 0) == 1,
	    "submit: the skewed upload's scissor is its region");

	(void)method_last;
	check(nvrtx_vk_session_close(&session) == 0, "submit: session closed");
	check(allocations == before, "submit: nothing leaked");
	free(vertex_words);
	free(fragment_words);
}

/* Every SPIR-V module the compositor embeds (wayland/shaders.h) compiles for SM75, as its pipelines need. */
static void
test_compositor_shaders(void)
{
	static const struct {
		const char *name;
		const uint32_t *words;
		size_t bytes;
		enum drv_gpu_shader_stage stage;
	} modules[] = {
		{ "quad.vert", kwl_quad_vert, sizeof(kwl_quad_vert), DRV_GPU_STAGE_VERTEX },
		{ "quad.frag", kwl_quad_frag, sizeof(kwl_quad_frag), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.vert", kwl_panel_vert, sizeof(kwl_panel_vert), DRV_GPU_STAGE_VERTEX },
		{ "panel.frag", kwl_panel_frag, sizeof(kwl_panel_frag), DRV_GPU_STAGE_FRAGMENT },
		{ "panel-batch.vert", kwl_batch_vert, sizeof(kwl_batch_vert), DRV_GPU_STAGE_VERTEX },
		{ "panel-batch.frag", kwl_batch_frag, sizeof(kwl_batch_frag), DRV_GPU_STAGE_FRAGMENT },
		{ "turn.vert", kwl_turn_vert, sizeof(kwl_turn_vert), DRV_GPU_STAGE_VERTEX },
		{ "panel.frag mode 0", kwl_panel_frag_0, sizeof(kwl_panel_frag_0), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 1", kwl_panel_frag_1, sizeof(kwl_panel_frag_1), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 2", kwl_panel_frag_2, sizeof(kwl_panel_frag_2), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 3", kwl_panel_frag_3, sizeof(kwl_panel_frag_3), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 4", kwl_panel_frag_4, sizeof(kwl_panel_frag_4), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 5", kwl_panel_frag_5, sizeof(kwl_panel_frag_5), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 6", kwl_panel_frag_6, sizeof(kwl_panel_frag_6), DRV_GPU_STAGE_FRAGMENT },
		{ "panel.frag mode 7", kwl_panel_frag_7, sizeof(kwl_panel_frag_7), DRV_GPU_STAGE_FRAGMENT },
	};
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	char what[96];
	unsigned index;
	int error;

	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		binary = NULL;
		memset(&diagnostic, 0, sizeof(diagnostic));
		error = nvrtx_sm75_compile_stage(modules[index].words, modules[index].bytes / 4U, modules[index].stage, &binary, &diagnostic);
		snprintf(what, sizeof(what), "compositor: %s compiles (%d %s)", modules[index].name, error,
			 diagnostic.reason != NULL ? diagnostic.reason : "");
		check(error == 0 && binary != NULL && binary->graphics != 0U && binary->texture_count <= NVRTX_VK_TEXTURES_MAX &&
		    binary->push_bytes <= NVRTX_VK_PUSH_BYTES, what);
		if (binary != NULL)
			nvrtx_sm75_binary_free(binary);
	}
}
