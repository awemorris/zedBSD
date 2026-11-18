/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphics golden context and the TU10x scrubber (the golden-context
 * stage).
 *
 * Before any graphics channel can run, the resource manager builds a golden
 * context: the state every new graphics context starts from.  It does so
 * when a 3D object is made on a privileged graphics channel whose context
 * buffers the driver has made and handed over (promoted).  The order:
 * read the buffers' sizes, make an address space the resource
 * manager shares (it keeps 512 MiB at 4 GiB of it, through the driver's
 * directories), make the channel, promote the buffers, make the 3D object,
 * then free the 3D object, the channel and the space.  The global buffers
 * stay, for every later graphics channel.  On TU102, TU104 and TU106 a
 * second, unprivileged graphics channel (the scrubber) is made the same way
 * and handed to the resource manager, which keeps it while the GPU runs.
 *
 * Everything lives in an arena of VRAM below the first block, written
 * through the register window.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/* The arena: its size (its reserve below the top is NVRTX_GR_ARENA_RESERVE). */
#define GR_ARENA_BYTES			0x8000000ULL
#define GR_ARENA_ALIGN			0x200000ULL

/*
 * The arena's parts.  The golden channel's instance block is 0x12000
 * bytes, and a channel's USERD lies one page after its instance block.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gr.c (r535_gr_oneinit).
 */
#define GR_GOLDEN_TABLES_AT		0x000000ULL
#define GR_SCRUBBER_TABLES_AT		0x100000ULL
#define GR_GOLDEN_INSTANCE_AT		0x200000ULL
#define GR_GOLDEN_INSTANCE_BYTES	0x12000U
#define GR_SCRUBBER_INSTANCE_AT		0x220000ULL
#define GR_SCRUBBER_INSTANCE_BYTES	0x2000U
#define GR_USERD_AFTER_INSTANCE		0x1000ULL
#define GR_BUFFERS_AT			0x400000ULL

/* The tables' directories in each set, as nvrtx_mmu_gr_tables() lays them out. */
#define GR_TABLE_DIRECTORY2_AT		0x1000ULL
#define GR_TABLE_DIRECTORY1_AT		0x2000ULL

/* Where the context buffers are in both spaces. */
#define GR_VA_BASE			0x20000000ULL
#define GR_VA_BYTES			0x8000000ULL

/* The two channels: their numbers and ring length (nothing is submitted on them). */
#define GR_GOLDEN_CHID			1U
#define GR_SCRUBBER_CHID		2U
#define GR_RING_ENTRIES			(0x1000U / 8U)

/*
 * The chips that need the scrubber channel.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/device/base.c (nvkm_device_ctor,
 * the chipset numbers).
 */
#define GR_CHIPSET_TU102		0x162U
#define GR_CHIPSET_TU104		0x164U
#define GR_CHIPSET_TU106		0x166U

/*
 * The promote call's numbers of the two access maps.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gr.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_PRIV_ACCESS_MAP,
 * NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_UNRESTRICTED_PRIV_ACCESS_MAP).
 */
#define GR_BUFFER_PRIV_MAP		10U
#define GR_BUFFER_UNRESTRICTED_MAP	11U

static int gr_buffers_info(struct nvrtx_gpu *gpu);
static int gr_layout(struct nvrtx_gpu *gpu, bool scrubber);
static int gr_tables(struct nvrtx_gpu *gpu, uint64_t tables_at, const uint64_t *physical, bool golden);
static int gr_space(struct nvrtx_gpu *gpu, uint32_t handle, uint64_t tables_at);
static int gr_channel(struct nvrtx_gpu *gpu, uint32_t handle, uint32_t space, uint32_t chid, uint64_t instance_at, const struct nvrtx_buffer *methods, bool privileged);
static int gr_scrubber_control(struct nvrtx_gpu *gpu, bool teardown);
static int gr_golden(struct nvrtx_gpu *gpu);
static int gr_scrubber(struct nvrtx_gpu *gpu);
static bool gr_needs_scrubber(const struct nvrtx_gpu *gpu);
static uint64_t gr_align(uint64_t value, uint32_t shift);

/*
 * Runs the golden-context stage: the golden context, then the scrubber on the
 * chips that need one.
 *
 * The copy channel of copy-channel is already running.  A failure is returned;
 * the finish's stop frees whatever objects were made.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gr.c (r535_gr_oneinit,
 * r535_gr_get_ctxbufs_info, r535_gr_promote_ctx).
 */
int
nvrtx_gr_start(
	struct nvrtx_gpu *gpu)
{
	bool scrubber;
	int error;

	/* Reads the buffers' sizes and lays the arena out. */
	nvrtx_stage_mark("golden-context begin");
	error = gr_buffers_info(gpu);
	if (error != 0)
		return error;

	scrubber = gr_needs_scrubber(gpu);
	error = gr_layout(gpu, scrubber);
	if (error != 0)
		return error;

	/* Builds the golden context. */
	error = gr_golden(gpu);
	if (error != 0)
		return error;

	nvrtx_stage_mark("golden-context ok: golden context");

	/* Only TU102, TU104 and TU106 need the scrubber. */
	if (!scrubber)
		return 0;

	/* Makes the scrubber channel and hands it to the resource manager. */
	error = gr_scrubber(gpu);
	if (error != 0)
		return error;

	/* Succeeded: graphics channels may be made from here. */
	nvrtx_stage_mark("golden-context ok: scrubber");
	return 0;
}

/*
 * Undoes golden-context's objects before the unload.
 *
 * The scrubber is told to tear down first, then its 3D object, channel and
 * space go; whatever the golden context's run left is freed the same way.
 * Failures are counted and shown; the unload removes the rest.
 */
void
nvrtx_gr_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_gr_state *gr;
	int failures;
	int error;

	gr = &gpu->gr;
	failures = 0;

	/* Has the resource manager let go of the scrubber channel. */
	if (gr->scrubber_enabled != 0) {
		error = gr_scrubber_control(gpu, true);
		if (error != 0)
			failures++;
		gr->scrubber_enabled = 0;
	}

	/* Frees the scrubber's 3D object, channel and space. */
	if (gr->scrubber_threed_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_SCRUBBER_THREED);
		if (error != 0)
			failures++;
		gr->scrubber_threed_made = 0;
	}

	if (gr->scrubber_channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_SCRUBBER_CHANNEL);
		if (error != 0)
			failures++;
		gr->scrubber_channel_made = 0;
	}

	if (gr->scrubber_space_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_SCRUBBER_SPACE);
		if (error != 0)
			failures++;
		gr->scrubber_space_made = 0;
	}

	/* Frees what a golden context run that stopped half way left. */
	if (gr->golden_threed_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_THREED);
		if (error != 0)
			failures++;
		gr->golden_threed_made = 0;
	}

	if (gr->golden_channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_CHANNEL);
		if (error != 0)
			failures++;
		gr->golden_channel_made = 0;
	}

	if (gr->golden_space_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_SPACE);
		if (error != 0)
			failures++;
		gr->golden_space_made = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d graphics frees failed; the unload removes the rest", failures);
}

/*
 * Gives back the system-memory method buffers of golden-context.
 *
 * Only once the GPU no longer reads them: after the GSP is stopped.
 */
void
nvrtx_gr_release(
	struct nvrtx_gpu *gpu)
{
	/* Frees each buffer that was allocated. */
	nvrtx_buffer_free(gpu, &gpu->gr.golden_methods);
	nvrtx_buffer_free(gpu, &gpu->gr.scrubber_methods);
}

/* Asks the resource manager for the graphics context buffers' sizes. */
static int
gr_buffers_info(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_gr_state *gr;
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	uint64_t total;
	size_t index;
	int error;

	gr = &gpu->gr;

	/* Sends the zeroed information to the resource manager's own subdevice. */
	params = kern_calloc(1U, NVRTX_GR_BUFFERS_INFO_BYTES);
	if (params == NULL)
		return ENOMEM;

	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    NVRTX_CTL_GR_BUFFERS_INFO,
	    params,
	    NVRTX_GR_BUFFERS_INFO_BYTES,
	    &reply,
	    &reply_length);
	kern_free(params);
	if (error != 0)
		return error;

	/* Reads engine 0's buffers. */
	gr->buffer_count = nvrtx_gr_buffers(reply, reply_length, gr->buffers, NVRTX_GR_BUFFER_MAX);
	if (gr->buffer_count == 0U) {
		nvrtx_stage_mark("golden-context fail: context buffer information missing");
		return EIO;
	}

	/* Shows how much the buffers take. */
	total = 0U;
	for (index = 0U; index < gr->buffer_count; index++)
		total += gr->buffers[index].size;
	nvrtx_stage_mark("golden-context ctxbufs: %u buffers, %u KiB", (unsigned)gr->buffer_count, (unsigned)(total / 1024U));

	/* Succeeded: the buffers are known. */
	return 0;
}

/*
 * Places the arena and every buffer in it and in the address range.
 *
 * Each buffer's VRAM address keeps to its page and its address in the range
 * to its alignment.  The scrubber gets its own copies of the buffers that are
 * not global.  Returns ENOSPC when the arena or the range is too small.
 */
static int
gr_layout(
	struct nvrtx_gpu *gpu,
	bool scrubber)
{
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t candidate;
	uint64_t physical;
	uint64_t virtual_offset;
	uint32_t region;
	size_t index;
	int error;

	gr = &gpu->gr;

	/* Takes the arena below the first block, in the last usable region. */
	error = nvrtx_mmu_vram_block(gpu->static_info, NVRTX_GR_ARENA_RESERVE, &candidate, &region);
	if (error != 0) {
		nvrtx_stage_mark("golden-context fail: no VRAM for the arena (%d)", error);
		return error;
	}

	gr->arena = gr_align(candidate, 21U);
	if (gr->arena + GR_ARENA_BYTES > gpu->engine.vram_block) {
		nvrtx_stage_mark("golden-context fail: the arena %llx meets the first block", (unsigned long long)gr->arena);
		return ENOSPC;
	}

	/* Places the golden context's buffers, then the scrubber's own copies. */
	physical = GR_BUFFERS_AT;
	virtual_offset = 0U;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		physical = gr_align(physical, buffer->page_shift);
		virtual_offset = gr_align(virtual_offset, buffer->align_shift);
		gr->golden_physical[index] = gr->arena + physical;
		gr->virtual_address[index] = GR_VA_BASE + virtual_offset;
		physical += gr_align(buffer->size, buffer->page_shift);
		virtual_offset += gr_align(buffer->size, 12U);
	}

	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];

		/* A global buffer is shared with the golden context. */
		if (buffer->global || !scrubber) {
			gr->scrubber_physical[index] = gr->golden_physical[index];
			continue;
		}

		physical = gr_align(physical, buffer->page_shift);
		gr->scrubber_physical[index] = gr->arena + physical;
		physical += gr_align(buffer->size, buffer->page_shift);
	}

	/* Everything must fit the arena and the range. */
	if (physical > GR_ARENA_BYTES || virtual_offset > GR_VA_BYTES) {
		nvrtx_stage_mark("golden-context fail: buffers need %llx of VRAM, %llx of range", (unsigned long long)physical, (unsigned long long)virtual_offset);
		return ENOSPC;
	}

	/* Succeeded: the arena is laid out. */
	nvrtx_stage_mark("golden-context arena at %llx, buffers %llx bytes", (unsigned long long)gr->arena, (unsigned long long)(physical - GR_BUFFERS_AT));
	return 0;
}

/*
 * Builds one set of the context's page tables and writes it into the arena.
 *
 * Every buffer the promote maps gets its pages: privileged, the access maps
 * read-only.  The golden context does not map the access map itself (the
 * resource manager reaches it by its physical address); the scrubber, which
 * shares it, maps it like the others but leaves out the unrestricted one.
 */
static int
gr_tables(
	struct nvrtx_gpu *gpu,
	uint64_t tables_at,
	const uint64_t *physical,
	bool golden)
{
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint8_t *image;
	size_t length;
	size_t index;
	int error;

	gr = &gpu->gr;

	/* Builds the directories in memory. */
	length = nvrtx_mmu_gr_tables_bytes(GR_VA_BYTES);
	image = kern_malloc(length);
	if (image == NULL)
		return ENOMEM;

	error = nvrtx_mmu_gr_tables(image, gr->arena + tables_at, GR_VA_BASE, GR_VA_BYTES);

	/* Maps each buffer the promote names by address. */
	for (index = 0U; error == 0 && index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (golden && buffer->buffer_id == GR_BUFFER_PRIV_MAP)
			continue;
		if (!golden && buffer->buffer_id == GR_BUFFER_UNRESTRICTED_MAP)
			continue;
		error = nvrtx_mmu_gr_map(image,
		    GR_VA_BASE,
		    GR_VA_BYTES,
		    gr->virtual_address[index],
		    physical[index],
		    gr_align(buffer->size, 12U),
		    buffer->read_only);
	}

	/* Copies the tables into the arena. */
	if (error == 0)
		error = nvrtx_vram_write(gpu, gr->arena + tables_at, image, length);
	kern_free(image);
	if (error != 0)
		return error;

	/* Succeeded: the tables are in the VRAM. */
	return 0;
}

/*
 * Makes an address space the resource manager shares, over a set of tables.
 *
 * The resource manager then writes its own range's entries into the tables'
 * first-level directory, so the tables must be in the VRAM before.
 */
static int
gr_space(
	struct nvrtx_gpu *gpu,
	uint32_t handle,
	uint64_t tables_at)
{
	uint8_t params[NVRTX_COPY_PDES_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint64_t root;
	int error;

	root = gpu->gr.arena + tables_at;

	/* Makes the space, not owned by the driver. */
	nvrtx_space_params_shared(params);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, handle, NVRTX_CLASS_SPACE, params, NVRTX_SPACE_PARAMS_BYTES);
	if (error != 0)
		return error;

	/* Records it for the stop. */
	if (handle == NVRTX_HANDLE_GOLDEN_SPACE)
		gpu->gr.golden_space_made = 1;
	else
		gpu->gr.scrubber_space_made = 1;

	/* Hands the resource manager the driver's top three directories for its range. */
	nvrtx_copy_pdes_params(params, root, root + GR_TABLE_DIRECTORY2_AT, root + GR_TABLE_DIRECTORY1_AT);
	error = nvrtx_rm_control(gpu,
	    NVRTX_HANDLE_CLIENT,
	    handle,
	    NVRTX_CTL_COPY_SERVER_PDES,
	    params,
	    NVRTX_COPY_PDES_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	/* Drops any stale translation under the root. */
	error = nvrtx_flush_tlb(gpu, root);
	if (error != 0)
		return error;

	/* Succeeded: the space uses the driver's tables. */
	return 0;
}

/*
 * Makes a graphics channel in a shared space; nothing is ever submitted on it.
 *
 * Its instance block and USERD are cleared VRAM of the arena, which the
 * resource manager fills; the method buffer is system memory.
 */
static int
gr_channel(
	struct nvrtx_gpu *gpu,
	uint32_t handle,
	uint32_t space,
	uint32_t chid,
	uint64_t instance_at,
	const struct nvrtx_buffer *methods,
	bool privileged)
{
	struct nvrtx_channel_setup setup;
	uint8_t *params;
	int error;

	/* Describes the channel. */
	setup.ring_va = 0U;
	setup.ring_entries = GR_RING_ENTRIES;
	setup.chid = chid;
	setup.space = space;
	setup.engine = NVRTX_ENGINE_GRAPHICS;
	setup.privileged = 0U;
	if (privileged)
		setup.privileged = 1U;
	setup.instance = gpu->gr.arena + instance_at;
	setup.userd = gpu->gr.arena + instance_at + GR_USERD_AFTER_INSTANCE;
	setup.methods = methods->dma.device_address;
	setup.methods_size = gpu->engine.methods_size;

	/* Makes it under the device. */
	params = kern_malloc(NVRTX_CHANNEL_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, handle, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	kern_free(params);
	if (error != 0)
		return error;

	/* Records it for the stop. */
	if (handle == NVRTX_HANDLE_GOLDEN_CHANNEL)
		gpu->gr.golden_channel_made = 1;
	else
		gpu->gr.scrubber_channel_made = 1;

	/* Succeeded: the channel exists. */
	return 0;
}

/*
 * Hands a graphics channel its context buffers (the promote).
 *
 * Also for the compute stage's channel (compute.c), which takes the
 * scrubber's form: the global buffers shared, its own made.
 *
 * For the golden context every buffer is new; the resource manager fills
 * the ones it initializes, and the access map is not mapped.  For the
 * scrubber the global buffers are the golden context's and only its own
 * buffers are filled; the unrestricted access map is left out.
 *
 * Each buffer is named at its golden context address, or at the address
 * virtual_address gives for it when that is not NULL (a channel sharing
 * the work space has its own buffers elsewhere in it).
 */
int
nvrtx_gr_promote(
	struct nvrtx_gpu *gpu,
	uint32_t channel,
	const uint64_t *physical,
	const uint64_t *virtual_address,
	bool golden)
{
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	struct nvrtx_promote_entry entries[NVRTX_GR_BUFFER_MAX];
	struct nvrtx_promote_entry *entry;
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	size_t count;
	size_t index;
	bool made_here;
	int error;

	gr = &gpu->gr;

	/* Describes each buffer. */
	count = 0U;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		made_here = golden || !buffer->global;

		/* A shared channel does not take the unrestricted access map. */
		if (!made_here && buffer->buffer_id == GR_BUFFER_UNRESTRICTED_MAP)
			continue;

		entry = &entries[count];
		entry->buffer_id = buffer->buffer_id;
		entry->initialize = 0;
		if (buffer->initialize && made_here)
			entry->initialize = 1;
		entry->nonmapped = 0;
		if (made_here && buffer->buffer_id == GR_BUFFER_PRIV_MAP)
			entry->nonmapped = 1;
		entry->virtual_address = gr->virtual_address[index];
		if (virtual_address != NULL) {
			/* The caller's address for it. */
			entry->virtual_address = virtual_address[index];
		}
		entry->physical = physical[index];
		entry->size = buffer->size;
		count++;
	}

	/* Sends the promote to the subdevice. */
	params = kern_malloc(NVRTX_PROMOTE_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	nvrtx_promote_params(params, NVRTX_HANDLE_CLIENT, channel, entries, count);
	error = nvrtx_rm_control(gpu,
	    NVRTX_HANDLE_CLIENT,
	    NVRTX_HANDLE_SUBDEVICE,
	    NVRTX_CTL_PROMOTE_CONTEXT,
	    params,
	    NVRTX_PROMOTE_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	kern_free(params);
	if (error != 0)
		return error;

	/* Succeeded: the channel has its context buffers. */
	return 0;
}

/* Tells the resource manager to take (or to let go of) the scrubber channel. */
static int
gr_scrubber_control(
	struct nvrtx_gpu *gpu,
	bool teardown)
{
	uint8_t params[NVRTX_SCRUBBER_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* One byte: whether this is the teardown. */
	params[0] = 0U;
	if (teardown)
		params[0] = 1U;

	/* Sends it to the subdevice of the client that holds the scrubber. */
	error = nvrtx_rm_control(gpu,
	    NVRTX_HANDLE_CLIENT,
	    NVRTX_HANDLE_SUBDEVICE,
	    NVRTX_CTL_SCRUBBER,
	    params,
	    NVRTX_SCRUBBER_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the resource manager answered. */
	return 0;
}

/* Builds the golden context, then frees its 3D object, channel and space. */
static int
gr_golden(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_gr_state *gr;
	int error;

	gr = &gpu->gr;

	/* Writes the tables and clears the channel's instance block and USERD. */
	error = gr_tables(gpu, GR_GOLDEN_TABLES_AT, gr->golden_physical, true);
	if (error == 0)
		error = nvrtx_vram_zero(gpu, gr->arena + GR_GOLDEN_INSTANCE_AT, GR_GOLDEN_INSTANCE_BYTES);
	if (error != 0)
		return error;

	/* Makes the shared space over the tables. */
	error = gr_space(gpu, NVRTX_HANDLE_GOLDEN_SPACE, GR_GOLDEN_TABLES_AT);
	if (error != 0)
		return error;

	/* Makes the privileged channel with its own method buffer. */
	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &gr->golden_methods);
	if (error != 0)
		return error;

	error = gr_channel(gpu, NVRTX_HANDLE_GOLDEN_CHANNEL, NVRTX_HANDLE_GOLDEN_SPACE, GR_GOLDEN_CHID, GR_GOLDEN_INSTANCE_AT, &gr->golden_methods, true);
	if (error != 0)
		return error;

	/* Hands it every buffer, all new. */
	error = nvrtx_gr_promote(gpu, NVRTX_HANDLE_GOLDEN_CHANNEL, gr->golden_physical, NULL, true);
	if (error != 0)
		return error;

	/* The 3D object makes the resource manager build the golden context. */
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_GOLDEN_CHANNEL, NVRTX_HANDLE_GOLDEN_THREED, gpu->identity.chip.profile->class_threed, NULL, 0U);
	if (error != 0)
		return error;

	gr->golden_threed_made = 1;

	/* The resource manager keeps the context; the channel and its space are not needed again. */
	error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_THREED);
	if (error != 0)
		return error;

	gr->golden_threed_made = 0;
	error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_CHANNEL);
	if (error != 0)
		return error;

	gr->golden_channel_made = 0;
	error = nvrtx_rm_free(gpu, NVRTX_HANDLE_GOLDEN_SPACE);
	if (error != 0)
		return error;

	/* Succeeded: the golden context exists and the global buffers stay. */
	gr->golden_space_made = 0;
	return 0;
}

/* Makes the scrubber channel and hands it to the resource manager, which keeps it. */
static int
gr_scrubber(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_gr_state *gr;
	int error;

	gr = &gpu->gr;

	/* Writes the tables and clears the channel's instance block and USERD. */
	error = gr_tables(gpu, GR_SCRUBBER_TABLES_AT, gr->scrubber_physical, false);
	if (error == 0)
		error = nvrtx_vram_zero(gpu, gr->arena + GR_SCRUBBER_INSTANCE_AT, GR_SCRUBBER_INSTANCE_BYTES);
	if (error != 0)
		return error;

	/* Makes the shared space, the unprivileged channel and its method buffer. */
	error = gr_space(gpu, NVRTX_HANDLE_SCRUBBER_SPACE, GR_SCRUBBER_TABLES_AT);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &gr->scrubber_methods);
	if (error != 0)
		return error;

	error = gr_channel(gpu, NVRTX_HANDLE_SCRUBBER_CHANNEL, NVRTX_HANDLE_SCRUBBER_SPACE, GR_SCRUBBER_CHID, GR_SCRUBBER_INSTANCE_AT, &gr->scrubber_methods, false);
	if (error != 0)
		return error;

	/* Hands it its own buffers and the shared global ones. */
	error = nvrtx_gr_promote(gpu, NVRTX_HANDLE_SCRUBBER_CHANNEL, gr->scrubber_physical, NULL, false);
	if (error != 0)
		return error;

	/* Gives it a 3D object. */
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_SCRUBBER_CHANNEL, NVRTX_HANDLE_SCRUBBER_THREED, gpu->identity.chip.profile->class_threed, NULL, 0U);
	if (error != 0)
		return error;

	gr->scrubber_threed_made = 1;

	/* Hands the channel to the resource manager. */
	error = gr_scrubber_control(gpu, false);
	if (error != 0)
		return error;

	/* Succeeded: the stop must tell the resource manager to let it go. */
	gr->scrubber_enabled = 1;
	return 0;
}

/* Reports whether the chip needs the scrubber channel (TU102, TU104, TU106). */
static bool
gr_needs_scrubber(
	const struct nvrtx_gpu *gpu)
{
	uint32_t chipset;

	chipset = gpu->identity.chip.chipset;

	/* The three Turing chips with the context-switch erratum. */
	if (chipset == GR_CHIPSET_TU102)
		return true;
	if (chipset == GR_CHIPSET_TU104)
		return true;
	if (chipset == GR_CHIPSET_TU106)
		return true;

	/* Every other chip does without. */
	return false;
}

/* Rounds a value up to a multiple of 2^shift. */
static uint64_t
gr_align(
	uint64_t value,
	uint32_t shift)
{
	uint64_t unit;

	unit = 1ULL << shift;

	/* Reports the rounded value. */
	return (value + unit - 1U) & ~(unit - 1U);
}
