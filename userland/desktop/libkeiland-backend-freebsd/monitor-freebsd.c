/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The machine's monitor on FreeBSD (WS134 p011, plan/ws134/design.md
 * section 1.3): the counters the kernel's sysctls keep, as the System
 * Monitor shows them.
 *
 *   the CPUs     kern.cp_times: user and nice are user, sys and intr are
 *                system, idle is idle; the ticks are kern.clockrate's
 *                stathz
 *   the memory   vm.stats.vm's page counts of hw.pagesize: the total, the
 *                free, the caches (inactive and laundry, and vfs.bufspace),
 *                what can be dropped at once (inactive); the swap from
 *                vm.swap_info
 *   the links    the network area's interfaces, loopback aside
 *   the disks    kern.devstat.all's direct-access devices (no pass)
 *   temperature  dev.cpu.0.temperature (coretemp, amdtemp), else the
 *                first ACPI thermal zone
 *
 * The GPUs keep nothing a program can read on FreeBSD (design.md section
 * 1.1); their bits stay out of valid.  Nothing here touches struct
 * kl_backend: the compositor samples on a thread of its own.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <sys/types.h>
#include <sys/devicestat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <vm/vm_param.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The most interfaces the network area is asked for (the loopback among them). */
#define MONITOR_INTERFACES	32U

/* The states kern.cp_times keeps for each CPU, and their places. */
#define MONITOR_CP_STATES	5U
#define MONITOR_CP_USER		0U
#define MONITOR_CP_NICE		1U
#define MONITOR_CP_SYS		2U
#define MONITOR_CP_INTR		3U
#define MONITOR_CP_IDLE		4U

/* The most swap devices read. */
#define MONITOR_SWAP_DEVICES	16

/*
 * A monitor: the buffer the variable sysctl values are read into, grown
 * as they need.
 */
struct kl_backend_monitor {
	unsigned char *buffer;
	size_t buffer_size;
};

static uint64_t monitor_now_ns(void);
static int monitor_fetch(struct kl_backend_monitor *monitor, const char *name, size_t *length);
static int monitor_cpus(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample);
static unsigned monitor_count(const char *name);
static int monitor_memory(struct kl_backend_monitor_sample *sample);
static void monitor_swap(struct kl_backend_monitor_sample *sample, uint64_t page_size);
static unsigned monitor_links(struct kl_backend_monitor_link *links, struct kl_backend_monitor_link_info *infos);
static uint64_t monitor_bintime_ns(const struct bintime *time);
static unsigned monitor_disk_kind(const char *name);
static int monitor_disks(struct kl_backend_monitor *monitor, struct kl_backend_monitor_sample *sample, struct kl_backend_monitor_info *info);
static void monitor_cpu_temperature(struct kl_backend_monitor_sample *sample);
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

	/* Succeeded: the monitor. */
	return monitor;
}

/*
 * Reads the info: the CPUs, the machine's name, the disks and the links
 * (no GPU).  Returns 0 or an errno value.
 */
int
kl_backend_monitor_info(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_info *info)
{
	struct kl_backend_monitor_sample *sample;
	struct kl_backend_monitor_link links[KL_MONITOR_LINK_MAX];
	uint64_t mixed;
	unsigned index;
	int cpus;
	size_t length;
	int status;

	/* Nothing known yet. */
	memset(info, 0, sizeof(*info));

	/* The CPUs. */
	length = sizeof(cpus);
	status = sysctlbyname("hw.ncpu", &cpus, &length, NULL, 0);
	if (status == 0 && cpus > 0)
		info->cpu_count = (unsigned)cpus;

	/* The machine's name. */
	status = gethostname(info->host, sizeof(info->host));
	if (status != 0)
		info->host[0] = '\0';
	info->host[sizeof(info->host) - 1U] = '\0';

	/* The disks, through a sample's read. */
	sample = calloc(1, sizeof(*sample));
	if (sample == NULL)
		return ENOMEM;
	(void)monitor_disks(monitor, sample, info);
	free(sample);

	/* The links. */
	info->link_count = monitor_links(links, info->link);

	/* The set's generation: the devices' ids mixed, so that it changes whenever one comes or goes. */
	mixed = info->generation;
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
	int status;

	/* Nothing read yet; the time first. */
	memset(sample, 0, sizeof(*sample));
	sample->time_ns = monitor_now_ns();

	/* The CPUs. */
	status = monitor_cpus(monitor, sample);
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
	status = monitor_disks(monitor, sample, info);
	if (status == 0)
		sample->valid |= KL_MONITOR_HAVE_DISKS;
	free(info);

	/* The CPU's temperature. */
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
	/* Nothing to close. */
	if (monitor == NULL)
		return;

	/* The buffer and the monitor. */
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
 * Reads a sysctl value of the kernel's length into the monitor's buffer,
 * growing it as needed.  Returns 0 with the value's length, or an errno
 * value.
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
		/* The length the kernel needs now, with room for a little growth. */
		size = 0;
		status = sysctlbyname(name, NULL, &size, NULL, 0);
		if (status != 0)
			return errno;
		size += size / 8U + 64U;

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

/* Reads each CPU's ticks from kern.cp_times; returns 0 or an errno value. */
static int
monitor_cpus(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample)
{
	struct clockinfo clock;
	const long *states;
	size_t length;
	size_t count;
	size_t cpu;
	int error;
	int status;

	/* The value: five longs a CPU. */
	error = monitor_fetch(monitor, "kern.cp_times", &length);
	if (error != 0)
		return error;

	/* As many CPUs as fit. */
	count = length / (MONITOR_CP_STATES * sizeof(long));
	if (count == 0U)
		return ENOENT;
	if (count > KL_MONITOR_CPU_MAX)
		count = KL_MONITOR_CPU_MAX;

	/* Each CPU's ticks: user + nice, sys + intr, idle. */
	states = (const long *)(const void *)monitor->buffer;
	for (cpu = 0; cpu < count; cpu++) {
		sample->cpu[cpu].user = (uint64_t)states[cpu * MONITOR_CP_STATES + MONITOR_CP_USER] +
		    (uint64_t)states[cpu * MONITOR_CP_STATES + MONITOR_CP_NICE];
		sample->cpu[cpu].system = (uint64_t)states[cpu * MONITOR_CP_STATES + MONITOR_CP_SYS] +
		    (uint64_t)states[cpu * MONITOR_CP_STATES + MONITOR_CP_INTR];
		sample->cpu[cpu].idle = (uint64_t)states[cpu * MONITOR_CP_STATES + MONITOR_CP_IDLE];
		sample->cpu[cpu].other = 0;
	}

	/* The ticks' rate: the statistics clock's (the clock's when it has none). */
	memset(&clock, 0, sizeof(clock));
	length = sizeof(clock);
	status = sysctlbyname("kern.clockrate", &clock, &length, NULL, 0);
	sample->cpu_hz = 128;
	if (status == 0 && clock.stathz > 0)
		sample->cpu_hz = (uint64_t)clock.stathz;
	else if (status == 0 && clock.hz > 0)
		sample->cpu_hz = (uint64_t)clock.hz;

	/* The CPUs read. */
	sample->cpu_count = (unsigned)count;

	/* Succeeded: the CPUs. */
	return 0;
}

/* Reads one count of pages (an unsigned int sysctl); 0 when it cannot be read. */
static unsigned
monitor_count(
	const char *name)
{
	unsigned value;
	size_t length;
	int status;

	/* The count. */
	value = 0;
	length = sizeof(value);
	status = sysctlbyname(name, &value, &length, NULL, 0);
	if (status != 0)
		return 0;

	/* Succeeded: the count. */
	return value;
}

/* Reads the memory and the swap; returns 0 or an errno value. */
static int
monitor_memory(
	struct kl_backend_monitor_sample *sample)
{
	uint64_t page_size;
	unsigned pages;
	unsigned free_pages;
	unsigned inactive;
	unsigned laundry;
	long buffers;
	int size;
	size_t length;
	int status;

	/* The page's size. */
	length = sizeof(size);
	status = sysctlbyname("hw.pagesize", &size, &length, NULL, 0);
	if (status != 0 || size <= 0)
		return ENOENT;
	page_size = (uint64_t)size;

	/* The page counts. */
	pages = monitor_count("vm.stats.vm.v_page_count");
	free_pages = monitor_count("vm.stats.vm.v_free_count");
	inactive = monitor_count("vm.stats.vm.v_inactive_count");
	laundry = monitor_count("vm.stats.vm.v_laundry_count");
	if (pages == 0U)
		return ENOENT;

	/* The buffer cache's bytes. */
	buffers = 0;
	length = sizeof(buffers);
	status = sysctlbyname("vfs.bufspace", &buffers, &length, NULL, 0);
	if (status != 0 || buffers < 0)
		buffers = 0;

	/* In bytes. */
	sample->memory_total = (uint64_t)pages * page_size;
	sample->memory_free = (uint64_t)free_pages * page_size;
	sample->memory_cache = ((uint64_t)inactive + laundry) * page_size + (uint64_t)buffers;
	sample->memory_reclaimable = (uint64_t)inactive * page_size;

	/* The swap. */
	monitor_swap(sample, page_size);

	/* Succeeded: the memory. */
	return 0;
}

/* Adds up the swap devices of vm.swap_info (their blocks are pages). */
static void
monitor_swap(
	struct kl_backend_monitor_sample *sample,
	uint64_t page_size)
{
	struct xswdev device;
	int mib[CTL_MAXNAME];
	size_t mib_length;
	size_t length;
	int index;
	int status;

	/* The name's numbers; without them, no swap. */
	mib_length = CTL_MAXNAME - 1U;
	status = sysctlnametomib("vm.swap_info", mib, &mib_length);
	if (status != 0)
		return;

	/* Each device, until there are no more. */
	for (index = 0; index < MONITOR_SWAP_DEVICES; index++) {
		mib[mib_length] = index;
		length = sizeof(device);
		status = sysctl(mib, (unsigned)(mib_length + 1U), &device, &length, NULL, 0);
		if (status != 0)
			break;
		if (device.xsw_version != XSWDEV_VERSION)
			break;

		/* Its size and what is used. */
		sample->swap_total += (uint64_t)device.xsw_nblks * page_size;
		sample->swap_used += (uint64_t)device.xsw_used * page_size;
	}
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

/* A bintime in nanoseconds. */
static uint64_t
monitor_bintime_ns(
	const struct bintime *time)
{
	uint64_t fraction;

	/* The fraction's top 32 bits are enough for nanoseconds. */
	fraction = ((uint64_t)(time->frac >> 32) * 1000000000ULL) >> 32;

	/* Succeeded: the time. */
	return (uint64_t)time->sec * 1000000000ULL + fraction;
}

/* A disk's kind by its driver's name: nvd and nda are NVMe, ada is ATA, da is SCSI (USB sticks among them), mmcsd is an SD card. */
static unsigned
monitor_disk_kind(
	const char *name)
{
	static const struct {
		const char *name;
		unsigned kind;
	} kinds[] = {
		{ "nvd", KL_MONITOR_DISK_NVME },
		{ "nda", KL_MONITOR_DISK_NVME },
		{ "ada", KL_MONITOR_DISK_IDE },
		{ "da", KL_MONITOR_DISK_SCSI },
		{ "mmcsd", KL_MONITOR_DISK_SDMMC }
	};
	size_t index;
	int differs;

	/* The driver's name. */
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++) {
		differs = strcmp(name, kinds[index].name);
		if (differs == 0)
			return kinds[index].kind;
	}

	/* Succeeded: another kind. */
	return KL_MONITOR_DISK_OTHER;
}

/*
 * Reads the direct-access disks of kern.devstat.all into a sample and the
 * info (its generation is devstat's).  Returns 0 or an errno value.
 */
static int
monitor_disks(
	struct kl_backend_monitor *monitor,
	struct kl_backend_monitor_sample *sample,
	struct kl_backend_monitor_info *info)
{
	const struct devstat *stats;
	struct kl_backend_monitor_disk *disk;
	char name[32];
	size_t length;
	size_t devices;
	size_t index;
	unsigned count;
	long generation;
	int error;

	/* The value: devstat's generation, then a struct devstat a device. */
	error = monitor_fetch(monitor, "kern.devstat.all", &length);
	if (error != 0)
		return error;
	if (length < sizeof(long))
		return EINVAL;
	memcpy(&generation, monitor->buffer, sizeof(generation));
	info->generation = (uint64_t)generation;
	devices = (length - sizeof(long)) / sizeof(struct devstat);
	stats = (const struct devstat *)(const void *)(monitor->buffer + sizeof(long));

	/* Each direct-access device, as many as fit. */
	count = 0;
	for (index = 0; index < devices && count < KL_MONITOR_DISK_MAX; index++) {
		/* A pass device or another kind is no disk. */
		if ((stats[index].device_type & DEVSTAT_TYPE_MASK) != DEVSTAT_TYPE_DIRECT)
			continue;
		if ((stats[index].device_type & DEVSTAT_TYPE_PASS) != 0)
			continue;

		/* Its name: the driver's and the unit. */
		(void)snprintf(name, sizeof(name), "%s%d", stats[index].device_name, stats[index].unit_number);

		/* Its work. */
		disk = &sample->disk[count];
		disk->id = monitor_name_id(name);
		disk->read_ops = stats[index].operations[DEVSTAT_READ];
		disk->write_ops = stats[index].operations[DEVSTAT_WRITE];
		disk->read_bytes = stats[index].bytes[DEVSTAT_READ];
		disk->write_bytes = stats[index].bytes[DEVSTAT_WRITE];
		disk->read_ns = monitor_bintime_ns(&stats[index].duration[DEVSTAT_READ]);
		disk->write_ns = monitor_bintime_ns(&stats[index].duration[DEVSTAT_WRITE]);
		disk->busy_ns = monitor_bintime_ns(&stats[index].busy_time);

		/* Its info. */
		info->disk[count].id = disk->id;
		info->disk[count].generation = 0;
		(void)snprintf(info->disk[count].name, sizeof(info->disk[count].name), "%s", name);
		info->disk[count].kind = monitor_disk_kind(stats[index].device_name);
		count++;
	}

	/* The disks read. */
	sample->disk_count = count;
	info->disk_count = count;

	/* Succeeded: the disks. */
	return 0;
}

/* Reads the CPU's temperature: coretemp's or amdtemp's, else the first ACPI thermal zone's (both in tenths of a kelvin). */
static void
monitor_cpu_temperature(
	struct kl_backend_monitor_sample *sample)
{
	size_t length;
	int decikelvin;
	int status;

	/* The CPU's sensor. */
	length = sizeof(decikelvin);
	status = sysctlbyname("dev.cpu.0.temperature", &decikelvin, &length, NULL, 0);

	/* Else the first thermal zone. */
	if (status != 0) {
		length = sizeof(decikelvin);
		status = sysctlbyname("hw.acpi.thermal.tz0.temperature", &decikelvin, &length, NULL, 0);
	}

	/* Neither. */
	if (status != 0)
		return;

	/* In millidegrees Celsius. */
	sample->cpu_milli_celsius = decikelvin * 100 - 273150;
	sample->valid |= KL_MONITOR_HAVE_TEMPERATURE;
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
