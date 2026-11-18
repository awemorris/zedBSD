/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first address space and channel of the nvrtx driver (stages address-space
 * to channel-stop).
 *
 * address-space takes a block at the top of the usable VRAM, writes the page
 * tables of one 2 MiB range into it through the register window onto the VRAM
 * (PRAMIN), has the resource manager make an address space the driver owns and
 * gives it the root, and invalidates the TLBs.  The range maps three
 * system-memory buffers: the ring of push-buffer entries, the push buffer and a
 * semaphore page.
 *
 * copy-channel picks a copy engine on a runlist of its own, makes the channel
 * (its instance block and USERD in the same VRAM block, its method buffer in
 * system memory), binds and schedules it, and runs one fence: the push buffer
 * releases a semaphore value that the CPU polls.
 *
 * channel-stop reports a channel error the firmware sent and frees the channel
 * and the address space; the GSP's unload in unload removes whatever is left.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The register window onto the VRAM: its 64 KiB-unit base register, where
 * it shows, and its size.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/maxwell/gm107/dev_bus.h
 * (NV_PBUS_BAR0_WINDOW) and turing/tu102/dev_ram.h (NV_PRAMIN), and Linux
 * v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/instmem/nv50.c
 * (nv50_instmem_set_bar0_window_addr).
 */
#define CHANNEL_REG_WINDOW_BASE		0x001700U
#define CHANNEL_WINDOW_AT		0x700000U
#define CHANNEL_WINDOW_BYTES		0x100000U

/*
 * A fill holds the register window for one piece of at most 16 KiB (4096
 * register writes), then steps aside while another user waits for the
 * window: the mutex's release does not hand it over, so a fill that took
 * it again at once kept every other user (the display's notifiers, the work
 * channel's submissions) waiting until a fill of megabytes (a new blob)
 * ended.  The step aside ends when no user waits, or after 2 ms, so a
 * stream of users cannot hold a fill back for good.
 */
#define CHANNEL_FILL_PIECE_BYTES	0x4000U
#define CHANNEL_FILL_ASIDE_US		2000U

/* The VRAM block: the page tables, then the instance block, then the USERD page. */
#define CHANNEL_BLOCK_BYTES		0x10000U
#define CHANNEL_INSTANCE_AT		0x5000U
#define CHANNEL_USERD_AT		0x6000U

/* The mapped range: the ring page, four push-buffer pages and the semaphore page. */
#define CHANNEL_VA			0x20000000ULL
#define CHANNEL_RING_PAGE		0U
#define CHANNEL_PUSH_PAGE		1U
#define CHANNEL_PUSH_PAGES		4U
#define CHANNEL_SEMAPHORE_PAGE		5U
#define CHANNEL_PAGES			6U
#define CHANNEL_RING_ENTRIES		512U

/* The address-space limit written into the instance block (47-bit virtual addresses). */
#define CHANNEL_VA_LIMIT		(1ULL << 47)

/* The channel number (the resource manager keeps channel 0). */
#define CHANNEL_ID			8U

/*
 * The TLB invalidate registers: the root, its upper half, and the trigger
 * (bit 31) with "all addresses" (bit 0).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmtu102.c (tu102_vmm_flush).
 */
#define CHANNEL_REG_FLUSH_ROOT		0xb830a0U
#define CHANNEL_REG_FLUSH_ROOT_UPPER	0xb830a4U
#define CHANNEL_REG_FLUSH		0xb830b0U
#define CHANNEL_FLUSH_TRIGGER		0x80000000U
#define CHANNEL_FLUSH_ALL_ADDRESSES	0x1U
#define CHANNEL_FLUSH_BAR		0x6U
#define CHANNEL_FLUSH_TIMEOUT_US	2000000U

/*
 * USERD's put and get of the ring.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (Nvc46fControl's GPGet and GPPut).
 */
#define CHANNEL_USERD_RING_GET		0x88U
#define CHANNEL_USERD_RING_PUT		0x8cU

/*
 * The doorbell: runlist in bits 22:16, channel in bits 11:0.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET, NV_VIRTUAL_FUNCTION_DOORBELL) and
 * Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/fifo/tu102.c
 * (tu102_chan_doorbell_handle, tu102_chan_start).
 */
#define CHANNEL_REG_DOORBELL		0xbb0090U

/* The fence value and how long the CPU waits for it. */
#define CHANNEL_FENCE_VALUE		0x1234U
#define CHANNEL_FENCE_TIMEOUT_US	2000000U
#define CHANNEL_FENCE_STEP_US		100U

/*
 * The copy fill: its subchannel, where its push buffer and its words go, how
 * many, and the values.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h (SUBC_NV90B5,
 * the copy engine's subchannel).
 */
#define CHANNEL_COPY_SUBCHANNEL		4U
#define CHANNEL_FILL_PUSH_AT		0x2000U
#define CHANNEL_FILL_AT			0x100U
#define CHANNEL_FILL_WORDS		64U
#define CHANNEL_FILL_VALUE		0xc0ffee01U
#define CHANNEL_FILL_FENCE_VALUE	0x1235U

/*
 * How long the end of channel-stop takes messages the firmware sent meanwhile.
 */
#define CHANNEL_DRAIN_TIMEOUT_US	100000U

static int channel_vram_write(struct nvrtx_gpu *gpu, uint64_t address, const uint8_t *data, size_t length);
static int channel_vram_read32(struct nvrtx_gpu *gpu, uint64_t address, uint32_t *value);
static int channel_vram_fill(struct nvrtx_gpu *gpu, uint64_t address, uint32_t value, size_t length);
static void channel_window_take(struct nvrtx_gpu *gpu);
static void channel_fill_aside(struct nvrtx_gpu *gpu);
static int channel_buffers(struct nvrtx_gpu *gpu);
static int channel_flush_tlb(struct nvrtx_gpu *gpu);
static int channel_flush(struct nvrtx_gpu *gpu, uint64_t root, uint32_t type);
static int channel_fence(struct nvrtx_gpu *gpu);
static int channel_submit(struct nvrtx_gpu *gpu, size_t push_at, size_t length, uint32_t index);
static int channel_wait(struct nvrtx_gpu *gpu, uint32_t expected, uint32_t put);
static int channel_copy_fill(struct nvrtx_gpu *gpu);

/*
 * Runs the address-space stage: the VRAM block, the page tables, the address
 * space and the TLB invalidate.
 */
int
nvrtx_space_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	uint64_t pages[CHANNEL_PAGES];
	uint8_t params[NVRTX_SPACE_PARAMS_BYTES];
	uint8_t *image;
	const uint8_t *reply;
	size_t reply_length;
	uint32_t region;
	uint32_t check;
	unsigned index;
	int error;

	engine = &gpu->engine;

	/* Takes the VRAM block at the top of the last usable region. */
	nvrtx_stage_mark("address-space begin");
	error = nvrtx_mmu_vram_block(gpu->static_info, CHANNEL_BLOCK_BYTES, &engine->vram_block, &region);
	if (error != 0) {
		nvrtx_stage_mark("address-space fail: no usable VRAM region (%d)", error);
		return error;
	}

	nvrtx_stage_mark("address-space vram: region %u, block at %llx", (unsigned)region, (unsigned long long)engine->vram_block);

	/* Allocates the system-memory buffers the range maps. */
	error = channel_buffers(gpu);
	if (error != 0)
		return error;

	/* Lists the range's pages in order. */
	pages[CHANNEL_RING_PAGE] = engine->ring.dma.device_address;
	for (index = 0U; index < CHANNEL_PUSH_PAGES; index++)
		pages[CHANNEL_PUSH_PAGE + index] = engine->push.dma.device_address + (uint64_t)index * NVRTX_GSP_PAGE_SIZE;
	pages[CHANNEL_SEMAPHORE_PAGE] = engine->semaphore.dma.device_address;

	/* Builds the tables in memory and copies them into the VRAM block. */
	image = kern_malloc(NVRTX_MMU_TABLES_BYTES);
	if (image == NULL)
		return ENOMEM;

	error = nvrtx_mmu_tables(image, engine->vram_block, CHANNEL_VA, pages, CHANNEL_PAGES);
	if (error == 0)
		error = channel_vram_write(gpu, engine->vram_block, image, NVRTX_MMU_TABLES_BYTES);
	if (error == 0)
		error = channel_vram_read32(gpu, engine->vram_block, &check);
	if (error == 0 && check != nvrtx_get32(image, 0U)) {
		nvrtx_stage_mark("address-space fail: root reads back %08x, wrote %08x", (unsigned)check, (unsigned)nvrtx_get32(image, 0U));
		error = EIO;
	}
	kern_free(image);
	if (error != 0)
		return error;

	nvrtx_stage_mark("address-space page tables at %llx", (unsigned long long)engine->vram_block);

	/* Makes the address space the driver owns. */
	nvrtx_space_params(params);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_SPACE, NVRTX_CLASS_SPACE, params, sizeof(params));
	if (error != 0)
		return error;

	engine->space_made = 1;

	/* Gives it the root. */
	nvrtx_pdir_params(params, engine->vram_block, NVRTX_HANDLE_SPACE);
	error = nvrtx_rm_control(gpu,
	    NVRTX_HANDLE_CLIENT,
	    NVRTX_HANDLE_DEVICE,
	    NVRTX_CTL_SET_PAGE_DIRECTORY,
	    params,
	    NVRTX_PDIR_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	engine->root_set = 1;

	/* Drops any stale translation of the new tables. */
	error = channel_flush_tlb(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the address space maps the range. */
	nvrtx_stage_mark("address-space ok: vaspace");
	return 0;
}

/*
 * Runs the copy-channel stage: the copy engine, the channel and one fence.
 */
int
nvrtx_channel_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	struct nvrtx_channel_setup setup;
	uint8_t params[NVRTX_CHANNEL_PARAMS_BYTES];
	uint8_t *instance;
	const uint8_t *reply;
	size_t reply_length;
	bool allowed;
	int error;

	engine = &gpu->engine;

	/* Reads the engines and their runlists, and picks a copy engine. */
	nvrtx_stage_mark("copy-channel begin");
	kern_memset(gpu->reply, 0, NVRTX_DEVINFO_PARAMS_BYTES);
	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    NVRTX_CTL_DEVICE_INFO,
	    gpu->reply,
	    NVRTX_DEVINFO_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;

	error = nvrtx_devinfo_pick(reply, reply_length, &engine->pick);
	if (error != 0) {
		nvrtx_stage_mark("copy-channel fail: no copy engine of its own runlist (%d)", error);
		return error;
	}

	/* Asks for the method buffer's size and allocates it. */
	kern_memset(params, 0, NVRTX_METHOD_BUFFER_PARAMS_BYTES);
	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    NVRTX_CTL_METHOD_BUFFER_SIZE,
	    params,
	    NVRTX_METHOD_BUFFER_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	if (error != 0)
		return error;
	if (reply_length < NVRTX_METHOD_BUFFER_PARAMS_BYTES)
		return EIO;

	engine->methods_size = nvrtx_get32(reply, 0U);
	if (engine->methods_size == 0U || engine->methods_size > 0x100000U)
		return EIO;

	error = nvrtx_buffer_alloc(gpu, engine->methods_size, &engine->methods);
	if (error != 0)
		return error;

	nvrtx_stage_mark("copy-channel copy engine %x runlist %u (graphics %u), method buffer %u",
	    (unsigned)engine->pick.engine,
	    (unsigned)engine->pick.runlist,
	    (unsigned)engine->pick.graphics_runlist,
	    (unsigned)engine->methods_size);

	/* Writes the instance block (naming the page tables) and a clear USERD into the VRAM block. */
	instance = kern_calloc(1U, NVRTX_INSTANCE_BYTES);
	if (instance == NULL)
		return ENOMEM;

	nvrtx_mmu_instance(instance, engine->vram_block, CHANNEL_VA_LIMIT);
	error = channel_vram_write(gpu, engine->vram_block + CHANNEL_INSTANCE_AT, instance, NVRTX_INSTANCE_BYTES);
	kern_memset(instance, 0, NVRTX_INSTANCE_BYTES);
	if (error == 0)
		error = channel_vram_write(gpu, engine->vram_block + CHANNEL_USERD_AT, instance, NVRTX_INSTANCE_BYTES);
	kern_free(instance);
	if (error != 0)
		return error;

	/* Makes the channel. */
	setup.ring_va = CHANNEL_VA + (uint64_t)CHANNEL_RING_PAGE * NVRTX_GSP_PAGE_SIZE;
	setup.ring_entries = CHANNEL_RING_ENTRIES;
	setup.chid = CHANNEL_ID;
	setup.space = NVRTX_HANDLE_SPACE;
	setup.engine = engine->pick.engine;
	setup.privileged = 0U;
	setup.instance = engine->vram_block + CHANNEL_INSTANCE_AT;
	setup.userd = engine->vram_block + CHANNEL_USERD_AT;
	setup.methods = engine->methods.dma.device_address;
	setup.methods_size = engine->methods_size;
	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_CHANNEL, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	if (error != 0)
		return error;

	engine->channel_made = 1;

	/* Binds it to the copy engine and lets the scheduler run it. */
	kern_memset(params, 0, NVRTX_BIND_PARAMS_BYTES);
	nvrtx_put32(params, 0U, engine->pick.engine);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_CHANNEL, NVRTX_CTL_BIND, params, NVRTX_BIND_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	kern_memset(params, 0, NVRTX_SCHEDULE_PARAMS_BYTES);
	params[0] = 1U;
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_CHANNEL, NVRTX_CTL_SCHEDULE, params, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	/* The stop takes the channel off the scheduler before it frees it. */
	engine->scheduled = 1;

	nvrtx_stage_mark("copy-channel ok: channel %u on runlist %u", (unsigned)CHANNEL_ID, (unsigned)engine->pick.runlist);

	/* Runs one fence through the channel. */
	error = channel_fence(gpu);
	if (error != 0)
		return error;

	/* Has the copy engine fill words, unless the boot stops before the copy. */
	allowed = nvrtx_stage_allowed("copy-fill");
	if (!allowed) {
		nvrtx_stage_mark("copy-fill skipped (nvrtx.stop=copy-fill)");
		return 0;
	}

	error = channel_copy_fill(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the channel runs work. */
	return 0;
}

/*
 * Runs stage channel-stop's stop: takes the messages the firmware sent, then
 * undoes address-space and copy-channel: the channel off the scheduler, the
 * copy object, the channel, the root, the address space.
 *
 * The finish runs it before every unload, whatever nvrtx.stop says: when the
 * channel was left scheduled for the unload to remove, the firmware did
 * not answer the unload call (ETIMEDOUT) and did not stop.  Failures are only
 * reported; the unload removes whatever stays.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_stop)
 * and Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/fifo/chan.c
 * (nvkm_chan_del).
 */
void
nvrtx_channel_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	uint8_t params[NVRTX_UNPDIR_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	size_t length;
	int failures;
	int error;

	engine = &gpu->engine;
	failures = 0;

	/* Takes what the firmware sent meanwhile (a channel error is reported by the wait). */
	if (gpu->reply != NULL)
		(void)nvrtx_rpc_wait(gpu, 0xffffffffU, 0U, CHANNEL_DRAIN_TIMEOUT_US, &length);

	/* Takes the channel off the scheduler, so no work of it runs while it goes. */
	if (engine->scheduled != 0) {
		kern_memset(params, 0, sizeof(params));
		error = nvrtx_rm_control(gpu,
		    NVRTX_HANDLE_CLIENT,
		    NVRTX_HANDLE_CHANNEL,
		    NVRTX_CTL_SCHEDULE,
		    params,
		    NVRTX_SCHEDULE_PARAMS_BYTES,
		    &reply,
		    &reply_length);
		if (error != 0)
			failures++;
		engine->scheduled = 0;
	}

	/* Frees the copy object, a child of the channel. */
	if (engine->copy_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_COPY);
		if (error != 0)
			failures++;
		engine->copy_made = 0;
	}

	/* Frees the channel. */
	if (engine->channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_CHANNEL);
		if (error != 0)
			failures++;
		engine->channel_made = 0;
	}

	/* Takes the root back from the address space, then frees it. */
	if (engine->root_set != 0) {
		kern_memset(params, 0, sizeof(params));
		nvrtx_put32(params, 0U, NVRTX_HANDLE_SPACE);
		error = nvrtx_rm_control(gpu,
		    NVRTX_HANDLE_CLIENT,
		    NVRTX_HANDLE_DEVICE,
		    NVRTX_CTL_UNSET_PAGE_DIRECTORY,
		    params,
		    sizeof(params),
		    &reply,
		    &reply_length);
		if (error != 0)
			failures++;
		engine->root_set = 0;
	}

	if (engine->space_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_SPACE);
		if (error != 0)
			failures++;
		engine->space_made = 0;
	}

	/* Says how the stop went. */
	if (failures == 0) {
		nvrtx_stage_mark("channel-stop ok: channel and vaspace freed");
	} else {
		nvrtx_stage_mark("channel-stop: %d frees failed; the unload removes the rest", failures);
	}
}

/*
 * Gives back the system-memory buffers of address-space and copy-channel.
 *
 * Only once the GPU no longer reads them: after the GSP is stopped.
 */
void
nvrtx_channel_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;

	engine = &gpu->engine;

	/* Frees each buffer that was allocated. */
	nvrtx_buffer_free(gpu, &engine->ring);
	nvrtx_buffer_free(gpu, &engine->push);
	nvrtx_buffer_free(gpu, &engine->semaphore);
	nvrtx_buffer_free(gpu, &engine->methods);
}

/*
 * Invalidates every TLB entry of the page tables under one root in the VRAM.
 *
 * Returns ETIMEDOUT when the invalidate does not finish in 2 s.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmtu102.c (tu102_vmm_flush).
 */
int
nvrtx_flush_tlb(
	struct nvrtx_gpu *gpu,
	uint64_t root)
{
	int error;

	/* Invalidates all the addresses of the root. */
	error = channel_flush(gpu, root, CHANNEL_FLUSH_ALL_ADDRESSES);
	if (error != 0)
		return error;

	/* Succeeded: no stale translation is left. */
	return 0;
}

/*
 * Invalidates the hub's TLB entries of a BAR's page tables (the vram-window
 * stage).
 *
 * The flush of a BAR's address space: all addresses, every root, the hub's
 * TLB only.  Returns ETIMEDOUT when the invalidate does not finish in 2 s.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmtu102.c (tu102_vmm_flush).
 */
int
nvrtx_flush_tlb_bar(
	struct nvrtx_gpu *gpu,
	uint64_t root)
{
	int error;

	/* Invalidates the hub's translations under the root. */
	error = channel_flush(gpu, root, CHANNEL_FLUSH_ALL_ADDRESSES | CHANNEL_FLUSH_BAR);
	if (error != 0)
		return error;

	/* Succeeded: the CPU's window sees the new entries. */
	return 0;
}

/*
 * Writes bytes of any length into the VRAM through the register window.
 *
 * The bytes go in pieces that each lie inside one 64 KiB step of the
 * window.  The address and the length are whole words.
 */
int
nvrtx_vram_write(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	const uint8_t *data,
	size_t length)
{
	size_t piece;
	size_t done;
	int error;

	/* Whole words only. */
	if ((address % 4U) != 0U || (length % 4U) != 0U)
		return EINVAL;

	/* Writes up to the next 64 KiB boundary each time. */
	done = 0U;
	while (done < length) {
		piece = 0x10000U - (size_t)((address + done) & 0xffffU);
		if (piece > length - done)
			piece = length - done;
		error = channel_vram_write(gpu, address + done, data + done, piece);
		if (error != 0)
			return error;
		done += piece;
	}

	/* Succeeded: the bytes are in the VRAM. */
	return 0;
}

/*
 * Clears a run of the VRAM through the register window.
 */
int
nvrtx_vram_zero(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	size_t length)
{
	uint8_t *zeros;
	size_t piece;
	size_t done;
	int error;

	/* A page of zeros is written over and over. */
	zeros = kern_calloc(1U, NVRTX_GSP_PAGE_SIZE);
	if (zeros == NULL)
		return ENOMEM;

	/* Clears the run a page at a time. */
	done = 0U;
	error = 0;
	while (done < length) {
		piece = NVRTX_GSP_PAGE_SIZE;
		if (piece > length - done)
			piece = length - done;
		error = nvrtx_vram_write(gpu, address + done, zeros, piece);
		if (error != 0)
			break;
		done += piece;
	}

	kern_free(zeros);

	/* Reports a write the window refused. */
	if (error != 0)
		return error;

	/* Succeeded: the run reads as zero. */
	return 0;
}

/*
 * Fills a run of the VRAM with one 32-bit value through the register window.
 *
 * The run goes in pieces of at most CHANNEL_FILL_PIECE_BYTES that each lie
 * inside one 64 KiB step of the window; between them the fill steps aside
 * for the window's other users.  The address and the length are whole
 * words.
 */
int
nvrtx_vram_fill(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	uint32_t value,
	size_t length)
{
	size_t piece;
	size_t done;
	int error;

	/* Whole words only. */
	if ((address % 4U) != 0U || (length % 4U) != 0U)
		return EINVAL;

	/* Fills up to the next 64 KiB boundary or one piece each time, stepping aside before the next. */
	done = 0U;
	while (done < length) {
		piece = 0x10000U - (size_t)((address + done) & 0xffffU);
		if (piece > CHANNEL_FILL_PIECE_BYTES)
			piece = CHANNEL_FILL_PIECE_BYTES;
		if (piece > length - done)
			piece = length - done;
		error = channel_vram_fill(gpu, address + done, value, piece);
		if (error != 0)
			return error;
		done += piece;
		if (done < length)
			channel_fill_aside(gpu);
	}

	/* Succeeded: every word of the run holds the value. */
	return 0;
}

/*
 * Reads one word of the VRAM through the register window.
 */
int
nvrtx_vram_read32(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	uint32_t *value)
{
	int error;

	/* Reads it through the window, which is put back afterwards. */
	error = channel_vram_read32(gpu, address, value);
	if (error != 0)
		return error;

	/* Succeeded: the word is in *value. */
	return 0;
}

/* Writes bytes into the VRAM through the register window, then puts the window back. */
static int
channel_vram_write(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	const uint8_t *data,
	size_t length)
{
	uint64_t base;
	uint32_t offset;
	uint32_t saved;
	size_t at;

	/* The range must lie inside one window position, in whole words. */
	base = address & ~0xffffULL;
	offset = (uint32_t)(address & 0xffffU);
	if ((length % 4U) != 0U || offset + length > CHANNEL_WINDOW_BYTES)
		return EINVAL;

	/* Points the window at the range, writes it word by word, and puts the window back. */
	channel_window_take(gpu);

	saved = nvrtx_rd32(gpu, CHANNEL_REG_WINDOW_BASE);
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, (uint32_t)(base >> 16));
	for (at = 0U; at < length; at += 4U)
		nvrtx_wr32(gpu, CHANNEL_WINDOW_AT + offset + (uint32_t)at, nvrtx_get32(data, at));
	(void)nvrtx_rd32(gpu, CHANNEL_WINDOW_AT + offset);
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, saved);

	mutex_unlock(&gpu->window_lock);

	/* Succeeded: the bytes are in the VRAM. */
	return 0;
}

/* Writes one value over a run of the VRAM through the register window, then puts the window back. */
static int
channel_vram_fill(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	uint32_t value,
	size_t length)
{
	uint64_t base;
	uint32_t offset;
	uint32_t saved;
	size_t at;

	/* The range must lie inside one window position, in whole words. */
	base = address & ~0xffffULL;
	offset = (uint32_t)(address & 0xffffU);
	if ((length % 4U) != 0U || offset + length > CHANNEL_WINDOW_BYTES)
		return EINVAL;

	/* Points the window at the range, writes the value word by word, and puts the window back. */
	mutex_lock(&gpu->window_lock);

	saved = nvrtx_rd32(gpu, CHANNEL_REG_WINDOW_BASE);
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, (uint32_t)(base >> 16));
	for (at = 0U; at < length; at += 4U)
		nvrtx_wr32(gpu, CHANNEL_WINDOW_AT + offset + (uint32_t)at, value);
	(void)nvrtx_rd32(gpu, CHANNEL_WINDOW_AT + offset);
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, saved);

	mutex_unlock(&gpu->window_lock);

	/* Succeeded: the run holds the value. */
	return 0;
}

/*
 * Takes the register window's lock for a user other than a fill, counted
 * as waiting until it has the lock, so a fill steps aside for it.
 */
static void
channel_window_take(
	struct nvrtx_gpu *gpu)
{
	/* Counts the wait, takes the lock, and ends the count. */
	(void)__atomic_add_fetch(&gpu->window_wanted, 1U, __ATOMIC_ACQ_REL);
	mutex_lock(&gpu->window_lock);
	(void)__atomic_sub_fetch(&gpu->window_wanted, 1U, __ATOMIC_ACQ_REL);
}

/*
 * Steps a fill aside between its pieces, the window's lock free: gives the
 * CPU up while another user waits for the lock, until none does or
 * CHANNEL_FILL_ASIDE_US have passed.
 */
static void
channel_fill_aside(
	struct nvrtx_gpu *gpu)
{
	uint64_t start;
	uint64_t now;
	uint32_t wanted;

	/* Gives the CPU up while a user still waits, within the bound. */
	start = nvrtx_now_us();
	for (;;) {
		wanted = __atomic_load_n(&gpu->window_wanted, __ATOMIC_ACQUIRE);
		if (wanted == 0U)
			break;
		now = nvrtx_now_us();
		if (now - start >= CHANNEL_FILL_ASIDE_US)
			break;
		sched_yield();
	}
}

/* Reads one word of the VRAM through the register window, then puts the window back. */
static int
channel_vram_read32(
	struct nvrtx_gpu *gpu,
	uint64_t address,
	uint32_t *value)
{
	uint32_t saved;

	/* The word must be aligned. */
	if ((address % 4U) != 0U)
		return EINVAL;

	/* Points the window at the word, reads it, and puts the window back. */
	channel_window_take(gpu);

	saved = nvrtx_rd32(gpu, CHANNEL_REG_WINDOW_BASE);
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, (uint32_t)((address & ~0xffffULL) >> 16));
	*value = nvrtx_rd32(gpu, CHANNEL_WINDOW_AT + (uint32_t)(address & 0xffffU));
	nvrtx_wr32(gpu, CHANNEL_REG_WINDOW_BASE, saved);

	mutex_unlock(&gpu->window_lock);

	/* Succeeded: the word is read. */
	return 0;
}

/* Allocates the ring, the push buffer and the semaphore page. */
static int
channel_buffers(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	int error;

	engine = &gpu->engine;

	/* One page of ring entries. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &engine->ring);
	if (error != 0)
		return error;

	/* Four pages of push buffer. */
	error = nvrtx_buffer_alloc(gpu, CHANNEL_PUSH_PAGES * NVRTX_GSP_PAGE_SIZE, &engine->push);
	if (error != 0)
		return error;

	/* One page for the semaphore. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &engine->semaphore);
	if (error != 0)
		return error;

	/* Succeeded: the buffers are clear. */
	return 0;
}

/* Invalidates every TLB entry of the driver's page tables. */
static int
channel_flush_tlb(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* Invalidates the translations of the first address space's root. */
	error = nvrtx_flush_tlb(gpu, gpu->engine.vram_block);
	if (error != 0)
		return error;

	/* Succeeded: no stale translation is left. */
	return 0;
}

/*
 * Points ring entry index at a push-buffer segment, moves the ring's put
 * past it, and rings the doorbell.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/engine/fifo/tu102.c (tu102_chan_start).
 */
static int
channel_submit(
	struct nvrtx_gpu *gpu,
	size_t push_at,
	size_t length,
	uint32_t index)
{
	struct nvrtx_engine_state *engine;
	uint64_t push_va;
	uint8_t word[4];
	int error;

	engine = &gpu->engine;
	push_va = CHANNEL_VA + (uint64_t)CHANNEL_PUSH_PAGE * NVRTX_GSP_PAGE_SIZE + push_at;

	/* Writes the entry; the push buffer is already complete in memory. */
	nvrtx_ring_entry((uint8_t *)engine->ring.dma.address + (size_t)index * 8U, push_va, (uint32_t)(length / 4U));
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Moves the put in USERD (the window's read-back makes sure it reached the VRAM). */
	nvrtx_put32(word, 0U, index + 1U);
	error = channel_vram_write(gpu, engine->vram_block + CHANNEL_USERD_AT + CHANNEL_USERD_RING_PUT, word, sizeof(word));
	if (error != 0)
		return error;

	/* Tells the host the channel has work. */
	nvrtx_wr32(gpu, CHANNEL_REG_DOORBELL, (engine->pick.runlist << 16) | CHANNEL_ID);

	/* Succeeded: the segment is submitted. */
	return 0;
}

/* Polls the semaphore until it holds a value; reports how far the ring got when it does not. */
static int
channel_wait(
	struct nvrtx_gpu *gpu,
	uint32_t expected,
	uint32_t put)
{
	struct nvrtx_engine_state *engine;
	const volatile uint32_t *semaphore;
	uint64_t start;
	uint64_t waited;
	uint32_t value;
	uint32_t get;

	engine = &gpu->engine;
	semaphore = (const volatile uint32_t *)engine->semaphore.dma.address;

	/* Polls until the value is there or the time, measured, is spent. */
	start = nvrtx_now_us();
	for (;;) {
		value = *semaphore;
		waited = nvrtx_now_us() - start;
		if (value == expected) {
			nvrtx_stage_mark("copy-channel ok: fence 0x%x reached after %u us", (unsigned)value, (unsigned)waited);
			return 0;
		}

		if (waited > CHANNEL_FENCE_TIMEOUT_US)
			break;

		kern_usleep_range(CHANNEL_FENCE_STEP_US, CHANNEL_FENCE_STEP_US * 2U);
	}

	/* The value never came: reports the semaphore and the ring's get. */
	get = 0xffffffffU;
	(void)channel_vram_read32(gpu, engine->vram_block + CHANNEL_USERD_AT + CHANNEL_USERD_RING_GET, &get);
	nvrtx_stage_mark("copy-channel fail: fence %08x after 2 s, ring get %u put %u", (unsigned)*semaphore, (unsigned)get, (unsigned)put);
	return ETIMEDOUT;
}

/* Puts one fence in the channel and waits for the semaphore to reach its value. */
static int
channel_fence(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	uint64_t semaphore_va;
	size_t length;
	int error;

	engine = &gpu->engine;
	semaphore_va = CHANNEL_VA + (uint64_t)CHANNEL_SEMAPHORE_PAGE * NVRTX_GSP_PAGE_SIZE;

	/* Clears the semaphore page and writes the fence at the start of the push buffer. */
	kern_memset(engine->semaphore.dma.address, 0, NVRTX_GSP_PAGE_SIZE);
	length = nvrtx_push_fence((uint8_t *)engine->push.dma.address, semaphore_va, CHANNEL_FENCE_VALUE);

	/* Submits it as ring entry 0 and waits. */
	error = channel_submit(gpu, 0U, length, 0U);
	if (error != 0)
		return error;

	error = channel_wait(gpu, CHANNEL_FENCE_VALUE, 1U);
	if (error != 0)
		return error;

	/* Succeeded: the channel ran the fence. */
	return 0;
}

/* Has the copy engine fill words of the semaphore page, then a second fence, and checks the words. */
static int
channel_copy_fill(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_engine_state *engine;
	uint8_t params[NVRTX_COPY_PARAMS_BYTES];
	const volatile uint32_t *words;
	uint64_t semaphore_va;
	uint8_t *push;
	size_t length;
	uint32_t index;
	int error;

	engine = &gpu->engine;
	semaphore_va = CHANNEL_VA + (uint64_t)CHANNEL_SEMAPHORE_PAGE * NVRTX_GSP_PAGE_SIZE;

	/* Makes the copy object on the channel (version 1, the channel's copy engine). */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, 0U, 1U);
	nvrtx_put32(params, 4U, engine->pick.engine);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_CHANNEL, NVRTX_HANDLE_COPY, gpu->identity.chip.profile->class_copy, params, sizeof(params));
	if (error != 0)
		return error;

	/* The stop frees the copy object before its channel. */
	engine->copy_made = 1;

	/* Writes the fill and a second fence after it, in the push buffer's second half. */
	push = (uint8_t *)engine->push.dma.address + CHANNEL_FILL_PUSH_AT;
	length = nvrtx_push_copy_fill(push,
	    CHANNEL_COPY_SUBCHANNEL,
	    gpu->identity.chip.profile->class_copy,
	    semaphore_va + CHANNEL_FILL_AT,
	    CHANNEL_FILL_WORDS,
	    CHANNEL_FILL_VALUE);
	length += nvrtx_push_fence(push + length, semaphore_va, CHANNEL_FILL_FENCE_VALUE);

	/* Submits it as ring entry 1 and waits for the second fence. */
	error = channel_submit(gpu, CHANNEL_FILL_PUSH_AT, length, 1U);
	if (error != 0)
		return error;

	error = channel_wait(gpu, CHANNEL_FILL_FENCE_VALUE, 2U);
	if (error != 0)
		return error;

	/* Every filled word must hold the value, and the word after them must not. */
	words = (const volatile uint32_t *)((uint8_t *)engine->semaphore.dma.address + CHANNEL_FILL_AT);
	for (index = 0U; index < CHANNEL_FILL_WORDS; index++) {
		if (words[index] != CHANNEL_FILL_VALUE) {
			nvrtx_stage_mark("copy-channel fail: copy fill word %u is %08x", (unsigned)index, (unsigned)words[index]);
			return EIO;
		}
	}

	if (words[CHANNEL_FILL_WORDS] == CHANNEL_FILL_VALUE) {
		nvrtx_stage_mark("copy-channel fail: copy fill ran past its length");
		return EIO;
	}

	/* Succeeded: the copy engine wrote the words. */
	nvrtx_stage_mark("copy-channel ok: copy engine filled %u words", (unsigned)CHANNEL_FILL_WORDS);
	return 0;
}

/*
 * Names a root (in the VRAM: aperture 0) and runs one invalidate of the given
 * type, waiting for its trigger to clear.
 */
static int
channel_flush(
	struct nvrtx_gpu *gpu,
	uint64_t root,
	uint32_t type)
{
	int error;

	/* Triggers the invalidate and waits for it, one at a time. */
	channel_window_take(gpu);

	nvrtx_wr32(gpu, CHANNEL_REG_FLUSH_ROOT, (uint32_t)(root >> 8));
	nvrtx_wr32(gpu, CHANNEL_REG_FLUSH_ROOT_UPPER, 0U);
	nvrtx_wr32(gpu, CHANNEL_REG_FLUSH, CHANNEL_FLUSH_TRIGGER | type);
	error = nvrtx_poll(gpu, CHANNEL_REG_FLUSH, CHANNEL_FLUSH_TRIGGER, 0U, CHANNEL_FLUSH_TIMEOUT_US);

	mutex_unlock(&gpu->window_lock);

	/* Reports an invalidate that did not finish. */
	if (error != 0) {
		nvrtx_stage_mark("TLB invalidate of root %llx did not finish", (unsigned long long)root);
		return error;
	}

	/* Succeeded: no stale translation is left. */
	return 0;
}
