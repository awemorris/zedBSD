/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's VkDeviceMemory lifetime contract, checked on the host
 * (BUG-244).
 *
 * Runs render/memory.c and render/object.c with the wire commands
 * libvulkan sends: buffers and an image are bound to an allocation, and
 * the allocation is freed before them (against the Vulkan rules, as a bad
 * application may).  The buffers and the image bound to it let go of it,
 * so that what reads their storage afterwards finds none instead of the
 * freed record; one bound to another allocation keeps its own; the freed
 * allocation leaves the device's list and the session's table.  Under
 * ASan any read of the freed record fails the run.  The kernel's heap,
 * locks and log are the host's (this file), single-threaded.
 */

#include "contract.h"

#include "../../i915.h"
#include "../../memory.h"
#include "../../render/gfx.h"
#include "../../render/internal.h"
#include "../../render/memory.h"
#include "../../render/object.h"
#include "../../render/state.h"

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The identities the test gives its objects (every kind numbers from its own start). */
#define MEMORY_FIRST		11U
#define MEMORY_SECOND		12U
#define BUFFER_FIRST		21U
#define BUFFER_SECOND		22U
#define BUFFER_OTHER		23U
#define IMAGE_FIRST		31U

/* How many log lines the code under test wrote, and how deep the mutexes are held now. */
static unsigned memory_log_lines;
static int memory_lock_depth;

static void memory_check_free_bound(void);
static void memory_check_free_unknown(void);
static struct i915_gfx_memory *memory_make(struct i915_render_session *session, uint64_t identity, uint64_t size);
static struct i915_gfx_buffer *memory_make_buffer(struct i915_render_session *session, uint64_t identity, uint64_t size);
static int memory_bind(struct i915_render_session *session, uint64_t resource, uint64_t memory, uint64_t offset, int image);
static void memory_free(struct i915_render_session *session, uint64_t identity);
static int memory_listed(struct i915_render_device *vk, struct i915_gfx_memory *memory);

/*
 * Runs the VkDeviceMemory lifetime checks.
 */
int
main(void)
{
	int failures;

	/* The checks. */
	contract_begin("i915 render memory lifetime contract (BUG-244)");
	memory_check_free_bound();
	memory_check_free_unknown();

	/* Reports the verdict. */
	failures = contract_end();
	if (failures != 0)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/*
 * Frees an allocation two buffers and an image are bound to, while a third
 * buffer is bound to another: the three let go of it and have no storage
 * (no CPU view, no GPU address), the third keeps its allocation, the
 * allocation leaves the device's list and the table, and the freeing is
 * logged once.
 */
static void
memory_check_free_bound(void)
{
	struct i915_render_device vk;
	struct i915_render_session session;
	struct i915_gfx_memory *first;
	struct i915_gfx_memory *second;
	struct i915_gfx_buffer *buffer_first;
	struct i915_gfx_buffer *buffer_second;
	struct i915_gfx_buffer *buffer_other;
	struct i915_gfx_image *image;
	struct i915_gem_object storage;
	int error;

	/* The section the results are listed under. */
	contract_section("vkFreeMemory before the buffers and images bound to it");

	/* The executor, its table and a session (no GPU node: nothing here reaches it). */
	memset(&vk, 0, sizeof(vk));
	memset(&session, 0, sizeof(session));
	error = drv_i915_object_table_create(&vk.objects);
	contract_check(error == 0, "the object table is made");
	if (error != 0)
		return;
	(void)mutex_init(&vk.memories_lock, LOCK_RANK_DEVICE, "test memories");
	session.vk = &vk;

	/* Two allocations, the first with storage at a GPU address. */
	memset(&storage, 0, sizeof(storage));
	storage.bytes = 65536U;
	storage.va = 0x100000U;
	first = memory_make(&session, MEMORY_FIRST, 65536U);
	second = memory_make(&session, MEMORY_SECOND, 4096U);
	contract_check(first != NULL && second != NULL, "two allocations are made");
	if (first == NULL || second == NULL)
		return;
	first->object = &storage;

	/* Two buffers and an image in the first, a buffer in the second, through the bind commands. */
	buffer_first = memory_make_buffer(&session, BUFFER_FIRST, 4096U);
	buffer_second = memory_make_buffer(&session, BUFFER_SECOND, 8192U);
	buffer_other = memory_make_buffer(&session, BUFFER_OTHER, 4096U);
	image = calloc(1U, sizeof(*image));
	contract_check(buffer_first != NULL && buffer_second != NULL && buffer_other != NULL && image != NULL, "three buffers and an image are made");
	if (buffer_first == NULL || buffer_second == NULL || buffer_other == NULL || image == NULL)
		return;
	image->bytes = 16384U;
	(void)drv_i915_object_insert(&session, I915_VK_OBJ_IMAGE, IMAGE_FIRST, image);
	error = memory_bind(&session, BUFFER_FIRST, MEMORY_FIRST, 0U, 0);
	error |= memory_bind(&session, BUFFER_SECOND, MEMORY_FIRST, 4096U, 0);
	error |= memory_bind(&session, IMAGE_FIRST, MEMORY_FIRST, 16384U, 1);
	error |= memory_bind(&session, BUFFER_OTHER, MEMORY_SECOND, 0U, 0);
	contract_check(error == 0, "the binds succeed");
	contract_check(buffer_second->memory == first && drv_i915_gfx_memory_va(buffer_second->memory, buffer_second->offset) == 0x101000U,
		       "a bound buffer has its allocation's GPU address");

	/* The first allocation is freed while they are bound to it. */
	memory_log_lines = 0U;
	memory_free(&session, MEMORY_FIRST);

	/* What was bound to it has no storage; nothing reads the freed record. */
	contract_check(buffer_first->memory == NULL && buffer_second->memory == NULL, "the buffers bound to it let go of it");
	contract_check(image->memory == NULL, "the image bound to it lets go of it");
	contract_check(drv_i915_gfx_memory_va(buffer_second->memory, buffer_second->offset) == 0U, "a buffer let go has no GPU address");
	contract_check(drv_i915_gfx_memory_cpu(image->memory, image->offset, 16U) == NULL, "an image let go has no CPU view");

	/* The other allocation's buffer keeps it. */
	contract_check(buffer_other->memory == second, "a buffer bound to another allocation keeps it");

	/* The freed allocation is gone from the table and the list; the binding was logged once. */
	contract_check(drv_i915_object_lookup(&session, I915_VK_OBJ_MEMORY, MEMORY_FIRST) == NULL, "the freed allocation leaves the table");
	contract_check(memory_listed(&vk, second) && vk.memories == second && second->next == NULL, "the freed allocation leaves the device's list");
	contract_check(memory_log_lines == 1U, "the free of a bound allocation is logged once");

	/* A bind to the freed allocation fails now. */
	error = memory_bind(&session, BUFFER_FIRST, MEMORY_FIRST, 0U, 0);
	contract_check(error != 0 && buffer_first->memory == NULL, "a bind to the freed allocation fails");

	/* The rest goes: the second allocation, the objects and the table. */
	memory_free(&session, MEMORY_SECOND);
	contract_check(buffer_other->memory == NULL && vk.memories == NULL, "the second allocation goes too");
	contract_check(memory_lock_depth == 0, "every lock taken is let go");
	drv_i915_object_forget(&session);
	free(buffer_first);
	free(buffer_second);
	free(buffer_other);
	free(image);
	drv_i915_object_table_destroy(vk.objects);
}

/*
 * Frees an identity the session never allocated: nothing happens, and
 * nothing is logged.
 */
static void
memory_check_free_unknown(void)
{
	struct i915_render_device vk;
	struct i915_render_session session;
	int error;

	/* The section the results are listed under. */
	contract_section("vkFreeMemory of an unknown allocation");

	/* The executor and a session. */
	memset(&vk, 0, sizeof(vk));
	memset(&session, 0, sizeof(session));
	error = drv_i915_object_table_create(&vk.objects);
	contract_check(error == 0, "the object table is made");
	if (error != 0)
		return;
	(void)mutex_init(&vk.memories_lock, LOCK_RANK_DEVICE, "test memories");
	session.vk = &vk;

	/* The free does nothing. */
	memory_log_lines = 0U;
	memory_free(&session, 99U);
	contract_check(memory_log_lines == 0U && vk.memories == NULL, "an unknown allocation's free does nothing");

	/* The table goes. */
	drv_i915_object_table_destroy(vk.objects);
}

/* Makes an allocation's record as vkAllocateMemory publishes it: in the table and on the device's list. */
static struct i915_gfx_memory *
memory_make(
	struct i915_render_session *session,
	uint64_t identity,
	uint64_t size)
{
	struct i915_gfx_memory *memory;
	int error;

	/* The record. */
	memory = calloc(1U, sizeof(*memory));
	if (memory == NULL)
		return NULL;
	memory->vk = session->vk;
	memory->gpu = session->gpu;
	memory->identity = identity;
	memory->size = size;

	/* In the table. */
	error = drv_i915_object_insert(session, I915_VK_OBJ_MEMORY, identity, memory);
	if (error != 0) {
		free(memory);
		return NULL;
	}

	/* On the device's list, newest first. */
	memory->next = session->vk->memories;
	session->vk->memories = memory;

	/* Succeeded: the allocation is published. */
	return memory;
}

/* Makes a buffer's record as vkCreateBuffer publishes it. */
static struct i915_gfx_buffer *
memory_make_buffer(
	struct i915_render_session *session,
	uint64_t identity,
	uint64_t size)
{
	struct i915_gfx_buffer *buffer;
	int error;

	/* The record. */
	buffer = calloc(1U, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	buffer->size = size;

	/* In the table. */
	error = drv_i915_object_insert(session, I915_VK_OBJ_BUFFER, identity, buffer);
	if (error != 0) {
		free(buffer);
		return NULL;
	}

	/* Succeeded: the buffer is published. */
	return buffer;
}

/*
 * Sends vkBindBufferMemory or vkBindImageMemory as libvulkan does:
 * [device][resource][memory][offset].  Returns 0 when the reply says the
 * bind succeeded.
 */
static int
memory_bind(
	struct i915_render_session *session,
	uint64_t resource,
	uint64_t memory,
	uint64_t offset,
	int image)
{
	struct i915_wire_reader reader;
	struct i915_wire_writer writer;
	uint64_t words[4];
	uint32_t result;
	uint8_t answer[16];
	int error;

	/* The command. */
	words[0] = 1U;
	words[1] = resource;
	words[2] = memory;
	words[3] = offset;
	memset(&reader, 0, sizeof(reader));
	reader.base = (const uint8_t *)words;
	reader.size = sizeof(words);
	memset(&writer, 0, sizeof(writer));
	writer.base = answer;
	writer.size = sizeof(answer);

	/* Carried out; the reply's first word is the VkResult. */
	error = drv_i915_gfx_bind(session, &reader, &writer, image);
	if (error != 0)
		return error;
	memcpy(&result, answer, sizeof(result));
	if (result != 0U)
		return EINVAL;

	/* Succeeded: bound. */
	return 0;
}

/* Sends vkFreeMemory as libvulkan does: [device][identity][pAllocator]. */
static void
memory_free(
	struct i915_render_session *session,
	uint64_t identity)
{
	struct i915_wire_reader reader;
	uint64_t words[3];

	/* The command, carried out. */
	words[0] = 1U;
	words[1] = identity;
	words[2] = 0U;
	memset(&reader, 0, sizeof(reader));
	reader.base = (const uint8_t *)words;
	reader.size = sizeof(words);
	(void)drv_i915_gfx_free_memory(session, &reader);
}

/* Tells whether an allocation is on its device's list. */
static int
memory_listed(
	struct i915_render_device *vk,
	struct i915_gfx_memory *memory)
{
	struct i915_gfx_memory *walk;

	/* Each one on the list. */
	for (walk = vk->memories; walk != NULL; walk = walk->next) {
		if (walk == memory)
			return 1;
	}

	/* Not on it. */
	return 0;
}

/*
 * Stands in for the kernel heap's zeroed allocation: the host's.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *pointer;

	/* The host's allocation. */
	pointer = calloc(count, size);

	/* Reports it (NULL when the host had none). */
	return pointer;
}

/*
 * Stands in for the kernel heap's release: the host's.
 */
void
kern_free(
	void *pointer)
{
	/* The host's release. */
	free(pointer);
}

/*
 * Stands in for the kernel log: counts the lines and shows them.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* One more line, shown. */
	memory_log_lines++;
	va_start(arguments, format);
	(void)vprintf(format, arguments);
	va_end(arguments);
}

/*
 * Stands in for a mutex's setup: nothing to do on one thread.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	UNUSED_PARAMETER(mutex);
	UNUSED_PARAMETER(rank);
	UNUSED_PARAMETER(name);

	/* Succeeded: ready. */
	return 0;
}

/*
 * Stands in for taking a mutex: counts how deeply mutexes are held.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	UNUSED_PARAMETER(mutex);

	/* One more held. */
	memory_lock_depth++;
}

/*
 * Stands in for letting a mutex go.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	UNUSED_PARAMETER(mutex);

	/* One fewer held. */
	memory_lock_depth--;
}

/*
 * Stands in for the direct map of physical RAM: the address itself (the
 * test's storage has no pages, and no check reads through the view).
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* The address as a pointer. */
	return (void *)(uintptr_t)address;
}

/*
 * Stands in for the texel buffer formats of state.c, which no check uses.
 */
int
drv_i915_gfx_texel_buffer_format(
	uint32_t format,
	uint32_t *surface_format,
	uint32_t *bytes)
{
	UNUSED_PARAMETER(format);
	UNUSED_PARAMETER(surface_format);
	UNUSED_PARAMETER(bytes);

	/* No format in this test. */
	return EINVAL;
}
