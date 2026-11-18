/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The work channel of the nvrtx driver: the parts that touch no hardware.
 *
 * The work channel (work.c) is the graphics channel the GPU device keeps for
 * the work it is given: one address space the driver owns over the 3.5 GiB at
 * NVRTX_WORK_VA_BASE, whose low part holds the graphics context's buffers at
 * the golden context's addresses, then the driver's own pages (the ring, the
 * semaphore and the fences' push buffers), then the runs the device maps for
 * its users and the regions of the sessions' own channels, which share the
 * space.  Each submission takes two ring entries: the caller's push buffer
 * segment, then a fence of the driver's that releases the next value of the
 * semaphore.
 */

#ifndef DRIVERS_GPU_NVRTX_WORK_H
#define DRIVERS_GPU_NVRTX_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The address space: seven first-level directory entries of 512 MiB each,
 * from 512 MiB up to the resource manager's range at 4 GiB (the users' runs
 * take every blob of every open, far more than one entry holds).
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmtu102.c (tu102_vmm, the
 * 29-bit level).
 */
#define NVRTX_WORK_VA_BASE		0x20000000ULL
#define NVRTX_WORK_VA_BYTES		0xe0000000ULL

/* The driver's own pages, after the graphics context's buffers, and the first address a mapping gets. */
#define NVRTX_WORK_VA_DRIVER		0x28000000ULL
#define NVRTX_WORK_VA_USER		0x28100000ULL

/*
 * The sessions' own channels: their numbers on the graphics runlist (after
 * the driver's channels: the golden context 1, the scrubber 2, the compute
 * launch 3, the work channel 4, the copy channel 8) and how many there are
 * at most.
 */
#define NVRTX_WORK_SESSION_CHID_FIRST	16U
#define NVRTX_WORK_SESSION_CHANNELS	32U

/* How many of a channel's last runs keep a record (their start and end stamps in the semaphore's page, and their submission's time). */
#define NVRTX_WORK_RUN_RECORDS		64U

/* The ring's entries (a power of two; one page of 8-byte entries) and the push buffer bytes of one fence. */
#define NVRTX_WORK_RING_ENTRIES		512U
#define NVRTX_WORK_FENCE_BYTES		64U

/*
 * The most words one ring entry carries (its length field, bits 30:10 of
 * its upper word).
 *
 * Value referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (NVC46F_GP_ENTRY1_LENGTH).
 */
#define NVRTX_WORK_SEGMENT_WORDS_MAX	0x1fffffU

/*
 * The two classes the channel has, and their subchannels.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state).
 */
#define NVRTX_WORK_SUBCHANNEL_THREED	0U
#define NVRTX_WORK_SUBCHANNEL_COMPUTE	1U

/*
 * The subchannel of the copy object the channel has with the video
 * decoder: the copy engine that shares the graphics runlist, which the
 * executor's copies out of block-linear video images run on.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h
 * (SUBC_NV90B5).
 */
#define NVRTX_WORK_SUBCHANNEL_COPY	4U

bool nvrtx_work_fence_passed(uint32_t seen, uint32_t value);
size_t nvrtx_work_push_objects(uint8_t *push, uint32_t threed_class, uint32_t compute_class);
size_t nvrtx_work_push_copy_object(uint8_t *push, uint32_t copy_class);

/*
 * The states of a channel of the work space (struct nvrtx_work_channel):
 * none before it exists, ready while it takes work, lost once its work
 * did not end (or the GSP reported an error on it) and its stop is
 * asked for, killing while the stop runs, and after the stop freed (the
 * resource manager let it go: nothing on the GPU reads its memory),
 * stopped (the GPU no longer runs it, but the resource manager keeps its
 * objects: its memory stays the device's) or stuck (nothing showed that it
 * stopped).
 */
#define NVRTX_WORK_CHANNEL_NONE		0U
#define NVRTX_WORK_CHANNEL_READY	1U
#define NVRTX_WORK_CHANNEL_LOST		2U
#define NVRTX_WORK_CHANNEL_KILLING	3U
#define NVRTX_WORK_CHANNEL_FREED	4U
#define NVRTX_WORK_CHANNEL_STOPPED	5U
#define NVRTX_WORK_CHANNEL_STUCK	6U

/*
 * What the recovery sees of one channel: whether it has an unfinished run
 * (busy), when its oldest unfinished run was submitted, whether that run
 * began on the GPU and the GPU's time it began, whether the graphics
 * engine holds the channel now (resident; 0 when the engine could not be
 * asked), and whether the channel may be stopped (ready, and stops allowed).
 */
struct nvrtx_recover_view {
	int busy;
	uint64_t oldest_submit_us;
	int started;
	uint64_t start_ns;
	int resident;
	int stoppable;
};

int nvrtx_recover_chid_next(uint32_t used, uint32_t last, uint32_t count);
int nvrtx_recover_culprit(const struct nvrtx_recover_view *views, uint32_t count, uint64_t now_us, uint64_t kill_end_us, uint32_t run_us);
uint32_t nvrtx_recover_kill_state(int stopped, int reported, int reset, int freed);
bool nvrtx_recover_stopped(uint32_t state, int busy);
size_t nvrtx_recover_stop_params(uint8_t *params);
size_t nvrtx_recover_reset_params(uint8_t *params, uint32_t engine_type, int reported);

#endif
