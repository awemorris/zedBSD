/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's render targets and free lists, and the
 * session's program windows.
 *
 * A session's first render makes its global free list, one fixed pool
 * of pages that never grows, and each frame shape it renders gets a
 * render target of its own: the firmware's data set in two halves with
 * their memories, and a local free list of the core's minimum size.  A
 * session keeps a few targets and reuses them; a compute-only session
 * makes none.  The firmware may ask for more pages, or for the lists to
 * be rebuilt after a recovery; every request is answered, a grow with
 * nothing added.  When a session closes, the firmware cleans its targets
 * and lists up before their memory goes; when it cannot, or the core may
 * still run the session's work, they stay with the core until a reset.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/render-target-layout.h"
#include "drivers/gpu/powervr/vk/vk.h"

/*
 * The most render targets a session keeps, one a framebuffer size: a
 * compositor's frame renders to its output, its windows' layers and its
 * blur targets, and a target made again for each costs firmware
 * commands a job.
 */
#define TARGETS_MAX			8U

/*
 * How long a cleanup of a target or a list may stay busy: this many
 * tries, a pause between them, and no more time in all.
 */
#define CLEANUP_TRIES			50U
#define CLEANUP_PAUSE_MS		10U
#define CLEANUP_LIMIT_MS		2000U

_Static_assert(POWERVR_TARGET_COMMAND_WORDS == POWERVR_COMMAND_WORDS, "a target command is one kernel command");
_Static_assert(POWERVR_TARGET_RECORD_BYTES <= 0x1000U, "a target's records fit one firmware page");

/*
 * One free list: the pages the parameter manager takes from, the list
 * of their numbers it reads (mapped in the session's general window),
 * and the firmware's record of it, listed in the render node's free lists
 * by its identity.  Owner is the render target a local list belongs to,
 * NULL for the global one; grow_requests counts the firmware's requests.
 */
struct powervr_free_list {
	struct powervr_free_list *next;
	struct powervr_target *owner;
	struct powervr_buffer *pages;
	struct powervr_resource *entries;
	struct powervr_object record;
	uint32_t identity;
	uint32_t page_count;
	unsigned grow_requests;
	unsigned listed;
	unsigned cleaned;
};

/*
 * One render target: the frame shape it was made for, what it costs,
 * its memories (one macrolist and region headers per half, the vertex
 * heap table and tail pointer cache shared), the firmware's block of its
 * two halves and their shared record, and its local free list.  Used is
 * the session's render count when it was last taken, so the least
 * recently used one is replaced.
 */
struct powervr_target {
	struct powervr_target_shape shape;
	struct powervr_target_layout layout;
	struct powervr_resource *macrolists;
	struct powervr_resource *region_headers;
	struct powervr_resource *vertex_heap;
	struct powervr_resource *tails;
	struct powervr_object record;
	struct powervr_free_list local;
	uint64_t used;
	unsigned cleaned;

	/*
	 * The half the next render takes; renders alternate between the two
	 * (each with its own macrolist and region headers), as Mesa
	 * alternates a data set's two render target datas, so a render never
	 * reuses the parameter manager state its predecessor's 3D read.
	 */
	unsigned next_half;
};

/*
 * A session's render targets: its global free list once made, the
 * targets it keeps, and how many renders took one.
 */
struct powervr_targets {
	struct powervr_free_list global;
	unsigned global_made;
	struct powervr_target *kept[TARGETS_MAX];
	unsigned count;
	uint64_t renders;
};

static int list_make(struct powervr_render *render, struct powervr_session *session, uint32_t pages, struct powervr_target *owner, struct powervr_free_list *list);
static void list_write_entries(struct powervr_free_list *list);
static void list_unlist(struct powervr_render *render, struct powervr_free_list *list);
static void list_destroy(struct powervr_render *render, struct powervr_session *session, struct powervr_free_list *list);
static void list_discard(struct powervr_render *render, struct powervr_session *session, struct powervr_free_list *list);
static struct powervr_free_list *list_find(struct powervr_render *render, uint32_t identity);
static int target_make(struct powervr_render *render, struct powervr_session *session, const struct powervr_target_shape *shape, struct powervr_target **result);
static int target_region_headers(struct powervr_session *session, uint64_t bytes, struct powervr_resource **result);
static int target_memory(struct powervr_session *session, uint64_t bytes, struct powervr_resource **result);
static void target_records(struct powervr_target *target, uint32_t global_list);
static int target_cleanup(struct powervr_render *render, struct powervr_target *target);
static void target_destroy(struct powervr_render *render, struct powervr_session *session, struct powervr_target *target);
static void target_discard(struct powervr_render *render, struct powervr_session *session, struct powervr_target *target);
static void resource_discard(struct powervr_render *render, struct powervr_session *session, struct powervr_resource *resource);
static int cleanup_object(struct powervr_render *render, uint32_t kind, uint32_t address);
static void notice_grow(struct powervr_render *render, const uint32_t notice[POWERVR_COMMAND_WORDS]);
static void notice_rebuild(struct powervr_render *render, const uint32_t notice[POWERVR_COMMAND_WORDS]);
static void notice_statistics(struct powervr_render *render, const uint32_t notice[POWERVR_COMMAND_WORDS]);
static void rebuild_list(struct powervr_free_list *list);
static struct powervr_gpu_space *program_space(struct powervr_session *session, enum powervr_program_heap heap);
static uint64_t ms_ticks(unsigned milliseconds);
static void describe_catalogue(struct powervr_render *render, uint64_t catalogue);
static void describe_listing(const struct powervr_free_list *list, uint32_t frame);
static void describe_half(struct powervr_render *render, const struct powervr_target *target, unsigned half);
static void describe_list(const char *name, const struct powervr_free_list *list);
static uint64_t target_word64(const struct powervr_target *target, uint32_t offset);

/*
 * Sets up a session's program windows.
 *
 * The data sequencer's and the shading cluster's start past the
 * firmware's static programs; their first ranges are the places the
 * Vulkan executor maps its programs at, held for it here so nothing
 * else is placed there.  The transfer window starts at its base.
 */
int
powervr_session_heaps_init(
	struct powervr_session *session)
{
	uint64_t address;
	int error;

	/* The three windows. */
	powervr_gpu_space_init(&session->pds_space, POWERVR_PDS_HEAP_BASE + POWERVR_PROGRAM_HEAP_START, POWERVR_PDS_HEAP_BYTES - POWERVR_PROGRAM_HEAP_START);
	powervr_gpu_space_init(&session->usc_space, POWERVR_USC_HEAP_BASE + POWERVR_PROGRAM_HEAP_START, POWERVR_USC_HEAP_BYTES - POWERVR_PROGRAM_HEAP_START);
	powervr_gpu_space_init(&session->transfer_space, POWERVR_TRANSFER_HEAP_BASE, POWERVR_TRANSFER_HEAP_BYTES);
	powervr_gpu_space_init(&session->region_space, POWERVR_REGION_HEADER_HEAP_BASE, POWERVR_REGION_HEADER_HEAP_BYTES);

	/* The executor's data sequencer programs: the window's first range. */
	error = powervr_gpu_space_take(&session->pds_space, POWERVR_EXECUTOR_PDS_BYTES, &address);
	if (error != 0)
		return error;
	if (address != POWERVR_PDS_HEAP_BASE + POWERVR_PROGRAM_HEAP_START)
		return EINVAL;

	/* The executor's shading cluster programs: likewise. */
	error = powervr_gpu_space_take(&session->usc_space, POWERVR_EXECUTOR_USC_BYTES, &address);
	if (error != 0)
		return error;
	if (address != POWERVR_USC_HEAP_BASE + POWERVR_PROGRAM_HEAP_START)
		return EINVAL;

	/* Succeeded: the windows are ready. */
	return 0;
}

/* Maps a buffer at a place of its own in one of a session's program windows. */
int
powervr_session_program_map(
	struct powervr_session *session,
	enum powervr_program_heap heap,
	struct powervr_buffer *buffer,
	uint64_t *address)
{
	struct powervr_render *render;
	struct powervr_gpu_space *space;
	unsigned flags;
	int error;

	/* The window, and the attributes of a buffer the processor writes. */
	render = session->render;
	space = program_space(session, heap);
	flags = powervr_gpu_buffer_flags(1, 0);
	if (POWERVR_PROGRAM_SLC_CACHED)
		flags = 0;

	/* Takes a place and maps the run there, then has the core drop stale table entries. */
	mutex_lock(&render->core->lock);

	error = powervr_gpu_space_take(space, buffer->run.size, address);
	if (error == 0) {
		error = powervr_gpu_mmu_map(&session->mmu, *address, buffer->run.paddr, buffer->run.size, flags);
		if (error != 0)
			(void)powervr_gpu_space_give(space, *address);
	}
	if (error == 0)
		error = powervr_command_mmu_flush(render->core);

	mutex_unlock(&render->core->lock);

	/* Not mapped. */
	if (error != 0)
		return error;

	/* Succeeded: the buffer is at its place. */
	return 0;
}

/* Unmaps a buffer from a session's program window and gives its place back. */
void
powervr_session_program_unmap(
	struct powervr_session *session,
	enum powervr_program_heap heap,
	struct powervr_buffer *buffer,
	uint64_t address)
{
	struct powervr_render *render;
	struct powervr_gpu_space *space;

	/* The window. */
	render = session->render;
	space = program_space(session, heap);

	/* Unmaps it, gives the place back and has the core drop the entries. */
	mutex_lock(&render->core->lock);

	powervr_gpu_mmu_unmap(&session->mmu, address, buffer->run.size);
	(void)powervr_gpu_space_give(space, address);
	(void)powervr_command_mmu_flush(render->core);

	mutex_unlock(&render->core->lock);
}

/*
 * Finds or makes a session's render target for a frame shape.
 *
 * The first call makes the session's global free list.  A kept target of
 * the same shape is reused; with every slot taken, the least recently
 * used target is cleaned up and replaced, and EBUSY says the firmware
 * still held it.
 */
int
powervr_session_target(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_target_shape *shape,
	struct powervr_target **result)
{
	struct powervr_targets *targets;
	struct powervr_target *target;
	unsigned oldest;
	unsigned index;
	uint64_t started;
	int error;

	/* The session's targets, made by its first render. */
	*result = NULL;
	targets = session->targets;
	if (targets == NULL) {
		targets = kern_calloc(1, sizeof(*targets));
		if (targets == NULL)
			return ENOMEM;
		session->targets = targets;
	}

	/* The global free list, made once. */
	if (!targets->global_made) {
		started = powervr_timing_now();
		error = list_make(render, session, POWERVR_FREE_LIST_GLOBAL_PAGES, NULL, &targets->global);
		powervr_timing_add(POWERVR_TIMING_LIST_MAKE, started);
		if (error != 0)
			return error;
		targets->global_made = 1;
	}

	/* A kept target of the same shape. */
	targets->renders++;
	for (index = 0; index < targets->count; index++) {
		target = targets->kept[index];
		if (target->shape.width != shape->width || target->shape.height != shape->height)
			continue;
		if (target->shape.samples != shape->samples || target->shape.layers != shape->layers)
			continue;

		/* Reused: it is the most recently used now. */
		target->used = targets->renders;
		*result = target;
		return 0;
	}

	/* Every slot taken: the least recently used target goes, once the firmware lets it go. */
	started = powervr_timing_now();
	if (targets->count == TARGETS_MAX) {
		oldest = 0;
		for (index = 1; index < targets->count; index++) {
			if (targets->kept[index]->used < targets->kept[oldest]->used)
				oldest = index;
		}
		error = target_cleanup(render, targets->kept[oldest]);
		if (error != 0)
			return error;
		target_destroy(render, session, targets->kept[oldest]);
		targets->count--;
		targets->kept[oldest] = targets->kept[targets->count];
		targets->kept[targets->count] = NULL;
	}

	/* A new target. */
	error = target_make(render, session, shape, &target);
	powervr_timing_add(POWERVR_TIMING_TARGET_MAKE, started);
	if (error != 0)
		return error;

	/* Kept, as the most recently used. */
	target->used = targets->renders;
	targets->kept[targets->count] = target;
	targets->count++;
	*result = target;
	return 0;
}

/* Reports the firmware address of one half of a render target's data set. */
uint32_t
powervr_target_data_address(
	const struct powervr_target *target,
	unsigned half)
{
	uint32_t address;

	/* The half's record in the target's block. */
	address = powervr_object_address(&target->record, POWERVR_TARGET_DATA_OFFSET(half));

	/* Succeeded: the address. */
	return address;
}

/* Reports the layout a render target was made with. */
const struct powervr_target_layout *
powervr_target_layout_of(
	const struct powervr_target *target)
{
	/* The target's own. */
	return &target->layout;
}

/* Reports the firmware address of the cleanup count of one half of a render target. */
uint32_t
powervr_target_cleanup_address(
	const struct powervr_target *target,
	unsigned half)
{
	uint32_t address;

	/* The count inside the half's record. */
	address = powervr_object_address(&target->record, POWERVR_TARGET_DATA_OFFSET(half) + (uint32_t)offsetof(struct powervr_fw_target_data, cleanup));

	/* Succeeded: the address. */
	return address;
}

/*
 * Has the firmware clean up every render target and free list of a
 * session: each target's halves and local list first, then the global
 * list, as the targets use it.
 */
int
powervr_session_targets_cleanup(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_targets *targets;
	unsigned index;
	int error;

	/* A session that never rendered has nothing the firmware knows. */
	targets = session->targets;
	if (targets == NULL)
		return 0;

	/* Each target. */
	for (index = 0; index < targets->count; index++) {
		error = target_cleanup(render, targets->kept[index]);
		if (error != 0)
			return error;
	}

	/* The global list, which no target uses any more. */
	if (targets->global_made && !targets->global.cleaned) {
		error = cleanup_object(render, POWERVR_CLEANUP_FREE_LIST, powervr_object_address(&targets->global.record, 0));
		if (error != 0)
			return error;
		targets->global.cleaned = 1;
	}

	/* Succeeded: the firmware no longer uses any of them. */
	return 0;
}

/* Returns a session's render targets and free lists once the firmware no longer uses them. */
void
powervr_session_targets_destroy(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_targets *targets;
	unsigned index;

	/* Nothing made. */
	targets = session->targets;
	if (targets == NULL)
		return;

	/* Each target, then the global list. */
	for (index = 0; index < targets->count; index++)
		target_destroy(render, session, targets->kept[index]);
	if (targets->global_made)
		list_destroy(render, session, &targets->global);

	/* The bookkeeping. */
	kern_free(targets);
	session->targets = NULL;
}

/*
 * Returns a kept session's render targets and free lists once the
 * firmware that knew them is gone.
 *
 * Their records' old places in the firmware's heap and their mappings
 * are not touched; the memory goes, or joins the buffers kept for the
 * reset.
 */
void
powervr_session_targets_discard(
	struct powervr_session *session)
{
	struct powervr_render *render;
	struct powervr_targets *targets;
	unsigned index;

	/* Nothing made. */
	render = session->render;
	targets = session->targets;
	if (targets == NULL)
		return;

	/* Each target, then the global list. */
	for (index = 0; index < targets->count; index++)
		target_discard(render, session, targets->kept[index]);
	if (targets->global_made)
		list_discard(render, session, &targets->global);

	/* The bookkeeping. */
	kern_free(targets);
	session->targets = NULL;
}

/* Acts on a free list notice of the firmware. */
void
powervr_targets_notice(
	struct powervr_render *render,
	uint32_t type,
	const struct powervr_object *buffer,
	uint32_t offset)
{
	uint32_t notice[POWERVR_COMMAND_WORDS];
	uint32_t index;

	/* The notice's words. */
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		notice[index] = powervr_object_get32(buffer, offset + index * 4U);

	/* A grow request, a rebuild request or the statistics. */
	if (type == POWERVR_NOTICE_FREE_LIST_GROW) {
		notice_grow(render, notice);
	} else if (type == POWERVR_NOTICE_FREE_LIST_REBUILD) {
		notice_rebuild(render, notice);
	} else if (type == POWERVR_NOTICE_STATISTICS) {
		notice_statistics(render, notice);
	}
}

/* Copies what the firmware reported of the free lists. */
void
powervr_render_target_events(
	const struct powervr_render *render,
	struct powervr_target_events *events)
{
	/* The counts as they stand. */
	*events = render->target_events;
}

/*
 * Makes a free list of a number of pages: the pages, the list of their
 * numbers mapped in the session's general window, and the firmware's
 * record, listed by a new identity.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_create, pvr_free_list_grow,
 * pvr_free_list_insert_node_locked: the pages' numbers from the list's
 * start when it is full from the start); Mesa 26.2.5
 * src/imagination/vulkan/pvr_free_list.c (pvr_free_list_create: the
 * list's buffer one entry per page, uncached for the core).
 */
static int
list_make(
	struct powervr_render *render,
	struct powervr_session *session,
	uint32_t pages,
	struct powervr_target *owner,
	struct powervr_free_list *list)
{
	struct powervr_fw_free_list record;
	struct powervr_buffer *entries;
	int error;

	/*
	 * The pages the parameter manager takes from, zeroed (with pages of
	 * the pool used as they were, or not cleared, the core locked up on
	 * the board).  A global list takes the spare made ahead by
	 * its own thread; a local list, or a global one with none ready, is
	 * made here.
	 */
	list->owner = owner;
	list->page_count = pages;
	error = ENOENT;
	if (owner == NULL && (uint64_t)pages * POWERVR_PM_PAGE_BYTES == POWERVR_SPARE_BYTES)
		error = powervr_spare_take(render, &list->pages);
	if (error != 0)
		error = powervr_buffer_create(render, (uint64_t)pages * POWERVR_PM_PAGE_BYTES, &list->pages);
	if (error != 0)
		return error;

	/* The list of their numbers, mapped where the core reads it; the handle takes the buffer. */
	error = powervr_buffer_create(render, (uint64_t)pages * POWERVR_FREE_LIST_ENTRY_BYTES, &entries);
	if (error != 0)
		goto fail;
	error = powervr_resource_make_free_list(session, entries, &list->entries);
	powervr_buffer_put(render, entries);
	if (error != 0)
		goto fail;
	list_write_entries(list);

	/* The firmware's record under a new identity, listed so the firmware's requests find it. */
	mutex_lock(&render->core->lock);

	list->identity = render->next_free_list++;
	error = powervr_object_create(&render->core->firmware.heap, (uint32_t)sizeof(record), POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &list->record);
	if (error == 0) {
		powervr_free_list_record(list->entries->address, pages, list->identity, &record);
		powervr_object_fill(&list->record, 0, (const uint8_t *)&record, sizeof(record));
		list->next = render->free_lists;
		render->free_lists = list;
		list->listed = 1;
	}

	mutex_unlock(&render->core->lock);

	/* No room for the record. */
	if (error != 0)
		goto fail;

	/* Succeeded: the list is the firmware's to use. */
	return 0;

fail:
	/* Gives back what was made and reports the failure. */
	if (list->entries != NULL)
		powervr_resource_unmake(session, list->entries);
	list->entries = NULL;
	powervr_buffer_put(render, list->pages);
	list->pages = NULL;
	return error;
}

/* Writes a free list's page list: every page's number, in order. */
static void
list_write_entries(
	struct powervr_free_list *list)
{
	volatile uint32_t *entries;
	uint32_t index;

	/* One entry per page, through the uncached view. */
	entries = (volatile uint32_t *)list->entries->buffer->view;
	for (index = 0; index < list->page_count; index++)
		entries[index] = powervr_free_list_entry(list->pages->run.paddr, index);
}

/* Takes a free list off the render node's list; the core's lock is held. */
static void
list_unlist(
	struct powervr_render *render,
	struct powervr_free_list *list)
{
	struct powervr_free_list **link;

	/* Not listed. */
	if (!list->listed)
		return;

	/* Finds it and unlinks it. */
	for (link = &render->free_lists; *link != NULL; link = &(*link)->next) {
		if (*link == list) {
			*link = list->next;
			break;
		}
	}
	list->listed = 0;
}

/* Returns a free list the firmware no longer uses. */
static void
list_destroy(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_free_list *list)
{
	/* Unlisted, and its record unmapped from the firmware's processor. */
	mutex_lock(&render->core->lock);

	list_unlist(render, list);
	powervr_object_destroy(&render->core->firmware.heap, &list->record);

	mutex_unlock(&render->core->lock);

	/* The page list's handle and the pages. */
	if (list->entries != NULL)
		powervr_resource_unmake(session, list->entries);
	list->entries = NULL;
	if (list->pages != NULL)
		powervr_buffer_put(render, list->pages);
	list->pages = NULL;
}

/* Returns a kept free list once the firmware is gone; the core's lock is held. */
static void
list_discard(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_free_list *list)
{
	/* Unlisted; the record's run goes, its old place is not touched. */
	list_unlist(render, list);
	powervr_object_drop(&list->record);

	/* The page list's handle and the pages. */
	if (list->entries != NULL)
		resource_discard(render, session, list->entries);
	list->entries = NULL;
	if (list->pages != NULL)
		powervr_buffer_put(render, list->pages);
	list->pages = NULL;
}

/* Finds a listed free list by its identity; the core's lock is held. */
static struct powervr_free_list *
list_find(
	struct powervr_render *render,
	uint32_t identity)
{
	struct powervr_free_list *list;

	/* The render node's list. */
	for (list = render->free_lists; list != NULL; list = list->next) {
		if (list->identity == identity)
			return list;
	}

	/* No such list. */
	return NULL;
}

/*
 * Makes a render target: its layout, its local free list, its memories
 * and its records.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_arch_render_target_dataset_create: the local free list of the
 * minimum size, the vertex heap table, the tail pointer cache, then the
 * macrolists and region headers of both halves, each pair in one
 * buffer; the region headers in the general window on a core without
 * the region header heap's quirk); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_hwrt.c (pvr_hwrt_dataset_create: the
 * shared record, then each half's).
 */
static int
target_make(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_target_shape *shape,
	struct powervr_target **result)
{
	struct powervr_target *target;
	int error;

	/* The bookkeeping and the layout. */
	*result = NULL;
	target = kern_calloc(1, sizeof(*target));
	if (target == NULL)
		return ENOMEM;
	target->shape = *shape;
	error = powervr_target_lay_out(shape, POWERVR_FREE_LIST_LOCAL_PAGES, POWERVR_FREE_LIST_GLOBAL_PAGES, &target->layout);
	if (error != 0)
		goto fail;

	/* The local free list. */
	error = list_make(render, session, POWERVR_FREE_LIST_LOCAL_PAGES, target, &target->local);
	if (error != 0)
		goto fail;

	/* The memories: two halves' macrolists and region headers, the shared vertex heap and tails. */
	error = target_memory(session, 2U * target->layout.macrolist_bytes, &target->macrolists);
	if (error != 0)
		goto fail;
	error = target_region_headers(session, 2U * target->layout.region_header_bytes, &target->region_headers);
	if (error != 0)
		goto fail;
	error = target_memory(session, target->layout.vertex_heap_bytes, &target->vertex_heap);
	if (error != 0)
		goto fail;
	error = target_memory(session, target->layout.tail_bytes, &target->tails);
	if (error != 0)
		goto fail;

	/* The firmware's block of both halves and their shared record. */
	mutex_lock(&render->core->lock);

	error = powervr_object_create(&render->core->firmware.heap, POWERVR_TARGET_RECORD_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &target->record);
	if (error == 0)
		target_records(target, powervr_object_address(&session->targets->global.record, 0));

	mutex_unlock(&render->core->lock);

	/* No room for the block. */
	if (error != 0)
		goto fail;

	/* The diagnostics: the target's record and the physical pages its lists give the parameter manager (whose catalogues come from them). */
	if (POWERVR_DIAGNOSTICS) {
		kern_logf("powervr: target made: session 0x%x record 0x%x, local list 0x%llx..0x%llx, global list 0x%llx..0x%llx\n",
			  (unsigned)session->identity, (unsigned)powervr_object_address(&target->record, 0),
			  (unsigned long long)target->local.pages->run.paddr,
			  (unsigned long long)(target->local.pages->run.paddr + target->local.pages->run.size),
			  (unsigned long long)(session->targets->global.pages != NULL ? session->targets->global.pages->run.paddr : 0U),
			  (unsigned long long)(session->targets->global.pages != NULL ? session->targets->global.pages->run.paddr + session->targets->global.pages->run.size : 0U));
	}

	/* Succeeded: the target is ready for its first render. */
	*result = target;
	return 0;

fail:
	/* Destroys the part-made target and reports the failure. */
	target_destroy(render, session, target);
	return error;
}

/* Makes a render target's region headers, zeroed and mapped in the session's region header window. */
static int
target_region_headers(
	struct powervr_session *session,
	uint64_t bytes,
	struct powervr_resource **result)
{
	struct powervr_buffer *buffer;
	int error;

	/* The zeroed buffer. */
	*result = NULL;
	error = powervr_buffer_create(session->render, bytes, &buffer);
	if (error != 0)
		return error;

	/* The session's handle on it in the region header window; the handle takes the creation reference. */
	error = powervr_resource_make_region_headers(session, buffer, result);
	powervr_buffer_put(session->render, buffer);

	/* Reports a window that could not take it. */
	if (error != 0)
		return error;

	/* Succeeded: the memory is mapped. */
	return 0;
}

/* Makes one memory of a render target, zeroed, uncached for the core and mapped in the session's general window. */
static int
target_memory(
	struct powervr_session *session,
	uint64_t bytes,
	struct powervr_resource **result)
{
	struct powervr_buffer *buffer;
	int error;

	/* The zeroed buffer. */
	*result = NULL;
	error = powervr_buffer_create(session->render, bytes, &buffer);
	if (error != 0)
		return error;

	/* The session's handle on it; the handle takes the creation reference. */
	error = powervr_resource_make(session, buffer, 1, 0, result);
	powervr_buffer_put(session->render, buffer);
	if (error != 0)
		return error;

	/* Succeeded: the memory is mapped. */
	return 0;
}

/* Fills a render target's records in its block; the core's lock is held. */
static void
target_records(
	struct powervr_target *target,
	uint32_t global_list)
{
	struct powervr_fw_target_common common;
	struct powervr_fw_target_data data;
	struct powervr_target_places places;
	unsigned half;

	/* The shared record. */
	powervr_target_common_record(&target->layout, &common);
	powervr_object_fill(&target->record, POWERVR_TARGET_COMMON_OFFSET, (const uint8_t *)&common, sizeof(common));

	/* What both halves are told alike. */
	places.common = powervr_object_address(&target->record, POWERVR_TARGET_COMMON_OFFSET);
	places.local_list = powervr_object_address(&target->local.record, 0);
	places.global_list = global_list;
	places.vertex_heap = target->vertex_heap->address;
	places.tail = target->tails->address;

	/* Each half: its own macrolist and region headers. */
	for (half = 0; half < 2U; half++) {
		places.macrolist = target->macrolists->address + half * target->layout.macrolist_bytes;
		places.region_headers = target->region_headers->address + half * target->layout.region_header_bytes;
		powervr_target_data_record(&places, &data);
		powervr_object_fill(&target->record, POWERVR_TARGET_DATA_OFFSET(half), (const uint8_t *)&data, sizeof(data));
	}
}

/*
 * Has the firmware clean up a render target: both halves, then its local
 * free list.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_hwrt.c
 * (pvr_hwrt_dataset_release: each half's cleanup, the last first) and
 * pvr_free_list.c (pvr_free_list_release).
 */
static int
target_cleanup(
	struct powervr_render *render,
	struct powervr_target *target)
{
	unsigned half;
	int error;

	/* Already cleaned up. */
	if (target->cleaned)
		return 0;

	/* Each half, the last first. */
	for (half = 2U; half > 0U; half--) {
		error = cleanup_object(render, POWERVR_CLEANUP_TARGET_DATA, powervr_target_data_address(target, half - 1U));
		if (error != 0)
			return error;
	}

	/* The local free list. */
	error = cleanup_object(render, POWERVR_CLEANUP_FREE_LIST, powervr_object_address(&target->local.record, 0));
	if (error != 0)
		return error;

	/* Succeeded: the firmware no longer uses the target. */
	target->local.cleaned = 1;
	target->cleaned = 1;
	return 0;
}

/* Returns a render target the firmware no longer uses (or never knew). */
static void
target_destroy(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_target *target)
{
	/* The block of records. */
	mutex_lock(&render->core->lock);

	powervr_object_destroy(&render->core->firmware.heap, &target->record);

	mutex_unlock(&render->core->lock);

	/* The memories. */
	if (target->tails != NULL)
		powervr_resource_unmake(session, target->tails);
	if (target->vertex_heap != NULL)
		powervr_resource_unmake(session, target->vertex_heap);
	if (target->region_headers != NULL)
		powervr_resource_unmake(session, target->region_headers);
	if (target->macrolists != NULL)
		powervr_resource_unmake(session, target->macrolists);

	/* The local free list, if it was made, and the bookkeeping. */
	if (target->local.pages != NULL)
		list_destroy(render, session, &target->local);
	kern_free(target);
}

/* Returns a kept render target once the firmware is gone; the core's lock is held. */
static void
target_discard(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_target *target)
{
	/* The block's run goes, its old place is not touched. */
	powervr_object_drop(&target->record);

	/* The memories. */
	if (target->tails != NULL)
		resource_discard(render, session, target->tails);
	if (target->vertex_heap != NULL)
		resource_discard(render, session, target->vertex_heap);
	if (target->region_headers != NULL)
		resource_discard(render, session, target->region_headers);
	if (target->macrolists != NULL)
		resource_discard(render, session, target->macrolists);

	/* The local free list and the bookkeeping. */
	if (target->local.pages != NULL)
		list_discard(render, session, &target->local);
	kern_free(target);
}

/*
 * Drops a kept session's handle without touching its mapping, whose
 * tables go with the session; after a fault the buffer joins those kept
 * for the reset.
 */
static void
resource_discard(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_resource *resource)
{
	/* The handle's hold on the buffer, and the handle. */
	powervr_buffer_put(render, resource->buffer);
	session->resources--;
	kern_free(resource);
}

/*
 * Has the firmware clean up one of its objects and waits for it, a
 * bounded number of times and no longer than the limit in all; EBUSY
 * when it stays busy.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c
 * (pvr_fw_structure_cleanup: the cleanup command, a bounded wait on its
 * return slot, busy when the slot says so) and pvr_free_list.c
 * (pvr_free_list_release: asked again while busy).
 */
static int
cleanup_object(
	struct powervr_render *render,
	uint32_t kind,
	uint32_t address)
{
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint64_t deadline;
	uint32_t answer;
	unsigned tries;
	uint64_t now;
	int error;

	/* The command, and when asking again stops. */
	powervr_cleanup_command(kind, address, command);
	deadline = sched_ticks() + ms_ticks(CLEANUP_LIMIT_MS);

	/* Sends it until the firmware says the object is idle. */
	error = EBUSY;
	for (tries = 0; tries < CLEANUP_TRIES; tries++) {
		mutex_lock(&render->core->lock);

		error = powervr_command_run(render->core, command, POWERVR_COMMAND_WAIT_MS, &answer);

		mutex_unlock(&render->core->lock);

		/* The firmware did not answer. */
		if (error != 0)
			return error;

		/* The object is idle and forgotten. */
		if ((answer & POWERVR_RETURN_CLEANUP_BUSY) == 0U)
			break;

		/* Still busy: out of time, or a pause before asking again. */
		error = EBUSY;
		now = sched_ticks();
		if (now >= deadline)
			break;
		sched_sleep(now + ms_ticks(CLEANUP_PAUSE_MS));
	}

	/* The object stayed busy. */
	if (error != 0) {
		kern_logf("powervr: cleanup of a render object stayed busy\n");
		return error;
	}

	/* Succeeded: the firmware no longer uses the object. */
	return 0;
}

/*
 * Answers a grow request: the list does not grow, so the answer adds
 * nothing, and the request is counted for the render that ran out.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_process_grow_req: no answer for a list it does not
 * know, else one answer sent without waiting).
 */
static void
notice_grow(
	struct powervr_render *render,
	const uint32_t notice[POWERVR_COMMAND_WORDS])
{
	struct powervr_free_list *list;
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t identity;
	uint32_t slot;
	int error;

	/* The list it names. */
	error = powervr_grow_request(notice, &identity);
	if (error != 0)
		return;
	list = list_find(render, identity);
	if (list == NULL) {
		kern_logf("powervr: grow request for an unknown free list %u\n", identity);
		return;
	}

	/* Counted and said: the render on it ran out of pages. */
	list->grow_requests++;
	render->target_events.grow_requests++;
	kern_logf("powervr: free list %u (%u pages) asked to grow (%u times); answered with nothing added\n",
		  (unsigned)identity, (unsigned)list->page_count, (unsigned)list->grow_requests);

	/* The answer: nothing added, nothing held ready. */
	powervr_grow_answer(powervr_object_address(&list->record, 0), list->page_count, command);
	error = powervr_command_send(render->core, command, &slot);
	if (error != 0)
		kern_logf("powervr: grow answer not sent (%d)\n", error);
}

/*
 * Answers a rebuild request after a recovery: each list it names gets
 * its pages back, the render targets on a local list are told they were
 * recovered, and the answer names the lists of the request.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_process_reconstruct_req, pvr_free_list_reconstruct).
 */
static void
notice_rebuild(
	struct powervr_render *render,
	const uint32_t notice[POWERVR_COMMAND_WORDS])
{
	struct powervr_free_list *list;
	uint32_t identities[POWERVR_REBUILD_LISTS_MAX];
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t count;
	uint32_t index;
	uint32_t slot;
	int error;

	/* The lists it names. */
	error = powervr_rebuild_request(notice, identities, &count);
	if (error != 0)
		return;

	/* Each one the node knows; a debug kernel says first how the firmware left it. */
	for (index = 0; index < count; index++) {
		list = list_find(render, identities[index]);
		if (list == NULL)
			continue;
		if (POWERVR_DEBUG)
			describe_list("rebuilt", list);
		rebuild_list(list);
	}
	render->target_events.rebuilds++;

	/* The answer. */
	powervr_rebuild_answer(identities, count, command);
	error = powervr_command_send(render->core, command, &slot);
	if (error != 0)
		kern_logf("powervr: rebuild answer not sent (%d)\n", error);
}

/*
 * Counts the partial renders and the out-of-memory events the
 * firmware's statistics report; other counters are not kept.
 */
static void
notice_statistics(
	struct powervr_render *render,
	const uint32_t notice[POWERVR_COMMAND_WORDS])
{
	uint32_t counter;
	int32_t adjustment;
	int error;

	/* The counter and the adjustment. */
	error = powervr_statistics_notice(notice, &counter, &adjustment);
	if (error != 0)
		return;

	/* Raises the one it names, and says it. */
	if (counter == POWERVR_STATISTICS_PARTIAL) {
		render->target_events.partial_renders += (unsigned)adjustment;
		kern_logf("powervr: partial render (%u so far)\n", render->target_events.partial_renders);
	} else if (counter == POWERVR_STATISTICS_OUT_OF_MEMORY) {
		render->target_events.out_of_memory += (unsigned)adjustment;
		kern_logf("powervr: parameter memory ran out (%u so far)\n", render->target_events.out_of_memory);
	}
}

/*
 * Rebuilds a free list: every page back on its list, its stack full and
 * nothing allocated, and a local list's target marked recovered with no
 * last geometry, so the next geometry is not taken for a first one.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_reconstruct).
 */
static void
rebuild_list(
	struct powervr_free_list *list)
{
	struct powervr_target *target;
	uint32_t current;
	uint32_t flags;
	uint32_t place;
	unsigned half;

	/* The page list, and the stack over the current pages with nothing taken. */
	list_write_entries(list);
	current = powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, current_pages));
	powervr_object_put32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, stack_top), current - 1U);
	powervr_object_put32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_pages), 0U);
	powervr_object_put32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_table_pages), 0U);

	/* The global list belongs to no target. */
	target = list->owner;
	if (target == NULL)
		return;

	/* Each half of its target: recovered, no last geometry. */
	for (half = 0; half < 2U; half++) {
		place = POWERVR_TARGET_DATA_OFFSET(half);
		powervr_object_put32(&target->record, place + (uint32_t)offsetof(struct powervr_fw_target_data, state), POWERVR_TARGET_STATE_RECOVERED);
		flags = powervr_object_get32(&target->record, place + (uint32_t)offsetof(struct powervr_fw_target_data, flags));
		powervr_object_put32(&target->record, place + (uint32_t)offsetof(struct powervr_fw_target_data, flags), flags & ~POWERVR_TARGET_LAST_GEOMETRY);
	}
}

/* Finds a session's space of one program window. */
static struct powervr_gpu_space *
program_space(
	struct powervr_session *session,
	enum powervr_program_heap heap)
{
	/* The window asked for. */
	if (heap == POWERVR_PROGRAM_PDS)
		return &session->pds_space;
	if (heap == POWERVR_PROGRAM_USC)
		return &session->usc_space;

	/* The transfer window. */
	return &session->transfer_space;
}

/* Converts milliseconds to scheduler ticks, rounding up. */
static uint64_t
ms_ticks(
	unsigned milliseconds)
{
	/* Rounds up, so a short span is still a tick. */
	return kern_ms_to_ticks(milliseconds);
}

/* Reads the parameter manager's catalogues (VCE 0 to 3, TE 0 to 3, ALIST) from one half of a target's record. */
void
powervr_target_catalogues(
	const struct powervr_target *target,
	unsigned half,
	uint64_t catalogues[POWERVR_TARGET_CATALOGUES])
{
	static const uint32_t places[POWERVR_TARGET_CATALOGUES] = {
		8U, 16U, 24U, 32U, 72U, 80U, 88U, 96U, 136U,
	};
	uint32_t base;
	uint32_t index;

	/* Each 64-bit word, low half first. */
	base = POWERVR_TARGET_DATA_OFFSET(half);
	for (index = 0; index < POWERVR_TARGET_CATALOGUES; index++) {
		catalogues[index] = (uint64_t)powervr_object_get32(&target->record, base + places[index]) |
		    (uint64_t)powervr_object_get32(&target->record, base + places[index] + 4U) << 32;
	}
}

/* Takes the half of a target the next render uses, and makes the other half the one after it. */
unsigned
powervr_target_take_half(
	struct powervr_target *target)
{
	unsigned half;

	/* This render's half, the next render's the other. */
	half = target->next_half;
	target->next_half = half ^ 1U;

	/* Succeeded: the half. */
	return half;
}

/* Reports the firmware address of a target's render target data record. */
uint32_t
powervr_target_record_address(
	const struct powervr_target *target)
{
	uint32_t address;

	/* The record's place in the firmware's heap. */
	address = powervr_object_address(&target->record, 0);

	/* Succeeded: the address. */
	return address;
}

/*
 * Prints a target's records for a render the firmware reset: the
 * buffers' device addresses, the first half's data record, the common
 * record and the local free list's record, eight words to a line.
 */
void
powervr_target_dump(
	const struct powervr_target *target)
{
	uint32_t bytes;
	uint32_t offset;

	/* The buffers the parameter manager and the tiler use. */
	kern_logf("powervr: target: macrotiles 0x%llx, region headers 0x%llx, vertex heap 0x%llx, tails 0x%llx, local list 0x%llx (%u pages from 0x%llx)\n",
		  (unsigned long long)(target->macrolists != NULL ? target->macrolists->address : 0U),
		  (unsigned long long)(target->region_headers != NULL ? target->region_headers->address : 0U),
		  (unsigned long long)(target->vertex_heap != NULL ? target->vertex_heap->address : 0U),
		  (unsigned long long)(target->tails != NULL ? target->tails->address : 0U),
		  (unsigned long long)(target->local.entries != NULL ? target->local.entries->address : 0U),
		  (unsigned)target->local.page_count,
		  (unsigned long long)(target->local.pages != NULL ? target->local.pages->run.paddr : 0U));

	/* The record: the first half's data, then the common part. */
	bytes = POWERVR_TARGET_RECORD_BYTES;
	for (offset = 0; offset < bytes; offset += 32U) {
		kern_logf("powervr: target: record +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)offset,
			  (unsigned)powervr_object_get32(&target->record, offset), (unsigned)powervr_object_get32(&target->record, offset + 4U),
			  (unsigned)powervr_object_get32(&target->record, offset + 8U), (unsigned)powervr_object_get32(&target->record, offset + 12U),
			  (unsigned)powervr_object_get32(&target->record, offset + 16U), (unsigned)powervr_object_get32(&target->record, offset + 20U),
			  (unsigned)powervr_object_get32(&target->record, offset + 24U), (unsigned)powervr_object_get32(&target->record, offset + 28U));
	}

	/* The local free list's record. */
	for (offset = 0; offset < (uint32_t)sizeof(struct powervr_fw_free_list); offset += 32U) {
		kern_logf("powervr: target: local list +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)offset,
			  (unsigned)powervr_object_get32(&target->local.record, offset), (unsigned)powervr_object_get32(&target->local.record, offset + 4U),
			  (unsigned)powervr_object_get32(&target->local.record, offset + 8U), (unsigned)powervr_object_get32(&target->local.record, offset + 12U),
			  (unsigned)powervr_object_get32(&target->local.record, offset + 16U), (unsigned)powervr_object_get32(&target->local.record, offset + 20U),
			  (unsigned)powervr_object_get32(&target->local.record, offset + 24U), (unsigned)powervr_object_get32(&target->local.record, offset + 28U));
	}
}

/* Prints a session's global free list's record, eight words to a line, when it was made. */
void
powervr_session_global_list_dump(
	const struct powervr_session *session)
{
	const struct powervr_targets *targets;
	const struct powervr_free_list *list;
	uint32_t offset;

	/* Nothing without the list. */
	targets = session->targets;
	if (targets == NULL || !targets->global_made) {
		kern_logf("powervr: target: no global list\n");
		return;
	}

	/* Its pages, then its record. */
	list = &targets->global;
	kern_logf("powervr: target: global list 0x%x: entries 0x%llx (%u pages from 0x%llx)\n",
		  (unsigned)powervr_object_address(&list->record, 0),
		  (unsigned long long)(list->entries != NULL ? list->entries->address : 0U),
		  (unsigned)list->page_count,
		  (unsigned long long)(list->pages != NULL ? list->pages->run.paddr : 0U));

	/* The record, eight words a line. */
	for (offset = 0; offset < (uint32_t)sizeof(struct powervr_fw_free_list); offset += 32U) {
		kern_logf("powervr: target: global list +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)offset,
			  (unsigned)powervr_object_get32(&list->record, offset), (unsigned)powervr_object_get32(&list->record, offset + 4U),
			  (unsigned)powervr_object_get32(&list->record, offset + 8U), (unsigned)powervr_object_get32(&list->record, offset + 12U),
			  (unsigned)powervr_object_get32(&list->record, offset + 16U), (unsigned)powervr_object_get32(&list->record, offset + 20U),
			  (unsigned)powervr_object_get32(&list->record, offset + 24U), (unsigned)powervr_object_get32(&list->record, offset + 28U));
	}
}

/*
 * Prints what the render targets know of a recovery: for a catalogue
 * (the page a parameter manager catalogue base named in a BIF fault, 0
 * for none), every free list whose pages hold it or whose entries list
 * its page with the page's words; then the target half at the data set
 * address the recovery names, with its record's parameter manager words,
 * its macrolist's first entries and its two lists' records.  The core's
 * lock is held (a reset notice), so the listed free lists and their
 * targets stay.
 */
void
powervr_targets_describe_fault(
	struct powervr_render *render,
	uint32_t target_data,
	uint64_t catalogue)
{
	struct powervr_free_list *list;
	unsigned half;

	/* Nothing without the render node. */
	if (render == NULL)
		return;

	/* The catalogue's page, when the fault named one. */
	if (catalogue != 0U)
		describe_catalogue(render, catalogue);

	/* The half at the data set address: each local list's target, both halves. */
	for (list = render->free_lists; list != NULL; list = list->next) {
		if (list->owner == NULL)
			continue;
		for (half = 0; half < 2U; half++) {
			if (powervr_target_data_address(list->owner, half) == target_data)
				describe_half(render, list->owner, half);
		}
	}
}

/*
 * Prints the free lists a catalogue page belongs to: each whose pages
 * hold it, with the page's first words and how many of its words are
 * set (read through the uncached view, as the core sees them), and each
 * whose entries name its page (a page two lists hand out is a page two
 * parameter managers write).
 */
static void
describe_catalogue(
	struct powervr_render *render,
	uint64_t catalogue)
{
	struct powervr_free_list *list;
	volatile const uint32_t *words;
	uint64_t page;
	uint32_t frame;
	uint32_t index;
	unsigned set;
	unsigned held;

	/* The page and its parameter manager number. */
	page = catalogue & ~(uint64_t)(POWERVR_PM_PAGE_BYTES - 1U);
	frame = (uint32_t)(page >> POWERVR_PM_PAGE_SHIFT);
	held = 0;

	/* Each listed free list: whether its pages hold the page, and whether its entries name it. */
	for (list = render->free_lists; list != NULL; list = list->next) {
		if (list->pages != NULL && page >= list->pages->run.paddr && page < list->pages->run.paddr + list->pages->run.size) {
			/* The page's words as the core reads them. */
			words = (volatile const uint32_t *)(list->pages->view + (page - list->pages->run.paddr));
			set = 0;
			for (index = 0; index < POWERVR_PM_PAGE_BYTES / 4U; index++) {
				if (words[index] != 0U)
					set++;
			}
			held++;
			kern_logf("powervr: hwr: catalogue 0x%llx is page %u of free list %u (%s, %u pages); %u of %u words set: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				  (unsigned long long)catalogue, (unsigned)((page - list->pages->run.paddr) >> POWERVR_PM_PAGE_SHIFT),
				  (unsigned)list->identity, list->owner != NULL ? "local" : "global", (unsigned)list->page_count,
				  set, (unsigned)(POWERVR_PM_PAGE_BYTES / 4U), (unsigned)words[0], (unsigned)words[1], (unsigned)words[2],
				  (unsigned)words[3], (unsigned)words[4], (unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
		}
		describe_listing(list, frame);
	}

	/* A page no list holds came from elsewhere. */
	if (held == 0U)
		kern_logf("powervr: hwr: catalogue 0x%llx is in no free list's pages\n", (unsigned long long)catalogue);
}

/* Prints where a free list's entries name a page, and its stack top, when they do. */
static void
describe_listing(
	const struct powervr_free_list *list,
	uint32_t frame)
{
	volatile const uint32_t *entries;
	uint32_t index;
	uint32_t first;
	uint32_t last;
	unsigned count;

	/* Nothing without the entries. */
	if (list->entries == NULL)
		return;

	/* Every entry naming the page: how many, the first and the last. */
	entries = (volatile const uint32_t *)list->entries->buffer->view;
	count = 0;
	first = 0;
	last = 0;
	for (index = 0; index < list->page_count; index++) {
		if (entries[index] != frame)
			continue;
		if (count == 0U)
			first = index;
		last = index;
		count++;
	}

	/* Said only when listed. */
	if (count == 0U)
		return;
	kern_logf("powervr: hwr: free list %u (%s, %u pages) names the page %u time(s), entries %u..%u; stack top %u, allocated %u, tables %u\n",
		  (unsigned)list->identity, list->owner != NULL ? "local" : "global", (unsigned)list->page_count, count,
		  (unsigned)first, (unsigned)last,
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, stack_top)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_pages)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_table_pages)));
}

/*
 * Prints one target half the recovery named: its record's parameter
 * manager words (each 64-bit catalogue, its last one, the lists'
 * stacks, flags and state), its macrolist's first entries, and its
 * local and global lists.
 */
static void
describe_half(
	struct powervr_render *render,
	const struct powervr_target *target,
	unsigned half)
{
	const struct powervr_free_list *list;
	volatile const uint32_t *macrolist;
	uint32_t base;
	uint32_t global;

	/* The half and its catalogues: VCE 0..3 and their last ones, TE 0..3 and theirs, the ALIST and its last. */
	base = POWERVR_TARGET_DATA_OFFSET(half);
	kern_logf("powervr: hwr: target 0x%x %ux%u half %u (next %u): vce %llx %llx %llx %llx last %llx; te %llx %llx %llx %llx last %llx %llx\n",
		  (unsigned)powervr_object_address(&target->record, 0), (unsigned)target->shape.width, (unsigned)target->shape.height,
		  half, target->next_half,
		  (unsigned long long)target_word64(target, base + 8U), (unsigned long long)target_word64(target, base + 16U),
		  (unsigned long long)target_word64(target, base + 24U), (unsigned long long)target_word64(target, base + 32U),
		  (unsigned long long)target_word64(target, base + 40U),
		  (unsigned long long)target_word64(target, base + 72U), (unsigned long long)target_word64(target, base + 80U),
		  (unsigned long long)target_word64(target, base + 88U), (unsigned long long)target_word64(target, base + 96U),
		  (unsigned long long)target_word64(target, base + 104U), (unsigned long long)target_word64(target, base + 112U));
	kern_logf("powervr: hwr: half %u: alist %llx last %llx stack %llx; macrolist stack %x flags %x state %x snapshot %x %x\n",
		  half, (unsigned long long)target_word64(target, base + 136U), (unsigned long long)target_word64(target, base + 144U),
		  (unsigned long long)target_word64(target, base + 152U),
		  (unsigned)powervr_object_get32(&target->record, base + 160U), (unsigned)powervr_object_get32(&target->record, base + 168U),
		  (unsigned)powervr_object_get32(&target->record, base + 172U), (unsigned)powervr_object_get32(&target->record, base + 188U),
		  (unsigned)powervr_object_get32(&target->record, base + 192U));

	/* The half's macrolist: the pages the parameter manager took for its tables. */
	if (target->macrolists != NULL) {
		macrolist = (volatile const uint32_t *)(target->macrolists->buffer->view + half * target->layout.macrolist_bytes);
		kern_logf("powervr: hwr: half %u macrolist: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", half,
			  (unsigned)macrolist[0], (unsigned)macrolist[1], (unsigned)macrolist[2], (unsigned)macrolist[3],
			  (unsigned)macrolist[4], (unsigned)macrolist[5], (unsigned)macrolist[6], (unsigned)macrolist[7],
			  (unsigned)macrolist[8], (unsigned)macrolist[9], (unsigned)macrolist[10], (unsigned)macrolist[11]);
	}

	/* Its local list, and the global list its record names. */
	describe_list("local", &target->local);
	global = powervr_object_get32(&target->record, base + (uint32_t)offsetof(struct powervr_fw_target_data, free_lists) + 4U);
	for (list = render->free_lists; list != NULL; list = list->next) {
		if (list->owner == NULL && powervr_object_address(&list->record, 0) == global)
			describe_list("global", list);
	}
}

/* Prints a free list's pages and its firmware record's stack. */
static void
describe_list(
	const char *name,
	const struct powervr_free_list *list)
{
	/* The pages, then the record's words that move. */
	kern_logf("powervr: hwr: %s list %u: pages 0x%llx..0x%llx (%u); current %u, stack top %u, allocated %u, tables %u, ready %u, flags %x\n",
		  name, (unsigned)list->identity,
		  (unsigned long long)(list->pages != NULL ? list->pages->run.paddr : 0U),
		  (unsigned long long)(list->pages != NULL ? list->pages->run.paddr + list->pages->run.size : 0U),
		  (unsigned)list->page_count,
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, current_pages)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, stack_top)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_pages)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, allocated_table_pages)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, ready_pages)),
		  (unsigned)powervr_object_get32(&list->record, (uint32_t)offsetof(struct powervr_fw_free_list, flags)));
}

/* Reads a 64-bit word of a target's record, low half first. */
static uint64_t
target_word64(
	const struct powervr_target *target,
	uint32_t offset)
{
	uint64_t low;
	uint64_t high;

	/* The two halves. */
	low = powervr_object_get32(&target->record, offset);
	high = powervr_object_get32(&target->record, offset + 4U);

	/* Succeeded: the word. */
	return low | (high << 32);
}
