/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's monitor on zedBSD (WS134 p008, plan/ws134/design.md
 * section 1.3): the kernel's counters as the System Monitor shows them.
 *
 *   the CPUs     sysctl hw.cputimes (ws134-p005): each CPU's user, system,
 *                idle and other ticks
 *   the memory   /dev/system's KERN_SYSTEM_GET_VMSTAT (the total, the free
 *                and the swap) and sysctl vfs.cache_memory.stats (the
 *                caches, and the clean file data that can be dropped); read
 *                at most every 5 seconds, since the first walks the pages
 *   the links    the network area's interfaces (SIOCGIFSTATS), loopback
 *                aside; a link's id is made from its name
 *   the disks    sysctl hw.diskstats (ws134-p006): each physical disk
 *   the GPUs     sysctl hw.gputelemetry (ws134-p007): what their drivers
 *                keep (none on the virtual GPU)
 *   the CPU's    sysctl hw.thermal (ws134-p009): the processor's sensor,
 *   temperature  else the first ACPI thermal zone (none on QEMU's q35)
 *
 * Nothing here touches struct kl_backend: the compositor samples on a
 * thread of its own.  The GPUs' temperatures and the power have no source
 * on zedBSD yet (design.md section 1.5) and stay out of valid.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <uapi/cache-memory.h>
#include <uapi/sysctl.h>
#include <uapi/system.h>

/* How long the memory read stays good, in nanoseconds. */
#define MONITOR_MEMORY_NS	5000000000ULL

/* The most interfaces the network area is asked for (the loopback among them). */
#define MONITOR_INTERFACES	32U

/*
 * A monitor: /dev/system for the memory (-1 when it cannot be opened), the
 * last memory read and when it was taken (0: never), and the buffer the
 * variable sysctl values are read into, grown as they need.
 */
struct kl_backend_monitor {
	int system;
	uint64_t memory_at_ns;
	unsigned memory_valid;
	uint64_t memory_total;
	uint64_t memory_free;
	uint64_t memory_cache;
	uint64_t memory_reclaimable;
	uint64_t swap_total;
	uint64_t swap_used;
	unsigned char *buffer;
	size_t buffer_size;
};

static uint64_t monitor_now_ns(void);
static int monitor_fetch(struct kl_backend_monitor *monitor, const char *name, size_t *length);
static int monitor_entries(const struct kl_backend_monitor *monitor, size_t length, uint32_t version, size_t element_size, uint32_t *count);
static int monitor_cpus(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample);
static void monitor_memory(struct kl_backend_monitor *monitor, uint64_t now);
static unsigned monitor_links(struct kl_backend_monitor_link *links, struct kl_backend_monitor_link_info *infos);
static int monitor_disks(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample, struct kl_backend_monitor_info *info);
static int monitor_gpus(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample, struct kl_backend_monitor_info *info);
static void monitor_temperature(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample);
static uint64_t monitor_name_id(const char *name);

/*
 * Opens a monitor.  Returns NULL with errno set on ENOMEM.
 */
struct kl_backend_monitor *
kl_backend_monitor_open(void)
{
	struct kl_backend_monitor *monitor;

	/* The monitor. */
	monitor = calloc(1, sizeof(*monitor));
	if (monitor == NULL)
		return NULL;

	/* /dev/system for the memory; without it the samples have no memory. */
	monitor->system = open("/dev/system", O_RDONLY | O_CLOEXEC);

	/* Succeeded: the monitor. */
	return monitor;
}

/*
 * Reads the info: the CPUs, the machine's name, the GPUs, the disks and the
 * links.  Returns 0 or an errno value.
 */
int
kl_backend_monitor_info(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_info *info)
{
	struct kl_backend_monitor_sample *sample;
	struct kl_backend_monitor_link links[KL_MONITOR_LINK_MAX];
	uint32_t cpus;
	size_t length;
	uint64_t links_id;
	unsigned index;
	int status;

	/* Nothing known yet. */
	memset(info, 0, sizeof(*info));

	/* The CPUs. */
	length = sizeof(cpus);
	status = sysctlbyname("hw.ncpu", &cpus, &length, NULL, 0);
	if (status == 0)
		info->cpu_count = cpus;

	/* The machine's name. */
	status = gethostname(info->host, sizeof(info->host));
	if (status != 0)
		info->host[0] = '\0';
	info->host[sizeof(info->host) - 1U] = '\0';

	/* The disks and the GPUs fill their part of the info through a sample's reads. */
	sample = calloc(1, sizeof(*sample));
	if (sample == NULL)
		return ENOMEM;
	(void)monitor_disks(monitor, sample, info);
	(void)monitor_gpus(monitor, sample, info);
	free(sample);

	/* The links. */
	info->link_count = monitor_links(links, info->link);

	/*
	 * The set's generation: the disks' (the kernel's), with the links'
	 * names and the GPUs mixed in, so that it changes whenever one of them
	 * comes or goes.
	 */
	links_id = 0;
	for (index = 0; index < info->link_count; index++)
		links_id ^= info->link[index].id;
	info->generation ^= links_id ^ ((uint64_t)info->gpu_count << 56);

	/* Succeeded: the info. */
	return 0;
}

/*
 * Takes a sample of every area.  Returns 0 with valid saying what could be
 * read, or ENOTSUP when nothing could.
 */
int
kl_backend_monitor_sample(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample)
{
	struct kl_backend_monitor_info *info;
	uint64_t now;
	int status;

	/* Nothing read yet; the time first. */
	memset(sample, 0, sizeof(*sample));
	now = monitor_now_ns();
	sample->time_ns = now;

	/* The CPUs. */
	status = monitor_cpus(monitor, sample);
	if (status == 0)
		sample->valid |= KL_MONITOR_HAVE_CPU_TIMES;

	/* The memory, read again when the last read is old. */
	monitor_memory(monitor, now);
	if (monitor->memory_valid != 0U) {
		sample->memory_total = monitor->memory_total;
		sample->memory_free = monitor->memory_free;
		sample->memory_cache = monitor->memory_cache;
		sample->memory_reclaimable = monitor->memory_reclaimable;
		sample->swap_total = monitor->swap_total;
		sample->swap_used = monitor->swap_used;
		sample->valid |= KL_MONITOR_HAVE_MEMORY;
		sample->valid |= KL_MONITOR_HAVE_SWAP;
	}

	/* The links. */
	sample->link_count = monitor_links(sample->link, NULL);
	sample->valid |= KL_MONITOR_HAVE_LINKS;

	/* The disks and the GPUs (their info is not kept here). */
	info = calloc(1, sizeof(*info));
	if (info == NULL)
		return ENOMEM;
	status = monitor_disks(monitor, sample, info);
	if (status == 0)
		sample->valid |= KL_MONITOR_HAVE_DISKS;
	(void)monitor_gpus(monitor, sample, info);
	free(info);

	/* The CPU's temperature, where the machine has a sensor. */
	monitor_temperature(monitor, sample);

	/* Nothing at all. */
	if (sample->valid == 0U)
		return ENOTSUP;

	/* Succeeded: the sample. */
	return 0;
}

/*
 * Closes a monitor.
 */
void
kl_backend_monitor_close(
	struct kl_backend_monitor *monitor)
{
	/* Nothing to close. */
	if (monitor == NULL)
		return;

	/* /dev/system, the buffer and the monitor. */
	if (monitor->system >= 0)
		(void)close(monitor->system);
	free(monitor->buffer);
	free(monitor);
}

/* The monotonic clock in nanoseconds. */
static uint64_t
monitor_now_ns(void)
{
	struct timespec now;
	int status;

	/* The clock; without it, zero. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/*
 * Reads a sysctl value of the kernel's length (a header and its entries)
 * into the monitor's buffer, growing it as needed.  Returns 0 with the
 * value's length, or an errno value.
 */
static int
monitor_fetch(
	struct kl_backend_monitor *monitor,
	const char *name,
	size_t *length)
{
	unsigned char *grown;
	size_t size;
	int status;
	int attempt;

	/* A few tries: the value may grow between the length and the read. */
	size = 0;
	status = -1;
	for (attempt = 0; attempt < 4; attempt++) {
		/* The length the kernel needs now. */
		size = 0;
		status = sysctlbyname(name, NULL, &size, NULL, 0);
		if (status != 0)
			return errno;

		/* The buffer, grown when it is too small. */
		if (size > monitor->buffer_size) {
			grown = realloc(monitor->buffer, size);
			if (grown == NULL)
				return ENOMEM;
			monitor->buffer = grown;
			monitor->buffer_size = size;
		}

		/* The value; one that grew is asked for again. */
		status = sysctlbyname(name, monitor->buffer, &size, NULL, 0);
		if (status == 0)
			break;

		/* Any failure but a grown value ends the tries. */
		if (errno != ENOMEM)
			return errno;
	}

	/* Still growing after every try. */
	if (status != 0)
		return EAGAIN;

	/* Succeeded: the value's length. */
	*length = size;
	return 0;
}

/*
 * Checks a value in the buffer: a header of the version and an element
 * size, and its entries all within the length (the three headers share
 * their first four fields).  Returns 0 with the entries' count, or EINVAL.
 */
static int
monitor_entries(
	const struct kl_backend_monitor *monitor,
	size_t length,
	uint32_t version,
	size_t element_size,
	uint32_t *count)
{
	const struct cpu_times_header *header;

	/* Too short for a header. */
	if (length < sizeof(*header))
		return EINVAL;

	/* Another layout. */
	header = (const struct cpu_times_header *)(const void *)monitor->buffer;
	if (header->version != version)
		return EINVAL;
	if (header->element_size != element_size)
		return EINVAL;

	/* Entries beyond the value. */
	if (header->struct_size + (size_t)header->count * element_size > length)
		return EINVAL;

	/* The entries' count. */
	*count = header->count;

	/* Succeeded: the entries. */
	return 0;
}

/* Reads each CPU's ticks into a sample; returns 0 or an errno value. */
static int
monitor_cpus(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample)
{
	const struct cpu_times_header *header;
	const struct cpu_times_entry *entry;
	size_t length;
	uint32_t count;
	uint32_t cpu;
	int error;

	/* The value. */
	error = monitor_fetch(monitor, "hw.cputimes", &length);
	if (error != 0)
		return error;

	/* Of the known layout. */
	error = monitor_entries(monitor, length, CPU_TIMES_VERSION, sizeof(*entry), &count);
	if (error != 0)
		return error;

	/* The rate of the ticks and each CPU's, as many as fit. */
	header = (const struct cpu_times_header *)(const void *)monitor->buffer;
	sample->cpu_hz = header->hz;
	if (count > KL_MONITOR_CPU_MAX)
		count = KL_MONITOR_CPU_MAX;
	for (cpu = 0; cpu < count; cpu++) {
		entry = (const struct cpu_times_entry *)(const void *)(monitor->buffer + header->struct_size + (size_t)cpu * sizeof(*entry));
		sample->cpu[cpu].user = entry->user;
		sample->cpu[cpu].system = entry->system;
		sample->cpu[cpu].idle = entry->idle;
		sample->cpu[cpu].other = entry->other;
	}

	/* The CPUs read. */
	sample->cpu_count = count;

	/* Succeeded: the CPUs. */
	return 0;
}

/*
 * Reads the memory when the last read is older than 5 seconds: the total,
 * the free and the swap from /dev/system, the caches from
 * vfs.cache_memory.stats.  A failed read keeps the last one.
 */
static void
monitor_memory(
	struct kl_backend_monitor *monitor,
	uint64_t now)
{
	struct vm_statistics statistics;
	struct cache_memory_stats cache;
	const struct cache_memory_usage *file_data;
	size_t length;
	int status;

	/* The last read is still good. */
	if (monitor->memory_at_ns != 0U && now < monitor->memory_at_ns + MONITOR_MEMORY_NS)
		return;

	/* No /dev/system. */
	if (monitor->system < 0)
		return;

	/* The kernel's memory (this walks the pages). */
	memset(&statistics, 0, sizeof(statistics));
	status = ioctl(monitor->system, KERN_SYSTEM_GET_VMSTAT, &statistics);
	if (status != 0)
		return;
	monitor->memory_at_ns = now;

	/* The total, the free and the swap (the swap in pages). */
	monitor->memory_total = statistics.physical_total;
	monitor->memory_free = statistics.physical_free;
	monitor->swap_total = statistics.swap_total * KERN_SYSTEM_SWAP_PAGE_SIZE;
	monitor->swap_used = 0;
	if (statistics.swap_total > statistics.swap_free)
		monitor->swap_used = (statistics.swap_total - statistics.swap_free) * KERN_SYSTEM_SWAP_PAGE_SIZE;
	monitor->memory_valid = 1;

	/* The caches; without them, none. */
	monitor->memory_cache = 0;
	monitor->memory_reclaimable = 0;
	memset(&cache, 0, sizeof(cache));
	length = sizeof(cache);
	status = sysctlbyname("vfs.cache_memory.stats", &cache, &length, NULL, 0);
	if (status != 0)
		return;

	/* A value of another layout. */
	if (length != sizeof(cache))
		return;
	if (cache.version != CACHE_MEMORY_VERSION)
		return;

	/* Every cache, and the clean file data (resident and not waiting to be written). */
	monitor->memory_cache = cache.resident_bytes;
	file_data = &cache.usage[CACHE_MEMORY_FILE_DATA];
	if (file_data->resident_bytes > file_data->pending_bytes)
		monitor->memory_reclaimable = file_data->resident_bytes - file_data->pending_bytes;
}

/*
 * Reads the links (the loopback aside) into a sample's links and, when
 * infos is not NULL, the info's.  Returns how many.
 */
static unsigned
monitor_links(
	struct kl_backend_monitor_link *links,
	struct kl_backend_monitor_link_info *infos)
{
	struct kl_backend_network_link interfaces[MONITOR_INTERFACES];
	size_t found;
	size_t index;
	unsigned count;

	/* The interfaces, as the network area reads them. */
	found = kl_backend_network_get_links(interfaces, MONITOR_INTERFACES);
	if (found > MONITOR_INTERFACES)
		found = MONITOR_INTERFACES;

	/* Each one but the loopback, as many as fit. */
	count = 0;
	for (index = 0; index < found && count < KL_MONITOR_LINK_MAX; index++) {
		/* The loopback carries nothing of the machine's. */
		if (interfaces[index].loopback)
			continue;

		/* Its counters. */
		links[count].id = monitor_name_id(interfaces[index].name);
		links[count].rx_bytes = interfaces[index].received_bytes;
		links[count].tx_bytes = interfaces[index].sent_bytes;
		links[count].up = (unsigned)interfaces[index].up;

		/* Its info. */
		if (infos != NULL) {
			infos[count].id = links[count].id;
			infos[count].generation = 0;
			(void)snprintf(infos[count].name, sizeof(infos[count].name), "%s", interfaces[index].name);
		}

		/* The next place. */
		count++;
	}

	/* Succeeded: the links. */
	return count;
}

/*
 * Reads the disks into a sample and the info (its generation is the
 * kernel's set of disks').  Returns 0 or an errno value.
 */
static int
monitor_disks(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample,
	struct kl_backend_monitor_info *info)
{
	const struct disk_stats_header *header;
	const struct disk_stats_entry *entry;
	struct kl_backend_monitor_disk *disk;
	size_t length;
	uint32_t count;
	uint32_t index;
	int error;

	/* The value. */
	error = monitor_fetch(monitor, "hw.diskstats", &length);
	if (error != 0)
		return error;

	/* Of the known layout. */
	error = monitor_entries(monitor, length, DISK_STATS_VERSION, sizeof(*entry), &count);
	if (error != 0)
		return error;

	/* The set's generation, and each disk, as many as fit. */
	header = (const struct disk_stats_header *)(const void *)monitor->buffer;
	info->generation = header->generation;
	if (count > KL_MONITOR_DISK_MAX)
		count = KL_MONITOR_DISK_MAX;
	for (index = 0; index < count; index++) {
		entry = (const struct disk_stats_entry *)(const void *)(monitor->buffer + header->struct_size + (size_t)index * sizeof(*entry));

		/* Its work. */
		disk = &sample->disk[index];
		disk->id = entry->id;
		disk->read_ops = entry->read_ops;
		disk->write_ops = entry->write_ops;
		disk->read_bytes = entry->read_bytes;
		disk->write_bytes = entry->write_bytes;
		disk->read_ns = entry->read_ns;
		disk->write_ns = entry->write_ns;
		disk->busy_ns = entry->busy_ns;

		/* Its info (the kinds are the kernel's numbers; the size is not reported). */
		info->disk[index].id = entry->id;
		info->disk[index].generation = entry->generation;
		(void)snprintf(info->disk[index].name, sizeof(info->disk[index].name), "%.31s", entry->name);
		info->disk[index].kind = entry->kind;
		info->disk[index].size_bytes = 0;
	}

	/* The disks read. */
	sample->disk_count = count;
	info->disk_count = count;

	/* Succeeded: the disks. */
	return 0;
}

/*
 * Reads the GPUs whose drivers keep telemetry into a sample and the info.
 * Returns 0 or an errno value.
 */
static int
monitor_gpus(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample,
	struct kl_backend_monitor_info *info)
{
	const struct gpu_telemetry_header *header;
	const struct gpu_telemetry_entry *entry;
	struct kl_backend_monitor_gpu *gpu;
	size_t length;
	uint32_t count;
	uint32_t index;
	int error;

	/* The value. */
	error = monitor_fetch(monitor, "hw.gputelemetry", &length);
	if (error != 0)
		return error;

	/* Of the known layout. */
	error = monitor_entries(monitor, length, GPU_TELEMETRY_VERSION, sizeof(*entry), &count);
	if (error != 0)
		return error;

	/* Each GPU, as many as fit. */
	header = (const struct gpu_telemetry_header *)(const void *)monitor->buffer;
	if (count > KL_MONITOR_GPU_MAX)
		count = KL_MONITOR_GPU_MAX;
	for (index = 0; index < count; index++) {
		entry = (const struct gpu_telemetry_entry *)(const void *)(monitor->buffer + header->struct_size + (size_t)index * sizeof(*entry));

		/* Its place in the list is its id (the drivers list their GPUs once, for good). */
		gpu = &sample->gpu[index];
		gpu->id = index + 1U;
		gpu->time_ns = entry->time_ns;

		/* The busy time. */
		if ((entry->valid & GPU_TELEMETRY_BUSY) != 0U) {
			gpu->busy_ns = entry->busy_ns;
			sample->valid |= KL_MONITOR_HAVE_GPU_BUSY;
		}

		/* The frequency: the hardware's, else the one asked for. */
		if ((entry->valid & GPU_TELEMETRY_CUR_MHZ) != 0U) {
			gpu->cur_mhz = entry->cur_mhz;
			sample->valid |= KL_MONITOR_HAVE_GPU_FREQ;
		} else if ((entry->valid & GPU_TELEMETRY_REQ_MHZ) != 0U) {
			gpu->cur_mhz = entry->req_mhz;
			sample->valid |= KL_MONITOR_HAVE_GPU_FREQ;
		}

		/* The top of the range. */
		if ((entry->valid & GPU_TELEMETRY_RANGE_MHZ) != 0U)
			gpu->max_mhz = entry->max_mhz;

		/* The objects' memory. */
		if ((entry->valid & GPU_TELEMETRY_OBJECTS) != 0U) {
			gpu->memory_used = entry->objects_bytes;
			gpu->memory_total = entry->objects_limit;
			sample->valid |= KL_MONITOR_HAVE_GPU_MEMORY;
		}

		/* Its info: the driver's name stands for the GPU's until the compositor's Vulkan names it. */
		info->gpu[index].id = gpu->id;
		info->gpu[index].generation = 1;
		(void)snprintf(info->gpu[index].driver, sizeof(info->gpu[index].driver), "%.15s", entry->driver);
		(void)snprintf(info->gpu[index].name, sizeof(info->gpu[index].name), "%.15s", entry->driver);
	}

	/* The GPUs read. */
	sample->gpu_count = count;
	info->gpu_count = count;

	/* Succeeded: the GPUs. */
	return 0;
}

/*
 * Reads the CPU's temperature into a sample (ws134-p009): the sensor
 * hw.thermal marks the processor's, else the first thermal zone, when it
 * has a temperature.  A kernel without hw.thermal, or a machine without a
 * sensor, leaves the sample without one.
 */
static void
monitor_temperature(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample)
{
	const struct thermal_header *header;
	const struct thermal_entry *entry;
	const struct thermal_entry *chosen;
	size_t length;
	uint32_t count;
	uint32_t index;
	int error;

	/* The value, of the known layout. */
	error = monitor_fetch(monitor, "hw.thermal", &length);
	if (error != 0)
		return;
	error = monitor_entries(monitor, length, THERMAL_VERSION, sizeof(*entry), &count);
	if (error != 0)
		return;

	/* The processor's sensor, else the first zone, among those with a temperature. */
	header = (const struct thermal_header *)(const void *)monitor->buffer;
	chosen = NULL;
	for (index = 0; index < count; index++) {
		entry = (const struct thermal_entry *)(const void *)(monitor->buffer + header->struct_size + (size_t)index * sizeof(*entry));
		if ((entry->valid & THERMAL_HAVE_TEMPERATURE) == 0U)
			continue;
		if ((entry->flags & THERMAL_FLAG_CPU) != 0U) {
			chosen = entry;
			break;
		}

		/* Else the first zone, kept while no processor's sensor comes. */
		if (chosen == NULL && entry->kind == THERMAL_KIND_ZONE)
			chosen = entry;
	}

	/* None. */
	if (chosen == NULL)
		return;

	/* Succeeded: its temperature. */
	sample->cpu_milli_celsius = chosen->milli_celsius;
	sample->valid |= KL_MONITOR_HAVE_TEMPERATURE;
}

/* A link's id from its name (FNV-1a), the same for the same name; never 0. */
static uint64_t
monitor_name_id(
	const char *name)
{
	uint64_t hash;
	size_t index;

	/* Each byte of the name. */
	hash = 14695981039346656037ULL;
	for (index = 0; name[index] != '\0'; index++) {
		hash ^= (unsigned char)name[index];
		hash *= 1099511628211ULL;
	}

	/* Zero stands for no id. */
	if (hash == 0U)
		hash = 1;

	/* Succeeded: the id. */
	return hash;
}
