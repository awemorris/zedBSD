/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's monitor on Linux (WS134 p011, plan/ws134/design.md section
 * 1.3): the counters /proc and /sys keep, as the System Monitor shows them.
 *
 *   the CPUs     /proc/stat's cpuN lines: user and nice are user, system,
 *                irq and softirq are system, idle and iowait are idle,
 *                steal is other; the ticks are USER_HZ
 *   the memory   /proc/meminfo: MemTotal, MemFree, the caches (Cached,
 *                Buffers and SReclaimable), what can be dropped at once
 *                (MemAvailable less MemFree), SwapTotal and SwapFree
 *   the links    the network area's interfaces, loopback aside
 *   the disks    /proc/diskstats for the whole disks of /sys/block (no
 *                partition, loop, RAM or device-mapper disk)
 *   the GPUs     /sys/class/drm/cardN: amdgpu's gpu_busy_percent (added up
 *                into a busy time here) and its memory, i915's frequencies,
 *                the hwmon temperature and power
 *   temperature  the CPU package's thermal zone (x86_pkg_temp) or hwmon
 *                (coretemp, k10temp)
 *
 * Nothing here touches struct kl_backend: the compositor samples on a
 * thread of its own.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The most interfaces the network area is asked for (the loopback among them). */
#define MONITOR_INTERFACES	32U

/* The longest line of /proc read. */
#define MONITOR_LINE_MAX	512U

/*
 * A monitor: the GPUs' cards found by the info (their /sys/class/drm
 * directories and ids), and for each the busy time added up from its busy
 * percentage and when it was last read (0: never).
 */
struct kl_backend_monitor {
	unsigned gpu_count;
	char gpu_path[KL_MONITOR_GPU_MAX][64];
	uint64_t gpu_id[KL_MONITOR_GPU_MAX];
	uint64_t gpu_busy_ns[KL_MONITOR_GPU_MAX];
	uint64_t gpu_read_ns[KL_MONITOR_GPU_MAX];
};

static uint64_t monitor_now_ns(void);
static int monitor_cpus(struct kl_backend_monitor_sample *sample);
static int monitor_memory(struct kl_backend_monitor_sample *sample);
static unsigned monitor_links(struct kl_backend_monitor_link *links, struct kl_backend_monitor_link_info *infos);
static int monitor_whole_disk(const char *name);
static unsigned monitor_disk_kind(const char *name);
static int monitor_disks(struct kl_backend_monitor_sample *sample, struct kl_backend_monitor_info *info);
static void monitor_find_gpus(struct kl_backend_monitor *monitor, struct kl_backend_monitor_info *info);
static void monitor_gpus(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample, uint64_t now);
static int monitor_read_number(const char *path, long long *value);
static int monitor_hwmon_number(const char *directory, const char *file, long long *value);
static void monitor_cpu_temperature(struct kl_backend_monitor_sample *sample);
static uint64_t monitor_name_id(const char *name);

/*
 * Opens a monitor.  Returns NULL with errno set on ENOMEM.
 */
struct kl_backend_monitor *
kl_backend_monitor_open(void)
{
	struct kl_backend_monitor *monitor;
	struct kl_backend_monitor_info *info;

	/* The monitor. */
	monitor = calloc(1, sizeof(*monitor));
	if (monitor == NULL)
		return NULL;

	/* The GPUs' cards, found once (their busy time is added up from now). */
	info = calloc(1, sizeof(*info));
	if (info == NULL) {
		free(monitor);
		return NULL;
	}

	/* Found through an info. */
	monitor_find_gpus(monitor, info);
	free(info);

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
	long online;
	uint64_t mixed;
	unsigned index;
	int status;

	/* Nothing known yet. */
	memset(info, 0, sizeof(*info));

	/* The CPUs the system has. */
	online = sysconf(_SC_NPROCESSORS_CONF);
	if (online > 0)
		info->cpu_count = (unsigned)online;

	/* The machine's name. */
	status = gethostname(info->host, sizeof(info->host));
	if (status != 0)
		info->host[0] = '\0';
	info->host[sizeof(info->host) - 1U] = '\0';

	/* The disks, through a sample's read. */
	sample = calloc(1, sizeof(*sample));
	if (sample == NULL)
		return ENOMEM;
	(void)monitor_disks(sample, info);
	free(sample);

	/* The GPUs, found again (a card that came is followed from now). */
	monitor_find_gpus(monitor, info);

	/* The links. */
	info->link_count = monitor_links(links, info->link);

	/* The set's generation: the devices' ids mixed, so that it changes whenever one comes or goes. */
	mixed = (uint64_t)info->gpu_count << 56;
	for (index = 0; index < info->disk_count; index++)
		mixed ^= info->disk[index].id;
	for (index = 0; index < info->link_count; index++)
		mixed ^= info->link[index].id * 3U;
	info->generation = mixed;

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
	status = monitor_cpus(sample);
	if (status == 0)
		sample->valid |= KL_MONITOR_HAVE_CPU_TIMES;

	/* The memory and the swap. */
	status = monitor_memory(sample);
	if (status == 0) {
		sample->valid |= KL_MONITOR_HAVE_MEMORY;
		sample->valid |= KL_MONITOR_HAVE_SWAP;
	}

	/* The links. */
	sample->link_count = monitor_links(sample->link, NULL);
	sample->valid |= KL_MONITOR_HAVE_LINKS;

	/* The disks (their info is not kept here). */
	info = calloc(1, sizeof(*info));
	if (info == NULL)
		return ENOMEM;
	status = monitor_disks(sample, info);
	if (status == 0)
		sample->valid |= KL_MONITOR_HAVE_DISKS;
	free(info);

	/* The GPUs and the CPU's temperature. */
	monitor_gpus(monitor, sample, now);
	monitor_cpu_temperature(sample);

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
	/* The monitor. */
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

/* Reads each CPU's ticks from /proc/stat; returns 0 or an errno value. */
static int
monitor_cpus(
	struct kl_backend_monitor_sample *sample)
{
	unsigned long long ticks[8];
	char line[MONITOR_LINE_MAX];
	FILE *file;
	char *read;
	unsigned cpu;
	unsigned max;
	long hz;
	int fields;
	int differs;

	/* The file. */
	file = fopen("/proc/stat", "r");
	if (file == NULL)
		return errno;

	/* Each "cpuN" line (not the total "cpu "). */
	max = 0;
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* Not a CPU's line (the total's "cpu " has no number). */
		differs = strncmp(line, "cpu", 3);
		if (differs != 0)
			continue;
		if (line[3] < '0' || line[3] > '9')
			continue;

		/* Its number and its ticks. */
		memset(ticks, 0, sizeof(ticks));
		fields = sscanf(line + 3, "%u %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &ticks[0], &ticks[1], &ticks[2], &ticks[3],
				&ticks[4], &ticks[5], &ticks[6], &ticks[7]);
		if (fields < 5)
			continue;
		if (cpu >= KL_MONITOR_CPU_MAX)
			continue;

		/* user + nice, system + irq + softirq, idle + iowait, steal. */
		sample->cpu[cpu].user = ticks[0] + ticks[1];
		sample->cpu[cpu].system = ticks[2] + ticks[5] + ticks[6];
		sample->cpu[cpu].idle = ticks[3] + ticks[4];
		sample->cpu[cpu].other = ticks[7];
		if (cpu + 1U > max)
			max = cpu + 1U;
	}

	/* The file goes. */
	(void)fclose(file);

	/* No CPU at all. */
	if (max == 0U)
		return ENOENT;

	/* Succeeded: the CPUs, counted in USER_HZ. */
	sample->cpu_count = max;
	hz = sysconf(_SC_CLK_TCK);
	sample->cpu_hz = 100;
	if (hz > 0)
		sample->cpu_hz = (uint64_t)hz;

	/* Succeeded: the CPUs. */
	return 0;
}

/* Reads the memory and the swap from /proc/meminfo; returns 0 or an errno value. */
static int
monitor_memory(
	struct kl_backend_monitor_sample *sample)
{
	static const char *const names[] = {
		"MemTotal", "MemFree", "MemAvailable", "Cached", "Buffers", "SReclaimable", "SwapTotal", "SwapFree"
	};
	char line[MONITOR_LINE_MAX];
	char name[64];
	unsigned long long kib;
	unsigned long long total;
	unsigned long long free_kib;
	unsigned long long available;
	unsigned long long cache;
	unsigned long long swap_total;
	unsigned long long swap_free;
	FILE *file;
	char *read;
	size_t wanted;
	int fields;
	int found;
	int differs;

	/* The file. */
	file = fopen("/proc/meminfo", "r");
	if (file == NULL)
		return errno;

	/* Each "Name: N kB" line that is wanted. */
	total = 0;
	free_kib = 0;
	available = 0;
	cache = 0;
	swap_total = 0;
	swap_free = 0;
	found = 0;
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* The name and the number. */
		fields = sscanf(line, "%63[^:]: %llu", name, &kib);
		if (fields != 2)
			continue;

		/* Which of the wanted names it is. */
		for (wanted = 0; wanted < sizeof(names) / sizeof(names[0]); wanted++) {
			differs = strcmp(name, names[wanted]);
			if (differs == 0)
				break;
		}

		/* Each wanted name to its place (the three caches add up). */
		switch (wanted) {
		case 0:
			total = kib;
			found = 1;
			break;
		case 1:
			free_kib = kib;
			break;
		case 2:
			available = kib;
			break;
		case 3:
		case 4:
		case 5:
			cache += kib;
			break;
		case 6:
			swap_total = kib;
			break;
		case 7:
			swap_free = kib;
			break;
		default:
			break;
		}
	}

	/* The file goes. */
	(void)fclose(file);

	/* Without the total there is nothing. */
	if (!found)
		return ENOENT;

	/* In bytes. */
	sample->memory_total = total * 1024U;
	sample->memory_free = free_kib * 1024U;
	sample->memory_cache = cache * 1024U;
	sample->memory_reclaimable = 0;
	if (available > free_kib)
		sample->memory_reclaimable = (available - free_kib) * 1024U;
	sample->swap_total = swap_total * 1024U;
	sample->swap_used = 0;
	if (swap_total > swap_free)
		sample->swap_used = (swap_total - swap_free) * 1024U;

	/* Succeeded: the memory. */
	return 0;
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

/* Whether a block device is a whole disk the monitor shows: in /sys/block, and not a loop, RAM or device-mapper disk. */
static int
monitor_whole_disk(
	const char *name)
{
	static const char *const virtual_prefixes[] = { "loop", "ram", "zram", "dm-" };
	char path[PATH_MAX];
	size_t kind;
	int status;
	int differs;

	/* The kinds that are no physical disk. */
	for (kind = 0; kind < sizeof(virtual_prefixes) / sizeof(virtual_prefixes[0]); kind++) {
		differs = strncmp(name, virtual_prefixes[kind], strlen(virtual_prefixes[kind]));
		if (differs == 0)
			return 0;
	}

	/* A partition has no directory of its own in /sys/block. */
	(void)snprintf(path, sizeof(path), "/sys/block/%s", name);
	status = access(path, F_OK);
	if (status != 0)
		return 0;

	/* Succeeded: a whole disk. */
	return 1;
}

/* A whole disk's kind: NVMe by its name, USB by its place on the bus, else another. */
static unsigned
monitor_disk_kind(
	const char *name)
{
	char path[PATH_MAX];
	char target[PATH_MAX];
	const char *usb;
	ssize_t length;
	int differs;

	/* NVMe. */
	differs = strncmp(name, "nvme", 4);
	if (differs == 0)
		return KL_MONITOR_DISK_NVME;

	/* Where the device sits: a USB device's path has "/usb" in it. */
	(void)snprintf(path, sizeof(path), "/sys/block/%s", name);
	length = readlink(path, target, sizeof(target) - 1U);
	if (length <= 0)
		return KL_MONITOR_DISK_OTHER;
	target[length] = '\0';
	usb = strstr(target, "/usb");
	if (usb != NULL)
		return KL_MONITOR_DISK_USB;

	/* An SD or MMC card. */
	differs = strncmp(name, "mmcblk", 6);
	if (differs == 0)
		return KL_MONITOR_DISK_SDMMC;

	/* Succeeded: another kind. */
	return KL_MONITOR_DISK_OTHER;
}

/*
 * Reads the whole disks of /proc/diskstats into a sample and the info.
 * Returns 0 or an errno value.
 */
static int
monitor_disks(
	struct kl_backend_monitor_sample *sample,
	struct kl_backend_monitor_info *info)
{
	unsigned long long column[11];
	struct kl_backend_monitor_disk *disk;
	char line[MONITOR_LINE_MAX];
	char name[64];
	unsigned major;
	unsigned minor;
	unsigned count;
	FILE *file;
	char *read;
	int fields;
	int whole;

	/* The file. */
	file = fopen("/proc/diskstats", "r");
	if (file == NULL)
		return errno;

	/* Each whole disk's line, as many as fit. */
	count = 0;
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;
		if (count >= KL_MONITOR_DISK_MAX)
			break;

		/* major minor name, then reads, reads merged, sectors read, ms reading, writes, writes merged, sectors written, ms writing, in flight, ms busy. */
		fields = sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &major, &minor, name, &column[0], &column[1],
				&column[2], &column[3], &column[4], &column[5], &column[6], &column[7], &column[8], &column[9]);
		if (fields < 13)
			continue;

		/* Only a whole disk. */
		whole = monitor_whole_disk(name);
		if (!whole)
			continue;

		/* Its work (sectors of 512 bytes, times in milliseconds). */
		disk = &sample->disk[count];
		disk->id = monitor_name_id(name);
		disk->read_ops = column[0];
		disk->read_bytes = column[2] * 512U;
		disk->read_ns = column[3] * 1000000U;
		disk->write_ops = column[4];
		disk->write_bytes = column[6] * 512U;
		disk->write_ns = column[7] * 1000000U;
		disk->busy_ns = column[9] * 1000000U;

		/* Its info. */
		info->disk[count].id = disk->id;
		info->disk[count].generation = 0;
		(void)snprintf(info->disk[count].name, sizeof(info->disk[count].name), "%.31s", name);
		info->disk[count].kind = monitor_disk_kind(name);
		count++;
	}

	/* The file goes. */
	(void)fclose(file);

	/* The disks read. */
	sample->disk_count = count;
	info->disk_count = count;

	/* Succeeded: the disks. */
	return 0;
}

/*
 * Finds the GPUs: /sys/class/drm's cardN (not their connectors), with
 * their driver's name.  A card already followed keeps its busy time.
 */
static void
monitor_find_gpus(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_info *info)
{
	char path[PATH_MAX];
	char target[PATH_MAX];
	const char *driver;
	struct dirent *entry;
	DIR *directory;
	ssize_t length;
	const char *dash;
	unsigned count;
	unsigned index;
	int known;
	int differs;

	/* The DRM class; none without it. */
	directory = opendir("/sys/class/drm");
	if (directory == NULL)
		return;

	/* Each "cardN". */
	count = 0;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		if (count >= KL_MONITOR_GPU_MAX)
			break;

		/* Only a card itself (cardN, no "-connector"). */
		differs = strncmp(entry->d_name, "card", 4);
		if (differs != 0)
			continue;
		dash = strchr(entry->d_name, '-');
		if (dash != NULL)
			continue;

		/* Its driver's name. */
		(void)snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", entry->d_name);
		length = readlink(path, target, sizeof(target) - 1U);
		if (length <= 0)
			continue;
		target[length] = '\0';
		driver = strrchr(target, '/');
		if (driver == NULL)
			driver = target;
		else
			driver++;

		/* Its info. */
		info->gpu[count].id = monitor_name_id(entry->d_name);
		info->gpu[count].generation = 0;
		(void)snprintf(info->gpu[count].driver, sizeof(info->gpu[count].driver), "%.15s", driver);
		(void)snprintf(info->gpu[count].name, sizeof(info->gpu[count].name), "%.15s", driver);

		/* Followed: its directory, its busy time kept when it was followed already. */
		(void)snprintf(path, sizeof(path), "/sys/class/drm/%s", entry->d_name);
		known = 0;
		for (index = 0; index < monitor->gpu_count; index++) {
			differs = strcmp(monitor->gpu_path[index], path);
			if (differs == 0)
				known = 1;
		}

		/* A new card starts its busy time now. */
		if (!known || count >= monitor->gpu_count) {
			monitor->gpu_busy_ns[count] = 0;
			monitor->gpu_read_ns[count] = 0;
		}

		/* Its directory and id for the samples. */
		(void)snprintf(monitor->gpu_path[count], sizeof(monitor->gpu_path[count]), "%s", path);
		monitor->gpu_id[count] = info->gpu[count].id;
		count++;
	}

	/* The directory goes. */
	(void)closedir(directory);

	/* The cards found. */
	monitor->gpu_count = count;
	info->gpu_count = count;
}

/*
 * Reads each GPU: its busy percentage (amdgpu) added up into a busy time
 * since the last read, its memory (amdgpu), its frequencies (i915's
 * gt_cur_freq_mhz and gt_max_freq_mhz), and its hwmon temperature and
 * power.
 */
static void
monitor_gpus(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample,
	uint64_t now)
{
	struct kl_backend_monitor_gpu *gpu;
	char path[PATH_MAX];
	char device[PATH_MAX];
	long long value;
	long long total;
	unsigned index;
	int error;

	/* Each card the info found. */
	for (index = 0; index < monitor->gpu_count; index++) {
		gpu = &sample->gpu[index];
		gpu->id = monitor->gpu_id[index];
		gpu->time_ns = now;
		(void)snprintf(device, sizeof(device), "%s/device", monitor->gpu_path[index]);

		/* The busy percentage, added up over the time since the last read. */
		(void)snprintf(path, sizeof(path), "%s/gpu_busy_percent", device);
		error = monitor_read_number(path, &value);
		if (error == 0 &&
		    value >= 0 &&
		    value <= 100) {
			if (monitor->gpu_read_ns[index] != 0U && now > monitor->gpu_read_ns[index])
				monitor->gpu_busy_ns[index] += (now - monitor->gpu_read_ns[index]) / 100U * (uint64_t)value;
			monitor->gpu_read_ns[index] = now;
			gpu->busy_ns = monitor->gpu_busy_ns[index];
			sample->valid |= KL_MONITOR_HAVE_GPU_BUSY;
		}

		/* The video memory in use (amdgpu). */
		(void)snprintf(path, sizeof(path), "%s/mem_info_vram_used", device);
		error = monitor_read_number(path, &value);
		if (error == 0) {
			/* Of how much. */
			(void)snprintf(path, sizeof(path), "%s/mem_info_vram_total", device);
			error = monitor_read_number(path, &total);
			if (error == 0 &&
			    value >= 0 &&
			    total > 0) {
				gpu->memory_used = (uint64_t)value;
				gpu->memory_total = (uint64_t)total;
				sample->valid |= KL_MONITOR_HAVE_GPU_MEMORY;
			}
		}

		/* The frequency now (i915, on the card's directory). */
		(void)snprintf(path, sizeof(path), "%s/gt_cur_freq_mhz", monitor->gpu_path[index]);
		error = monitor_read_number(path, &value);
		if (error == 0 && value > 0) {
			gpu->cur_mhz = (unsigned)value;
			sample->valid |= KL_MONITOR_HAVE_GPU_FREQ;
		}

		/* The highest it may run at. */
		(void)snprintf(path, sizeof(path), "%s/gt_max_freq_mhz", monitor->gpu_path[index]);
		error = monitor_read_number(path, &value);
		if (error == 0 && value > 0)
			gpu->max_mhz = (unsigned)value;

		/* The temperature (millidegrees) of its hwmon. */
		error = monitor_hwmon_number(device, "temp1_input", &value);
		if (error == 0) {
			gpu->milli_celsius = (int)value;
			sample->valid |= KL_MONITOR_HAVE_TEMPERATURE;
		}

		/* The power (microwatts). */
		error = monitor_hwmon_number(device, "power1_average", &value);
		if (error == 0 && value >= 0) {
			gpu->milli_watts = (unsigned)(value / 1000);
			sample->valid |= KL_MONITOR_HAVE_POWER;
		}
	}

	/* The cards. */
	sample->gpu_count = monitor->gpu_count;
}

/* Reads one decimal number from a file; returns 0 or an errno value. */
static int
monitor_read_number(
	const char *path,
	long long *value)
{
	FILE *file;
	int fields;

	/* The file. */
	file = fopen(path, "r");
	if (file == NULL)
		return errno;

	/* Its number. */
	fields = fscanf(file, "%lld", value);
	(void)fclose(file);
	if (fields != 1)
		return EINVAL;

	/* Succeeded: the number. */
	return 0;
}

/* Reads a number from the first hwmon under a device's directory that has the file; returns 0 or an errno value. */
static int
monitor_hwmon_number(
	const char *directory,
	const char *file,
	long long *value)
{
	char path[PATH_MAX];
	struct dirent *entry;
	DIR *hwmon;
	int error;
	int differs;

	/* The device's hwmon directory. */
	(void)snprintf(path, sizeof(path), "%s/hwmon", directory);
	hwmon = opendir(path);
	if (hwmon == NULL)
		return ENOENT;

	/* Each hwmonN, until one has the file. */
	error = ENOENT;
	for (;;) {
		entry = readdir(hwmon);
		if (entry == NULL)
			break;
		differs = strncmp(entry->d_name, "hwmon", 5);
		if (differs != 0)
			continue;

		/* Its file. */
		(void)snprintf(path, sizeof(path), "%s/hwmon/%s/%s", directory, entry->d_name, file);
		error = monitor_read_number(path, value);
		if (error == 0)
			break;
	}

	/* The directory goes. */
	(void)closedir(hwmon);

	/* Reports a file no hwmon had. */
	if (error != 0)
		return error;

	/* Succeeded: the number. */
	return 0;
}

/* Reads the CPU package's temperature: the x86_pkg_temp thermal zone, else coretemp's or k10temp's hwmon. */
static void
monitor_cpu_temperature(
	struct kl_backend_monitor_sample *sample)
{
	char path[PATH_MAX];
	char type[64];
	struct dirent *entry;
	long long value;
	FILE *file;
	DIR *directory;
	char *read;
	int error;
	int differs;

	/* The thermal zones. */
	directory = opendir("/sys/class/thermal");
	if (directory == NULL)
		return;

	/* Each zone, until the package's. */
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		differs = strncmp(entry->d_name, "thermal_zone", 12);
		if (differs != 0)
			continue;

		/* Its type. */
		(void)snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", entry->d_name);
		file = fopen(path, "r");
		if (file == NULL)
			continue;
		read = fgets(type, sizeof(type), file);
		(void)fclose(file);
		if (read == NULL)
			continue;
		differs = strncmp(type, "x86_pkg_temp", 12);
		if (differs != 0)
			continue;

		/* Its temperature, in millidegrees. */
		(void)snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", entry->d_name);
		error = monitor_read_number(path, &value);
		if (error == 0) {
			sample->cpu_milli_celsius = (int)value;
			sample->valid |= KL_MONITOR_HAVE_TEMPERATURE;
			break;
		}
	}

	/* The directory goes. */
	(void)closedir(directory);
}

/* A device's id from its name (FNV-1a), the same for the same name; never 0. */
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
