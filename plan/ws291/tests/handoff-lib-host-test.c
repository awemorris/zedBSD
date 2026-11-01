/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test H3b of libvulkan's submission hand-off: the whole library
 * (queue.c, commands.c, context.c, sync.c, sender.c and the rest) against
 * a fake GPU node behind its ioctl, mmap and munmap (the library's objects
 * are compiled with those names taken by the h3b_ ones below).  The node
 * answers GPU_GET_INFO and the capset as nvrtx does (jobs, commands run at
 * once), keeps blobs in host memory, runs each command stream at once
 * (finding the commands that ask for a reply by their header and answering
 * them), supervises jobs that end 50 us after they were submitted, and
 * checks the order the streams arrive in: a command buffer is submitted
 * only after its End, and the parts of a frame arrive in order.
 *
 * Three threads: one draws (frames of fence-less parts and a fenced last
 * part, waiting for each slot's fence, as mview's --submits does), one
 * waits on and polls the fences, one creates and destroys fences.  Each run
 * is bounded by the caller's time limit (a deadlock is a time-out).
 *
 * usage: handoff-lib-host-test [abort-case]
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>
#include "internal.h"

#define H3B_SLOTS 2U
#define H3B_PARTS 40U
#define H3B_FRAMES 150U
#define H3B_DRAWS 4U
#define H3B_BLOBS 64U
#define H3B_JOBS 4096U
#define H3B_COMMANDS 256U
#define H3B_JOB_NS 50000U

/* A blob of the fake node: its handle, resource and host memory. */
struct h3b_blob {
	uint64_t handle;
	uint32_t resource;
	size_t bytes;
	uint8_t *memory;
};

/* A job of the fake node: when it ends, and whether its record was consumed. */
struct h3b_job {
	uint64_t ends_ns;
	int consumed;
};

static pthread_mutex_t h3b_lock = PTHREAD_MUTEX_INITIALIZER;
static struct h3b_blob h3b_blobs[H3B_BLOBS];
static unsigned h3b_blob_count;
static struct h3b_job h3b_jobs[H3B_JOBS];
static uint64_t h3b_job_count;
static uint64_t h3b_cb_ids[H3B_COMMANDS];
static unsigned h3b_cb_state[H3B_COMMANDS];
static unsigned h3b_cb_count;
static int h3b_last_part = -1;
static unsigned h3b_submits;
static unsigned h3b_job_submits;
static unsigned h3b_failures;
static unsigned h3b_checks;
static int h3b_gate_closed;
static pthread_cond_t h3b_gate = PTHREAD_COND_INITIALIZER;
static volatile int h3b_stop;
static struct vulkan_context h3b_context;
static struct VkDevice_T *h3b_device;
static struct VkQueue_T *h3b_queue;

int h3b_ioctl(int fd, unsigned long request, ...);
void *h3b_mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset);
int h3b_munmap(void *address, size_t bytes);

static void check(int ok, const char *what);
static uint64_t now_ns(void);
static uint32_t get32(const uint8_t *at);
static uint64_t get64(const uint8_t *at);
static void put32(uint8_t *at, uint32_t value);
static void put64(uint8_t *at, uint64_t value);
static struct h3b_blob *blob_of_resource(uint32_t resource);
static int command_index(uint64_t id);
static int run_stream(const uint8_t *data, size_t bytes);
static void setup(void);
static void *draw_main(void *argument);
static void *fence_main(void *argument);
static void *object_main(void *argument);
static int abort_case(void);

/* Vulkan entry points the library names but this test leaves out (the Wayland surface). */
VkResult vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
	(void)instance; (void)info; (void)allocator; (void)surface;
	abort();
}

VkBool32 vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physical, uint32_t family, struct wl_display *display)
{
	(void)physical; (void)family; (void)display;
	abort();
}

static void
check(int ok, const char *what)
{
	__atomic_add_fetch(&h3b_checks, 1U, __ATOMIC_RELAXED);
	if (ok)
		return;
	if (__atomic_add_fetch(&h3b_failures, 1U, __ATOMIC_RELAXED) <= 20U)
		printf("FAIL %s\n", what);
}

static uint64_t
now_ns(void)
{
	struct timespec sample;

	clock_gettime(CLOCK_MONOTONIC, &sample);
	return (uint64_t)sample.tv_sec * 1000000000U + (uint64_t)sample.tv_nsec;
}

static uint32_t get32(const uint8_t *at) { uint32_t v; memcpy(&v, at, 4); return v; }
static uint64_t get64(const uint8_t *at) { uint64_t v; memcpy(&v, at, 8); return v; }
static void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, 4); }
static void put64(uint8_t *at, uint64_t value) { memcpy(at, &value, 8); }

static struct h3b_blob *
blob_of_resource(uint32_t resource)
{
	unsigned index;

	for (index = 0; index < h3b_blob_count; index++) {
		if (h3b_blobs[index].resource == resource)
			return &h3b_blobs[index];
	}
	return NULL;
}

static int
command_index(uint64_t id)
{
	unsigned index;

	for (index = 0; index < h3b_cb_count; index++) {
		if (h3b_cb_ids[index] == id)
			return (int)index;
	}
	return -1;
}

/* Whether a word is the number of a command this test's library calls send with a reply asked for. */
static int
reply_opcode(uint32_t opcode)
{
	switch (opcode) {
	case GPU_OP_DESTROY_DEVICE:
	case GPU_OP_QUEUE_SUBMIT:
	case GPU_OP_QUEUE_WAIT_IDLE:
	case GPU_OP_CREATE_FENCE:
	case GPU_OP_DESTROY_FENCE:
	case GPU_OP_RESET_FENCES:
	case GPU_OP_GET_FENCE_STATUS:
	case GPU_OP_CREATE_COMMAND_POOL:
	case GPU_OP_DESTROY_COMMAND_POOL:
	case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
	case GPU_OP_FREE_COMMAND_BUFFERS:
	case GPU_OP_BEGIN_COMMAND_BUFFER:
	case GPU_OP_END_COMMAND_BUFFER:
	case GPU_OP_RESET_COMMAND_BUFFER:
	case GPU_OP_ENUMERATE_INSTANCE_VERSION:
		return 1;
	default:
		return 0;
	}
}

/*
 * Runs one command stream: the reply stream it names, each command that
 * asks for a reply answered there (its body ends where the next such
 * command or the reply seek begins), the order checks, the trailer.
 */
static int
run_stream(const uint8_t *data, size_t bytes)
{
	size_t heads[512];
	size_t head_count;
	size_t at;
	size_t end;
	size_t seek;
	size_t index;
	struct h3b_blob *reply_blob;
	uint8_t *reply;
	size_t cursor;
	uint32_t opcode;
	uint64_t id;
	uint32_t count;
	uint32_t request;
	uint32_t commands;
	uint32_t waits;
	uint32_t command;
	int found;
	int part;

	/* The reply stream: [178][0][u64 1][u32 resource][u64 0][u64 capacity]. */
	if (bytes < 36 || get32(data) != GPU_OP_SET_REPLY_STREAM)
		return EINVAL;
	reply_blob = blob_of_resource(get32(data + 16));
	if (reply_blob == NULL)
		return EINVAL;
	reply = reply_blob->memory;
	at = 36;

	/* The reply seek and the trailer close the stream: [179][0][u64 offset][137][1][u64 1]. */
	if (bytes < at + 32 || get32(data + bytes - 32) != GPU_OP_SEEK_REPLY_STREAM)
		return EINVAL;
	seek = (size_t)get64(data + bytes - 24);
	end = bytes - 32;

	/* The commands that ask for replies, by their headers. */
	head_count = 0;
	for (index = at; index + 8 <= end; index += 4) {
		if (get32(data + index + 4) == GPU_OP_FLAG_REPLY && reply_opcode(get32(data + index)) && head_count < 512)
			heads[head_count++] = index;
	}

	/* Each answered in order. */
	cursor = 0;
	for (index = 0; index < head_count; index++) {
		size_t body;
		size_t stop;

		body = heads[index] + 8;
		stop = index + 1 < head_count ? heads[index + 1] : end;
		opcode = get32(data + heads[index]);
		put32(reply + cursor, opcode);
		cursor += 4;
		switch (opcode) {
		case GPU_OP_DESTROY_DEVICE:
		case GPU_OP_DESTROY_FENCE:
		case GPU_OP_DESTROY_COMMAND_POOL:
		case GPU_OP_FREE_COMMAND_BUFFERS:
			break;
		case GPU_OP_CREATE_FENCE:
		case GPU_OP_CREATE_COMMAND_POOL:
			put32(reply + cursor, VK_SUCCESS);
			put64(reply + cursor + 4, 1U);
			put64(reply + cursor + 12, get64(data + stop - 8));
			cursor += 20;
			break;
		case GPU_OP_ALLOCATE_COMMAND_BUFFERS:
			id = get64(data + stop - 8);
			put32(reply + cursor, VK_SUCCESS);
			put64(reply + cursor + 4, 1U);
			put64(reply + cursor + 12, id);
			cursor += 20;
			break;
		case GPU_OP_RESET_COMMAND_BUFFER:
		case GPU_OP_BEGIN_COMMAND_BUFFER:
		case GPU_OP_END_COMMAND_BUFFER:
			id = get64(data + body);
			command = (uint32_t)command_index(id);
			if (command < h3b_cb_count) {
				if (opcode == GPU_OP_END_COMMAND_BUFFER) {
					check(h3b_cb_state[command] == 1U, "End comes after its Begin");
					h3b_cb_state[command] = 2U;
				} else if (opcode == GPU_OP_BEGIN_COMMAND_BUFFER) {
					h3b_cb_state[command] = 1U;
				} else {
					h3b_cb_state[command] = 0U;
				}
			}
			put32(reply + cursor, VK_SUCCESS);
			cursor += 4;
			break;
		case GPU_OP_QUEUE_SUBMIT:
			/* [u64 queue][u32 n][u64 n] then each submit: [u32 sType][u64 pNext][u32 w][u64 w][w u64][u64 w][w u32][u32 c][u64 c][c u64][u32 s][u64 s][s u64]. */
			at = body + 8;
			count = get32(data + at);
			at += 12;
			for (request = 0; request < count && at < stop; request++) {
				at += 12;
				waits = get32(data + at);
				at += 12 + (size_t)waits * 8 + 8 + (size_t)waits * 4;
				commands = get32(data + at);
				at += 12;
				for (command = 0; command < commands; command++) {
					found = command_index(get64(data + at));
					at += 8;
					if (found < 0)
						continue;
					check(h3b_cb_state[found] == 2U, "a command buffer is submitted only after its End");
					part = found % (int)H3B_PARTS;
					check(part == (h3b_last_part + 1) % (int)H3B_PARTS, "the parts of a frame arrive in order");
					h3b_last_part = part;
				}
				at += 12 + (size_t)get32(data + at) * 8;
			}
			h3b_submits++;
			put32(reply + cursor, VK_SUCCESS);
			cursor += 4;
			break;
		case GPU_OP_ENUMERATE_INSTANCE_VERSION:
			put32(reply + cursor, VK_SUCCESS);
			cursor += 4;
			break;
		default:
			put32(reply + cursor, VK_SUCCESS);
			cursor += 4;
			break;
		}
	}

	/* The trailer: the version reply at the seek's offset. */
	put32(reply + seek, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	put32(reply + seek + 4, 0U);
	put32(reply + seek + 8, 1U);
	put32(reply + seek + 12, 0U);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	put32(reply + seek + 16, VK_MAKE_VERSION(1, 3, 0));
	return 0;
}

/* The fake node's ioctl. */
int
h3b_ioctl(int fd, unsigned long request, ...)
{
	__builtin_va_list arguments;
	void *argument;
	struct gpu_info *info;
	struct gpu_capset *capset;
	struct gpu_blob_create *blob;
	struct gpu_resource_map *map;
	struct gpu_command *command;
	struct gpu_job_submit *job;
	struct gpu_command_wait *wait;
	struct gpu_job_retire *retire;
	struct gpu_job_retire_entry *entries;
	struct gpu_job_capacity *capacity;
	uint64_t ends;
	uint64_t now;
	uint64_t deadline;
	uint32_t index;
	int error;

	(void)fd;
	__builtin_va_start(arguments, request);
	argument = __builtin_va_arg(arguments, void *);
	__builtin_va_end(arguments);
	error = 0;
	pthread_mutex_lock(&h3b_lock);
	while (h3b_gate_closed && (request == GPU_COMMAND || request == GPU_JOB_SUBMIT))
		pthread_cond_wait(&h3b_gate, &h3b_lock);

	switch (request) {
	case GPU_GET_INFO:
		info = argument;
		info->capabilities = GPU_CAP_CAPSET | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_COMMAND | GPU_CAP_MAPPING |
				     GPU_CAP_NOTIFICATION | GPU_CAP_JOB_SUBMIT;
		info->max_resources = 1024;
		info->max_resource_bytes = 256U * 1024U * 1024U;
		break;
	case GPU_GET_CAPSET:
		capset = argument;
		memset(capset->data, 0, 176);
		put32(capset->data, 1U);
		put32(capset->data + 4, VK_MAKE_VERSION(1, 3, 269));
		put32(capset->data + 152, 64U);
		put32(capset->data + 160, 0x5a424453U);
		put32(capset->data + 164, 7U);
		put32(capset->data + 168, 0x5a4e4154U);
		put32(capset->data + 172, 2U);
		capset->bytes = 176;
		break;
	case GPU_BLOB_CREATE:
		blob = argument;
		if (h3b_blob_count == H3B_BLOBS) {
			error = ENOMEM;
			break;
		}
		h3b_blobs[h3b_blob_count].handle = h3b_blob_count + 1U;
		h3b_blobs[h3b_blob_count].resource = h3b_blob_count + 100U;
		h3b_blobs[h3b_blob_count].bytes = (size_t)blob->bytes;
		h3b_blobs[h3b_blob_count].memory = calloc(1, (size_t)blob->bytes + 4096U);
		blob->handle = h3b_blobs[h3b_blob_count].handle;
		blob->resource_id = h3b_blobs[h3b_blob_count].resource;
		h3b_blob_count++;
		break;
	case GPU_RESOURCE_MAP:
		map = argument;
		if (map->handle == 0 || map->handle > h3b_blob_count) {
			error = EINVAL;
			break;
		}
		map->offset = map->handle * 0x100000U;
		map->bytes = h3b_blobs[map->handle - 1U].bytes;
		break;
	case GPU_RESOURCE_DESTROY:
		break;
	case GPU_COMMAND:
		command = argument;
		usleep(5);
		error = run_stream((const uint8_t *)(uintptr_t)command->address, command->bytes);
		break;
	case GPU_JOB_SUBMIT:
		job = argument;
		error = run_stream((const uint8_t *)(uintptr_t)job->address, job->bytes);
		if (error != 0)
			break;
		if (h3b_job_count + 1U >= H3B_JOBS) {
			error = EAGAIN;
			break;
		}
		h3b_job_count++;
		h3b_jobs[h3b_job_count].ends_ns = now_ns() + H3B_JOB_NS;
		h3b_jobs[h3b_job_count].consumed = 0;
		job->sequence = h3b_job_count;
		job->outcome = GPU_JOB_SUBMIT_COMMITTED;
		job->error = 0;
		h3b_job_submits++;
		break;
	case GPU_COMMAND_WAIT:
		wait = argument;
		if (wait->sequence == 0 || wait->sequence > h3b_job_count || h3b_jobs[wait->sequence].consumed) {
			error = ENOENT;
			break;
		}
		ends = h3b_jobs[wait->sequence].ends_ns;
		now = now_ns();
		if (now < ends) {
			if (wait->timeout_ns == 0) {
				error = EAGAIN;
				break;
			}
			deadline = wait->timeout_ns == UINT64_MAX ? ends : now + wait->timeout_ns;
			pthread_mutex_unlock(&h3b_lock);
			while (now_ns() < ends && now_ns() < deadline)
				usleep(10);
			pthread_mutex_lock(&h3b_lock);
			if (now_ns() < ends) {
				error = ETIMEDOUT;
				break;
			}
			if (h3b_jobs[wait->sequence].consumed) {
				error = ENOENT;
				break;
			}
		}
		wait->status = 0;
		if (wait->flags & GPU_WAIT_CONSUME)
			h3b_jobs[wait->sequence].consumed = 1;
		break;
	case GPU_JOB_RETIRE:
		retire = argument;
		entries = (struct gpu_job_retire_entry *)(uintptr_t)retire->address;
		now = now_ns();
		for (index = 0; index < retire->count; index++) {
			if (entries[index].sequence == 0 || entries[index].sequence > h3b_job_count) {
				entries[index].state = GPU_JOB_RETIRE_MISSING;
			} else if (h3b_jobs[entries[index].sequence].consumed || now < h3b_jobs[entries[index].sequence].ends_ns) {
				entries[index].state = GPU_JOB_RETIRE_PENDING;
			} else {
				h3b_jobs[entries[index].sequence].consumed = 1;
				entries[index].state = GPU_JOB_RETIRE_RETIRED;
				entries[index].status = 0;
				retire->retired++;
			}
		}
		break;
	case GPU_JOB_CAPACITY:
		capacity = argument;
		capacity->sequence = 1;
		capacity->available = 1;
		break;
	default:
		error = ENOTTY;
		break;
	}
	pthread_mutex_unlock(&h3b_lock);
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

void *
h3b_mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset)
{
	uint64_t handle;
	void *memory;

	(void)address; (void)bytes; (void)prot; (void)flags; (void)fd;
	handle = (uint64_t)offset / 0x100000U;
	pthread_mutex_lock(&h3b_lock);
	memory = handle != 0 && handle <= h3b_blob_count ? h3b_blobs[handle - 1U].memory : MAP_FAILED;
	pthread_mutex_unlock(&h3b_lock);
	return memory;
}

int
h3b_munmap(void *address, size_t bytes)
{
	(void)address; (void)bytes;
	return 0;
}

/* The context, a device and a queue made as vkCreateDevice makes them, without a renderer device. */
static void
setup(void)
{
	struct vulkan_object *object;
	VkResult status;

	status = vulkan_context_open(&h3b_context, "/dev/null");
	check(status == VK_SUCCESS, "the context opens on the fake node");
	check(h3b_context.sync_command == VK_TRUE, "the context runs commands at once");
	status = vulkan_object_alloc(sizeof(struct VkDevice_T), __alignof__(struct VkDevice_T), VULKAN_OBJECT_DEVICE, NULL, &h3b_context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &object);
	check(status == VK_SUCCESS, "a device");
	h3b_device = (struct VkDevice_T *)object;
	pthread_mutex_init(&h3b_device->mutex, NULL);
	(void)vulkan_object_reserve_id(object);
	status = vulkan_object_alloc(sizeof(struct VkQueue_T), __alignof__(struct VkQueue_T), VULKAN_OBJECT_QUEUE, &h3b_device->object, &h3b_context, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &object);
	check(status == VK_SUCCESS, "a queue");
	h3b_queue = (struct VkQueue_T *)object;
	h3b_queue->device = h3b_device;
	h3b_queue->timeline_index = 1;
	pthread_mutex_init(&h3b_queue->mutex, NULL);
	(void)vulkan_object_reserve_id(object);
	h3b_device->queues = calloc(1, sizeof(*h3b_device->queues));
	h3b_device->queues[0] = h3b_queue;
	h3b_device->queue_count = 1;
	(void)vulkan_object_publish(&h3b_device->object);
	(void)vulkan_object_publish(&h3b_queue->object);
}

static VkCommandBuffer h3b_buffers[H3B_COMMANDS];
static VkFence h3b_fences[H3B_SLOTS];

/* Frames of H3B_PARTS parts: each part reset, begun, drawn, ended and submitted; the last with the slot's fence. */
static void *
draw_main(void *argument)
{
	VkCommandBufferBeginInfo begin;
	VkSubmitInfo submit;
	VkResult status;
	unsigned frame;
	unsigned slot;
	unsigned part;
	unsigned draw;
	VkCommandBuffer buffer;

	(void)argument;
	for (frame = 0; frame < H3B_FRAMES; frame++) {
		slot = frame % H3B_SLOTS;
		status = vkWaitForFences((VkDevice)h3b_device, 1, &h3b_fences[slot], VK_TRUE, UINT64_MAX);
		if (status != VK_SUCCESS && h3b_failures == 0)
			printf("wait status %d frame %u\n", (int)status, frame);
		check(status == VK_SUCCESS, "the slot's fence is waited for");
		status = vkResetFences((VkDevice)h3b_device, 1, &h3b_fences[slot]);
		check(status == VK_SUCCESS, "the slot's fence is reset");
		for (part = 0; part < H3B_PARTS; part++) {
			buffer = h3b_buffers[slot * H3B_PARTS + part];
			status = vkResetCommandBuffer(buffer, 0);
			check(status == VK_SUCCESS, "reset");
			memset(&begin, 0, sizeof(begin));
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			status = vkBeginCommandBuffer(buffer, &begin);
			check(status == VK_SUCCESS, "begin");
			for (draw = 0; draw < H3B_DRAWS; draw++)
				vkCmdDraw(buffer, 6, 2, 0, 0);
			status = vkEndCommandBuffer(buffer);
			check(status == VK_SUCCESS, "end");
			memset(&submit, 0, sizeof(submit));
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &buffer;
			status = vkQueueSubmit((VkQueue)h3b_queue, 1, &submit, part + 1 == H3B_PARTS ? h3b_fences[slot] : VK_NULL_HANDLE);
			check(status == VK_SUCCESS, "submit");
		}
	}
	for (slot = 0; slot < H3B_SLOTS; slot++) {
		status = vkWaitForFences((VkDevice)h3b_device, 1, &h3b_fences[slot], VK_TRUE, UINT64_MAX);
		check(status == VK_SUCCESS, "the last fences are waited for");
	}
	return NULL;
}

/* Polls and briefly waits on the drawing thread's fences. */
static void *
fence_main(void *argument)
{
	VkResult status;
	unsigned slot;

	(void)argument;
	while (!__atomic_load_n(&h3b_stop, __ATOMIC_ACQUIRE)) {
		for (slot = 0; slot < H3B_SLOTS; slot++) {
			status = vkGetFenceStatus((VkDevice)h3b_device, h3b_fences[slot]);
			if (status != VK_SUCCESS && status != VK_NOT_READY && h3b_failures == 0)
				printf("fence status %d\n", (int)status);
			check(status == VK_SUCCESS || status == VK_NOT_READY, "a fence's status is read");
		}
		status = vkWaitForFences((VkDevice)h3b_device, H3B_SLOTS, h3b_fences, VK_FALSE, 200000U);
		check(status == VK_SUCCESS || status == VK_TIMEOUT, "the fences are waited on");

		/* A status read holds the device's mutex across its transaction: a spinning reader would starve the drawer. */
		usleep(300);
	}
	return NULL;
}

/* Makes and destroys fences, each a transaction of its own, while hand-offs are queued. */
static void *
object_main(void *argument)
{
	VkFenceCreateInfo create;
	VkFence fence;
	VkResult status;

	(void)argument;
	while (!__atomic_load_n(&h3b_stop, __ATOMIC_ACQUIRE)) {
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		status = vkCreateFence((VkDevice)h3b_device, &create, NULL, &fence);
		if (status != VK_SUCCESS && h3b_failures == 0)
			printf("create status %d\n", (int)status);
		check(status == VK_SUCCESS, "a fence is made");
		if (status == VK_SUCCESS)
			vkDestroyFence((VkDevice)h3b_device, fence, NULL);
		usleep(50);
	}
	return NULL;
}

static void
make_objects(void)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo allocate;
	VkFenceCreateInfo create;
	VkCommandPool pool;
	VkResult status;
	unsigned index;

	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	status = vkCreateCommandPool((VkDevice)h3b_device, &pool_info, NULL, &pool);
	check(status == VK_SUCCESS, "a command pool");
	for (index = 0; index < H3B_SLOTS * H3B_PARTS; index++) {
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool = pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		status = vkAllocateCommandBuffers((VkDevice)h3b_device, &allocate, &h3b_buffers[index]);
		check(status == VK_SUCCESS, "a command buffer");
		pthread_mutex_lock(&h3b_lock);
		h3b_cb_ids[h3b_cb_count++] = ((struct VkCommandBuffer_T *)h3b_buffers[index])->object.wire_id;
		pthread_mutex_unlock(&h3b_lock);
	}
	for (index = 0; index < H3B_SLOTS; index++) {
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		create.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		status = vkCreateFence((VkDevice)h3b_device, &create, NULL, &h3b_fences[index]);
		check(status == VK_SUCCESS, "a slot's fence");
	}
}

/* In a fresh process: a transaction begun with a context mutex held, while a hand-off waits, aborts. */
static int
abort_case(void)
{
	VkCommandBufferBeginInfo begin;
	VkSubmitInfo submit;
	VkFenceCreateInfo create;
	VkFence fence;

	setup();
	make_objects();
	pthread_mutex_lock(&h3b_lock);
	h3b_gate_closed = 1;
	pthread_mutex_unlock(&h3b_lock);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	(void)vkBeginCommandBuffer(h3b_buffers[0], &begin);
	(void)vkEndCommandBuffer(h3b_buffers[0]);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &h3b_buffers[0];
	(void)vkQueueSubmit((VkQueue)h3b_queue, 1, &submit, VK_NULL_HANDLE);
	vulkan_context_lock(&h3b_context);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	(void)vkCreateFence((VkDevice)h3b_device, &create, NULL, &fence);
	return 4;
}

int
main(int argc, char **argv)
{
	pthread_t draw;
	pthread_t fences;
	pthread_t objects;
	pid_t child;
	int status;
	const char *handoff;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1 && strcmp(argv[1], "abort-case") == 0)
		return abort_case();

	setup();
	make_objects();
	pthread_create(&draw, NULL, draw_main, NULL);
	if (getenv("H3B_DRAW_ONLY") == NULL) {
		pthread_create(&fences, NULL, fence_main, NULL);
		pthread_create(&objects, NULL, object_main, NULL);
	}
	pthread_join(draw, NULL);
	__atomic_store_n(&h3b_stop, 1, __ATOMIC_RELEASE);
	if (getenv("H3B_DRAW_ONLY") == NULL) {
		pthread_join(fences, NULL);
		pthread_join(objects, NULL);
	}

	vulkan_sender_drain(&h3b_context);
	handoff = getenv("VULKAN_ZEDBSD_SUBMIT_HANDOFF");
	printf("submits %u (jobs %u), hand-offs queued %llu sent %llu, context error %d\n", h3b_submits, h3b_job_submits,
	       (unsigned long long)h3b_context.handoff_queued, (unsigned long long)h3b_context.handoff_sent, (int)h3b_context.error);
	check(h3b_submits == H3B_FRAMES * H3B_PARTS, "every part reached the node");
	check(h3b_job_submits >= H3B_FRAMES, "each frame's last part went as a job");
	if (handoff != NULL && strcmp(handoff, "0") == 0)
		check(h3b_context.handoff_queued == 0, "with the hand-off off, nothing is handed off");
	else
		check(h3b_context.handoff_queued == (uint64_t)H3B_FRAMES * (H3B_PARTS - 1U), "every fence-less part was handed off");
	check(h3b_context.handoff_sent == h3b_context.handoff_queued, "every hand-off was sent");
	check(h3b_context.error == VK_SUCCESS, "the context was not lost");

	/* The queue's teardown, as vkDestroyDevice does it: its places go after a drain. */
	vulkan_queue_finish(h3b_queue);
	check(h3b_queue->handoffs == NULL, "the queue's places are gone after the drain");

	/* The abort of a drain with a context mutex held. */
	if (handoff == NULL || strcmp(handoff, "0") != 0) {
		fflush(stdout);
		child = fork();
		if (child == 0) {
			execl(argv[0], argv[0], "abort-case", (char *)NULL);
			_exit(5);
		}
		status = 0;
		waitpid(child, &status, 0);
		check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "a transaction with a context mutex held aborts while a hand-off waits");
	}

	printf("handoff-lib-host-test: %u checks, %u failures\n", h3b_checks, h3b_failures);
	if (h3b_failures != 0) {
		printf("handoff-lib-host-test: FAIL\n");
		return 1;
	}
	printf("handoff-lib-host-test: PASS\n");
	return 0;
}
