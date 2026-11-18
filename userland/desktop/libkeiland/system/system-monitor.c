/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's monitor for applications (WS134 p012, plan/ws134/design.md
 * section 1.3; libkeiland/system/kl-system-protocol.h's kl_system_monitor_v1): the
 * compositor's samples, taken on the kl_system's queue by
 * kl_system_dispatch, made into frames of rates (system-monitor-rate.c).
 *
 * A sample comes as its events and a sample_done: the events fill the
 * pending raw sample, and the done makes it the current one, makes the
 * frame from the one before, and acks it (the compositor sends the next
 * only then).  The info comes the same way, ended by info_done.
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include "system-private.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Marks protocol callback arguments that this client does not inspect. */
#define UNUSED_PARAMETER(name) ((void)(name))

/*
 * A monitor: its object, the info as told and how many times it changed,
 * the info being told, the raw sample being told, the last two whole
 * ones (have_current and have_previous say they are there), and the
 * newest frame and whether it was taken.
 */
struct kl_system_monitor {
	struct wl_proxy *proxy;
	struct kl_monitor_info info;
	unsigned info_changes;
	struct kl_monitor_info info_pending;
	struct system_monitor_raw pending;
	struct system_monitor_raw current;
	struct system_monitor_raw previous;
	unsigned have_current;
	struct kl_monitor_frame frame;
	unsigned frame_new;
};

/* The listener of kl_system_monitor_v1's events, in their order. */
struct system_monitor_listener {
	void (*info)(void *data, struct wl_proxy *proxy, uint32_t cpu_count, const char *host, uint32_t generation_high, uint32_t generation_low);
	void (*device)(void *data, struct wl_proxy *proxy, uint32_t kind, uint32_t id_high, uint32_t id_low, uint32_t generation_high, uint32_t generation_low, uint32_t subkind, const char *name, const char *driver);
	void (*info_done)(void *data, struct wl_proxy *proxy, uint32_t serial);
	void (*cpu)(void *data, struct wl_proxy *proxy, uint32_t index, uint32_t user_high, uint32_t user_low, uint32_t system_high, uint32_t system_low, uint32_t idle_high, uint32_t idle_low, uint32_t other_high, uint32_t other_low);
	void (*memory)(void *data, struct wl_proxy *proxy, uint32_t total_high, uint32_t total_low, uint32_t free_high, uint32_t free_low, uint32_t cache_high, uint32_t cache_low, uint32_t reclaimable_high, uint32_t reclaimable_low, uint32_t swap_total_high, uint32_t swap_total_low, uint32_t swap_used_high, uint32_t swap_used_low);
	void (*link)(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t rx_high, uint32_t rx_low, uint32_t tx_high, uint32_t tx_low, uint32_t up);
	void (*disk)(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t read_ops_high, uint32_t read_ops_low, uint32_t write_ops_high, uint32_t write_ops_low, uint32_t read_bytes_high, uint32_t read_bytes_low, uint32_t write_bytes_high, uint32_t write_bytes_low, uint32_t read_ns_high, uint32_t read_ns_low, uint32_t write_ns_high, uint32_t write_ns_low, uint32_t busy_ns_high, uint32_t busy_ns_low);
	void (*gpu)(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t time_high, uint32_t time_low, uint32_t busy_high, uint32_t busy_low, uint32_t used_high, uint32_t used_low, uint32_t total_high, uint32_t total_low, uint32_t cur_mhz, uint32_t max_mhz, int32_t milli_celsius, uint32_t milli_watts);
	void (*sample_done)(void *data, struct wl_proxy *proxy, uint32_t serial, uint32_t time_high, uint32_t time_low, uint32_t valid_high, uint32_t valid_low, uint32_t cpu_hz, int32_t cpu_milli_celsius);
};

static void monitor_info(void *data, struct wl_proxy *proxy, uint32_t cpu_count, const char *host, uint32_t generation_high, uint32_t generation_low);
static void monitor_device(void *data, struct wl_proxy *proxy, uint32_t kind, uint32_t id_high, uint32_t id_low, uint32_t generation_high, uint32_t generation_low, uint32_t subkind, const char *name, const char *driver);
static void monitor_info_done(void *data, struct wl_proxy *proxy, uint32_t serial);
static void monitor_cpu(void *data, struct wl_proxy *proxy, uint32_t index, uint32_t user_high, uint32_t user_low, uint32_t system_high, uint32_t system_low, uint32_t idle_high, uint32_t idle_low, uint32_t other_high, uint32_t other_low);
static void monitor_memory(void *data, struct wl_proxy *proxy, uint32_t total_high, uint32_t total_low, uint32_t free_high, uint32_t free_low, uint32_t cache_high, uint32_t cache_low, uint32_t reclaimable_high, uint32_t reclaimable_low, uint32_t swap_total_high, uint32_t swap_total_low, uint32_t swap_used_high, uint32_t swap_used_low);
static void monitor_link(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t rx_high, uint32_t rx_low, uint32_t tx_high, uint32_t tx_low, uint32_t up);
static void monitor_disk(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t read_ops_high, uint32_t read_ops_low, uint32_t write_ops_high, uint32_t write_ops_low, uint32_t read_bytes_high, uint32_t read_bytes_low, uint32_t write_bytes_high, uint32_t write_bytes_low, uint32_t read_ns_high, uint32_t read_ns_low, uint32_t write_ns_high, uint32_t write_ns_low, uint32_t busy_ns_high, uint32_t busy_ns_low);
static void monitor_gpu(void *data, struct wl_proxy *proxy, uint32_t id_high, uint32_t id_low, uint32_t time_high, uint32_t time_low, uint32_t busy_high, uint32_t busy_low, uint32_t used_high, uint32_t used_low, uint32_t total_high, uint32_t total_low, uint32_t cur_mhz, uint32_t max_mhz, int32_t milli_celsius, uint32_t milli_watts);
static void monitor_sample_done(void *data, struct wl_proxy *proxy, uint32_t serial, uint32_t time_high, uint32_t time_low, uint32_t valid_high, uint32_t valid_low, uint32_t cpu_hz, int32_t cpu_milli_celsius);
static uint64_t monitor_wide(uint32_t high, uint32_t low);

/* The monitor object's callbacks, which fill the monitor they are given. */
static const struct system_monitor_listener system_monitor_listener = {
	monitor_info,
	monitor_device,
	monitor_info_done,
	monitor_cpu,
	monitor_memory,
	monitor_link,
	monitor_disk,
	monitor_gpu,
	monitor_sample_done
};

/*
 * Opens a monitor sampled every period_ms.  Returns NULL with errno
 * ENOTSUP when the compositor offers none, EINVAL without a system, or
 * ENOMEM.
 */
struct kl_system_monitor *
kl_system_monitor_open(
	struct kl_system *system,
	unsigned period_ms)
{
	struct kl_system_monitor *monitor;

	/* A system to ask. */
	if (system == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* The monitor's record (large: the CPUs' counters twice and a frame). */
	monitor = calloc(1, sizeof(*monitor));
	if (monitor == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Its object on the system's queue. */
	monitor->proxy = system_monitor_make(system, period_ms, &system_monitor_listener, monitor);
	if (monitor->proxy == NULL) {
		free(monitor);
		errno = ENOTSUP;
		return NULL;
	}

	/* Succeeded: the samples come with kl_system_dispatch. */
	return monitor;
}

/*
 * Copies the newest frame: 1 when one came since the last take, 0 when
 * none did.
 */
int
kl_system_monitor_take(
	struct kl_system_monitor *monitor,
	struct kl_monitor_frame *frame)
{
	/* None new. */
	if (!monitor->frame_new)
		return 0;

	/* The frame, taken. */
	*frame = monitor->frame;
	monitor->frame_new = 0;

	/* Succeeded: a new frame. */
	return 1;
}

/*
 * The info as last told, and how many times it changed.
 */
const struct kl_monitor_info *
kl_system_monitor_info(
	const struct kl_system_monitor *monitor,
	unsigned *changes)
{
	/* How many times, for a caller that asks. */
	if (changes != NULL)
		*changes = monitor->info_changes;

	/* Succeeded: the info. */
	return &monitor->info;
}

/*
 * Closes a monitor.
 */
void
kl_system_monitor_close(
	struct kl_system_monitor *monitor)
{
	/* Nothing to close. */
	if (monitor == NULL)
		return;

	/* The object goes (the compositor stops sampling after its last), then the record. */
	wl_proxy_marshal(monitor->proxy, KL_SYSTEM_MONITOR_DESTROY);
	wl_proxy_destroy(monitor->proxy);
	free(monitor);
}

/* Starts an info: the machine. */
static void
monitor_info(
	void *data,
	struct wl_proxy *proxy,
	uint32_t cpu_count,
	const char *host,
	uint32_t generation_high,
	uint32_t generation_low)
{
	struct kl_system_monitor *monitor;

	UNUSED_PARAMETER(proxy);
	UNUSED_PARAMETER(generation_high);
	UNUSED_PARAMETER(generation_low);

	/* A new pending info with the machine. */
	monitor = data;
	memset(&monitor->info_pending, 0, sizeof(monitor->info_pending));
	monitor->info_pending.cpu_count = cpu_count;
	system_view_copy(monitor->info_pending.host, sizeof(monitor->info_pending.host), host);
}

/* Adds a device to the pending info: a GPU, a disk or a link. */
static void
monitor_device(
	void *data,
	struct wl_proxy *proxy,
	uint32_t kind,
	uint32_t id_high,
	uint32_t id_low,
	uint32_t generation_high,
	uint32_t generation_low,
	uint32_t subkind,
	const char *name,
	const char *driver)
{
	struct kl_system_monitor *monitor;
	struct kl_monitor_info *info;
	uint64_t id;

	UNUSED_PARAMETER(proxy);
	UNUSED_PARAMETER(generation_high);
	UNUSED_PARAMETER(generation_low);

	/* The device, by its kind, while there is room. */
	monitor = data;
	info = &monitor->info_pending;
	id = monitor_wide(id_high, id_low);
	if (kind == KL_SYSTEM_MONITOR_DEVICE_GPU && info->gpu_count < KL_MONITOR_GPU_MAX) {
		info->gpu[info->gpu_count].id = id;
		system_view_copy(info->gpu[info->gpu_count].name, sizeof(info->gpu[0].name), name);
		system_view_copy(info->gpu[info->gpu_count].driver, sizeof(info->gpu[0].driver), driver);
		info->gpu_count++;
	} else if (kind == KL_SYSTEM_MONITOR_DEVICE_DISK && info->disk_count < KL_MONITOR_DISK_MAX) {
		info->disk[info->disk_count].id = id;
		info->disk[info->disk_count].kind = subkind;
		system_view_copy(info->disk[info->disk_count].name, sizeof(info->disk[0].name), name);
		info->disk_count++;
	} else if (kind == KL_SYSTEM_MONITOR_DEVICE_LINK && info->link_count < KL_MONITOR_LINK_MAX) {
		info->link[info->link_count].id = id;
		system_view_copy(info->link[info->link_count].name, sizeof(info->link[0].name), name);
		info->link_count++;
	}
}

/* The info is whole: it is the one in effect. */
static void
monitor_info_done(
	void *data,
	struct wl_proxy *proxy,
	uint32_t serial)
{
	struct kl_system_monitor *monitor;

	UNUSED_PARAMETER(proxy);
	UNUSED_PARAMETER(serial);

	/* In effect, and counted as a change. */
	monitor = data;
	monitor->info = monitor->info_pending;
	monitor->info_changes++;
}

/* A CPU's ticks in the pending sample. */
static void
monitor_cpu(
	void *data,
	struct wl_proxy *proxy,
	uint32_t index,
	uint32_t user_high,
	uint32_t user_low,
	uint32_t system_high,
	uint32_t system_low,
	uint32_t idle_high,
	uint32_t idle_low,
	uint32_t other_high,
	uint32_t other_low)
{
	struct kl_system_monitor *monitor;
	struct system_monitor_cpu *cpu;

	UNUSED_PARAMETER(proxy);

	/* A CPU beyond the room is not kept. */
	monitor = data;
	if (index >= KL_MONITOR_CPU_MAX)
		return;

	/* Its ticks; the CPUs counted are as many as the highest index. */
	cpu = &monitor->pending.cpu[index];
	cpu->user = monitor_wide(user_high, user_low);
	cpu->system = monitor_wide(system_high, system_low);
	cpu->idle = monitor_wide(idle_high, idle_low);
	cpu->other = monitor_wide(other_high, other_low);
	if (index + 1U > monitor->pending.cpu_count)
		monitor->pending.cpu_count = index + 1U;
}

/* The memory in the pending sample. */
static void
monitor_memory(
	void *data,
	struct wl_proxy *proxy,
	uint32_t total_high,
	uint32_t total_low,
	uint32_t free_high,
	uint32_t free_low,
	uint32_t cache_high,
	uint32_t cache_low,
	uint32_t reclaimable_high,
	uint32_t reclaimable_low,
	uint32_t swap_total_high,
	uint32_t swap_total_low,
	uint32_t swap_used_high,
	uint32_t swap_used_low)
{
	struct kl_system_monitor *monitor;

	UNUSED_PARAMETER(proxy);

	/* Each value. */
	monitor = data;
	monitor->pending.memory_total = monitor_wide(total_high, total_low);
	monitor->pending.memory_free = monitor_wide(free_high, free_low);
	monitor->pending.memory_cache = monitor_wide(cache_high, cache_low);
	monitor->pending.memory_reclaimable = monitor_wide(reclaimable_high, reclaimable_low);
	monitor->pending.swap_total = monitor_wide(swap_total_high, swap_total_low);
	monitor->pending.swap_used = monitor_wide(swap_used_high, swap_used_low);
}

/* A link's counters in the pending sample. */
static void
monitor_link(
	void *data,
	struct wl_proxy *proxy,
	uint32_t id_high,
	uint32_t id_low,
	uint32_t rx_high,
	uint32_t rx_low,
	uint32_t tx_high,
	uint32_t tx_low,
	uint32_t up)
{
	struct kl_system_monitor *monitor;
	struct system_monitor_link *link;

	UNUSED_PARAMETER(proxy);

	/* A link beyond the room is not kept. */
	monitor = data;
	if (monitor->pending.link_count >= KL_MONITOR_LINK_MAX)
		return;

	/* Its counters. */
	link = &monitor->pending.link[monitor->pending.link_count];
	link->id = monitor_wide(id_high, id_low);
	link->rx = monitor_wide(rx_high, rx_low);
	link->tx = monitor_wide(tx_high, tx_low);
	link->up = up;
	monitor->pending.link_count++;
}

/* A disk's counters in the pending sample. */
static void
monitor_disk(
	void *data,
	struct wl_proxy *proxy,
	uint32_t id_high,
	uint32_t id_low,
	uint32_t read_ops_high,
	uint32_t read_ops_low,
	uint32_t write_ops_high,
	uint32_t write_ops_low,
	uint32_t read_bytes_high,
	uint32_t read_bytes_low,
	uint32_t write_bytes_high,
	uint32_t write_bytes_low,
	uint32_t read_ns_high,
	uint32_t read_ns_low,
	uint32_t write_ns_high,
	uint32_t write_ns_low,
	uint32_t busy_ns_high,
	uint32_t busy_ns_low)
{
	struct kl_system_monitor *monitor;
	struct system_monitor_disk *disk;

	UNUSED_PARAMETER(proxy);

	/* A disk beyond the room is not kept. */
	monitor = data;
	if (monitor->pending.disk_count >= KL_MONITOR_DISK_MAX)
		return;

	/* Its counters. */
	disk = &monitor->pending.disk[monitor->pending.disk_count];
	disk->id = monitor_wide(id_high, id_low);
	disk->read_ops = monitor_wide(read_ops_high, read_ops_low);
	disk->write_ops = monitor_wide(write_ops_high, write_ops_low);
	disk->read_bytes = monitor_wide(read_bytes_high, read_bytes_low);
	disk->write_bytes = monitor_wide(write_bytes_high, write_bytes_low);
	disk->read_ns = monitor_wide(read_ns_high, read_ns_low);
	disk->write_ns = monitor_wide(write_ns_high, write_ns_low);
	disk->busy_ns = monitor_wide(busy_ns_high, busy_ns_low);
	monitor->pending.disk_count++;
}

/* A GPU's counters and present values in the pending sample. */
static void
monitor_gpu(
	void *data,
	struct wl_proxy *proxy,
	uint32_t id_high,
	uint32_t id_low,
	uint32_t time_high,
	uint32_t time_low,
	uint32_t busy_high,
	uint32_t busy_low,
	uint32_t used_high,
	uint32_t used_low,
	uint32_t total_high,
	uint32_t total_low,
	uint32_t cur_mhz,
	uint32_t max_mhz,
	int32_t milli_celsius,
	uint32_t milli_watts)
{
	struct kl_system_monitor *monitor;
	struct system_monitor_gpu *gpu;

	UNUSED_PARAMETER(proxy);

	/* A GPU beyond the room is not kept. */
	monitor = data;
	if (monitor->pending.gpu_count >= KL_MONITOR_GPU_MAX)
		return;

	/* Its values. */
	gpu = &monitor->pending.gpu[monitor->pending.gpu_count];
	gpu->id = monitor_wide(id_high, id_low);
	gpu->time_ns = monitor_wide(time_high, time_low);
	gpu->busy_ns = monitor_wide(busy_high, busy_low);
	gpu->memory_used = monitor_wide(used_high, used_low);
	gpu->memory_total = monitor_wide(total_high, total_low);
	gpu->cur_mhz = cur_mhz;
	gpu->max_mhz = max_mhz;
	gpu->milli_celsius = milli_celsius;
	gpu->milli_watts = milli_watts;
	monitor->pending.gpu_count++;
}

/*
 * The sample is whole: it becomes the current one, the frame is made from
 * the one before (from the second sample on), and it is acked.
 */
static void
monitor_sample_done(
	void *data,
	struct wl_proxy *proxy,
	uint32_t serial,
	uint32_t time_high,
	uint32_t time_low,
	uint32_t valid_high,
	uint32_t valid_low,
	uint32_t cpu_hz,
	int32_t cpu_milli_celsius)
{
	struct kl_system_monitor *monitor;

	UNUSED_PARAMETER(valid_high);

	/* The sample's own values, and the sample in effect; the one before is kept. */
	monitor = data;
	monitor->pending.time_ns = monitor_wide(time_high, time_low);
	monitor->pending.valid = valid_low;
	monitor->pending.cpu_hz = cpu_hz;
	monitor->pending.cpu_milli_celsius = cpu_milli_celsius;
	monitor->previous = monitor->current;
	monitor->current = monitor->pending;
	memset(&monitor->pending, 0, sizeof(monitor->pending));

	/* A frame from the second sample on. */
	if (monitor->have_current) {
		system_monitor_rates(&monitor->previous, &monitor->current, &monitor->frame);
		monitor->frame_new = 1;
	}

	/* The next sample has one before it. */
	monitor->have_current = 1;

	/* Taken: the compositor may send the next. */
	wl_proxy_marshal(proxy, KL_SYSTEM_MONITOR_ACK, serial);
}

/* A u64 from its high and low halves. */
static uint64_t
monitor_wide(
	uint32_t high,
	uint32_t low)
{
	/* The halves together. */
	return ((uint64_t)high << 32) | (uint64_t)low;
}
