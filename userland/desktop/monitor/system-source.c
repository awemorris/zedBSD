/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's source of the machine itself (WS134 p013,
 * plan/ws134/design.md section 1.5): the frames of libkeiland's
 * kl_system_monitor_*, which the compositor samples, made into the
 * monitor's frames.
 *
 * A field the system does not give (the CPU's frequency everywhere, a
 * GPU's use and temperature on a virtual GPU, ...) is shown as 0, not a
 * stand-in (2026-10-22 user:
 * "テレメトリが取れないときは0表示にしてください。"): only the
 * simulation's own source makes values up.  Such a field is not valid, so
 * the rules never grade the system by it (rules.c).  No
 * device is made up: the GPUs are the system's, or the one that draws the
 * window when the system names none.
 */

#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How long the first info is waited for, in round trips of the display. */
#define SYSTEM_INFO_TRIES	40U

static void system_info(struct sm_source *source, struct sm_system *system);
static void system_frame(struct sm_source *source, struct sm_system *system, uint64_t now_ms, struct sm_frame *frame);

/*
 * Opens the system's source on the window's display: the desktop's system,
 * its monitor sampled every period_ms and the first info.  gpu_name names
 * the GPU that draws the window.  Returns 0, or ENOTSUP when the desktop
 * offers no monitor, or another errno value.
 */
int
sm_system_open(
	struct sm_source *source,
	struct sm_system *system,
	struct wl_display *display,
	unsigned period_ms,
	uint64_t seed,
	const char *gpu_name)
{
	const struct kl_monitor_info *info;
	unsigned changes;
	unsigned tries;
	unsigned gpus;
	unsigned index;
	int status;

	/* Nothing yet. */
	memset(system, 0, sizeof(*system));

	/* The desktop's system. */
	system->system = kl_system_open(display);
	if (system->system == NULL)
		return errno;

	/* Its monitor. */
	system->monitor = kl_system_monitor_open(system->system, period_ms);
	if (system->monitor == NULL) {
		status = errno;
		kl_system_close(system->system);
		system->system = NULL;
		return status;
	}

	/* The first info, which comes with the first sample. */
	changes = 0;
	for (tries = 0; tries < SYSTEM_INFO_TRIES && changes == 0U; tries++) {
		status = wl_display_roundtrip(display);
		if (status < 0)
			break;
		(void)kl_system_dispatch(system->system, NULL);
		(void)kl_system_monitor_info(system->monitor, &changes);
	}

	/* The machine's shape: the system's CPUs, its GPUs or the drawing one. */
	info = kl_system_monitor_info(system->monitor, &changes);
	gpus = info->gpu_count;
	if (gpus == 0U)
		gpus = 1;
	(void)sm_source_open_sim(source, seed, period_ms, info->cpu_count, gpus, 1);
	source->kind = SM_SOURCE_SYSTEM;

	/* No size is made up: each total is 0 until the system gives it. */
	source->info.memory_total = 0;
	source->info.swap_total = 0;
	for (index = 0; index < SM_GPU_MAX; index++)
		source->info.gpu_memory_total[index] = 0;

	/* The machine as the system tells it; the first GPU is named after the drawing device. */
	system_info(source, system);
	if (gpu_name != NULL && gpu_name[0] != '\0')
		(void)snprintf(source->info.gpu_name[0], SM_NAME_MAX, "%s", gpu_name);
	printf("ZMON SYSTEM open cpus=%u gpus=%u system_gpus=%u disks=%u links=%u changes=%u\n", source->info.cpu_count, source->info.gpu_count,
	       info->gpu_count, info->disk_count, info->link_count, changes);

	/* Succeeded: the frames come with the system's samples. */
	return 0;
}

/*
 * Closes the system's source.
 */
void
sm_system_close(
	struct sm_system *system)
{
	/* The monitor before its system. */
	if (system->monitor != NULL)
		kl_system_monitor_close(system->monitor);
	system->monitor = NULL;

	/* The system. */
	if (system->system != NULL)
		kl_system_close(system->system);
	system->system = NULL;
}

/*
 * Takes the system's events and gives a frame when a new sample came:
 * returns 1 with a frame, 0 when none came.
 */
int
sm_system_take(
	struct sm_source *source,
	struct sm_system *system,
	uint64_t now_ms,
	struct sm_frame *frame)
{
	unsigned changes;
	int taken;
	int status;

	/* The compositor's events the window's dispatch read. */
	status = kl_system_dispatch(system->system, NULL);
	if (status != 0)
		return 0;

	/* The info again when the devices changed. */
	(void)kl_system_monitor_info(system->monitor, &changes);
	if (changes != system->info_changes)
		system_info(source, system);

	/* A new sample's frame. */
	taken = kl_system_monitor_take(system->monitor, &system->frame);
	if (!taken)
		return 0;

	/* Made into the monitor's frame. */
	system_frame(source, system, now_ms, frame);

	/* Succeeded: a frame. */
	return 1;
}

/* Takes the system's info into the source's: its name and CPUs, and its GPUs' names when it has any. */
static void
system_info(
	struct sm_source *source,
	struct sm_system *system)
{
	const struct kl_monitor_info *info;
	unsigned index;

	/* The info, and its number of changes. */
	info = kl_system_monitor_info(system->monitor, &system->info_changes);

	/* The machine's name. */
	if (info->host[0] != '\0')
		(void)snprintf(source->info.host, SM_NAME_MAX, "%s", info->host);

	/* Its CPUs. */
	if (info->cpu_count != 0U) {
		source->info.cpu_count = info->cpu_count;
		if (source->info.cpu_count > SM_CPU_MAX)
			source->info.cpu_count = SM_CPU_MAX;
	}

	/* Its GPUs, named after their drivers (the first is renamed after the drawing device at the open). */
	for (index = 0; index < info->gpu_count && index < SM_GPU_MAX; index++) {
		if (index != 0U)
			(void)snprintf(source->info.gpu_name[index], SM_NAME_MAX, "%s", info->gpu[index].name);
	}
}

/*
 * Makes the monitor's frame from the system's: every value 0, then each
 * field the system gave, marked valid.  Nothing is simulated: a field the
 * system left out is drawn as 0 and, not being valid, grades nothing.
 */
static void
system_frame(
	struct sm_source *source,
	struct sm_system *system,
	uint64_t now_ms,
	struct sm_frame *frame)
{
	const struct kl_monitor_frame *real;
	uint64_t used;
	unsigned count;
	unsigned index;

	/* A frame at the time with every value 0 and no field given yet. */
	real = &system->frame;
	memset(frame, 0, sizeof(*frame));
	frame->time_ms = now_ms;
	frame->valid = 0;
	frame->simulated = 0;

	/* The CPUs: the whole and each. */
	if ((real->valid & KL_MONITOR_FRAME_CPU) != 0U && real->cpu_count != 0U) {
		frame->cpu = real->cpu;
		count = real->cpu_count;
		if (count > SM_CPU_MAX)
			count = SM_CPU_MAX;
		for (index = 0; index < count; index++)
			frame->cpu_core[index] = real->cpu_core[index];
		frame->valid |= SM_HAVE_CPU;
	}

	/* The memory: in use is what is neither free nor a cache; available is the free and the caches that can be dropped. */
	if ((real->valid & KL_MONITOR_FRAME_MEMORY) != 0U && real->memory_total != 0U) {
		source->info.memory_total = real->memory_total;
		used = 0;
		if (real->memory_total > real->memory_free + real->memory_cache)
			used = real->memory_total - real->memory_free - real->memory_cache;
		frame->memory_used = used;
		frame->memory_cache = real->memory_cache;
		frame->memory_available = real->memory_free + real->memory_reclaimable;
		frame->valid |= SM_HAVE_MEMORY;
	}

	/* The swap. */
	if ((real->valid & KL_MONITOR_FRAME_SWAP) != 0U) {
		source->info.swap_total = real->swap_total;
		frame->swap_used = real->swap_used;
		frame->valid |= SM_HAVE_SWAP;
	}

	/* The network's bytes a second. */
	if ((real->valid & KL_MONITOR_FRAME_LINKS) != 0U) {
		frame->rx_rate = real->rx_rate;
		frame->tx_rate = real->tx_rate;
		frame->valid |= SM_HAVE_NETWORK;
	}

	/* The disks' bytes a second and their latency. */
	if ((real->valid & KL_MONITOR_FRAME_DISKS) != 0U) {
		frame->read_rate = real->read_rate;
		frame->write_rate = real->write_rate;
		frame->disk_latency_ms = real->disk_latency_ms;
		frame->valid |= SM_HAVE_DISK;
		frame->valid |= SM_HAVE_DISK_LATENCY;
	}

	/* The GPUs the system has (none on a virtual GPU: the drawing one stays at 0). */
	count = real->gpu_count;
	if (count > source->info.gpu_count)
		count = source->info.gpu_count;
	for (index = 0; index < count; index++) {
		/* Its use. */
		if ((real->valid & KL_MONITOR_FRAME_GPU_BUSY) != 0U) {
			frame->gpu_busy[index] = real->gpu[index].busy;
			frame->valid |= SM_HAVE_GPU_BUSY;
		}

		/* Its memory, and the limit it is held within. */
		if ((real->valid & KL_MONITOR_FRAME_GPU_MEMORY) != 0U) {
			source->info.gpu_memory_total[index] = real->gpu[index].memory_total;
			frame->gpu_memory_used[index] = real->gpu[index].memory_used;
			frame->valid |= SM_HAVE_GPU_MEMORY;
		}

		/* Its temperature (the frame's temperatures may be the CPU's alone: a GPU without one reads 0). */
		if ((real->valid & KL_MONITOR_FRAME_TEMPERATURE) != 0U && real->gpu[index].milli_celsius != 0) {
			frame->gpu_celsius[index] = (double)real->gpu[index].milli_celsius / 1000.0;
			frame->valid |= SM_HAVE_GPU_TEMPERATURE;
		}

		/* Its power, likewise. */
		if ((real->valid & KL_MONITOR_FRAME_POWER) != 0U && real->gpu[index].milli_watts != 0U) {
			frame->gpu_watts[index] = (double)real->gpu[index].milli_watts / 1000.0;
			frame->valid |= SM_HAVE_GPU_POWER;
		}
	}
}
