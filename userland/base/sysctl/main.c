/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD sysctl userland command.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <uapi/sysctl.h>
#include <uapi/io-stats.h>
#include <uapi/cache-memory.h>
#include <uapi/writeback.h>
#include <uapi/readahead.h>

#define NAME_MAX 64U

static int show_cputimes(void);
static int show_diskstats(void);
static int show_gputelemetry(void);
static int show_thermal(void);
static int fetch_value(const char *name, unsigned char **buffer, size_t *length);
static int show_writeback(void);
static int show_readahead(void);
static int set_writeback(const char *value);
static int show_all(void);
static int show_name(const char *name);
static int set_name(const char *argument, const char *equal);

/*
 * Runs the sysctl command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	const char *equal;
	int error;

	/* Handles the selected command-line operation. */
	if (argc == 2 && strcmp(argv[1], "-a") == 0) {
		/* Obtains the show all result. */
		function_result = show_all();

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (argc != 2) {
		fprintf(stderr, "usage: sysctl name[=value] | sysctl -a\n");

		/* Reports operation failure. */
		return 2;
	}
	equal = strchr(argv[1], '=');

	/* Handles the equal availability. */
	if (equal == NULL) {
		/* Validates the command-line arguments. */
		if (show_name(argv[1]) == 0)
			return 0;
		error = errno;
	} else {
		error = set_name(argv[1], equal);
		if (error == 0)
			return 0;
	}
	fprintf(stderr, "sysctl: %s: %s\n", argv[1], strerror(error));

	/* Reports operation failure. */
	return 1;
}

/* Supports the show all operation. */
static int
show_all(
	void)
{
	static const char *const names[] = {
	    "hw.cputimes",
	    "hw.diskstats",
	    "hw.gputelemetry",
	    "hw.thermal",
	    "kern.boot.firmware_partition",
	    "kern.boot.config_partition",
	    "kern.boot.config_matches",
	    "kern.boot.root_image",
	    "kern.boot.login",
	    "vfs.bufcache.max_bytes",
	    "vfs.bufcache.current_bytes",
	    "vfs.bufcache.dirty_bytes",
	    "vfs.bufcache.stats",
	    "vfs.cache_memory.stats",
	    "vfs.cache_memory.target_bytes",
	    "vfs.writeback.stats",
	    "vfs.readahead.stats",
	};
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		/* Handles a failed show name operation. */
		if (show_name(names[i]) != 0) {
			fprintf(stderr, "sysctl: %s: %s\n", names[i],
				strerror(errno));

			/* Reports operation failure. */
			return 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the show name operation. */
static int
show_name(
	const char *name)
{
	size_t length_local;
	size_t length_local1;
	struct bufcache_stats stats;
	struct io_stats io;
	struct cache_memory_stats cache;
	static const char *const categories[] = {
	    "file_data", "file_metadata", "buffer_data", "buffer_metadata",
	    "io_pool", "dma", "worker"
	};
	struct memory_stats memory;
	unsigned event;
	uint64_t value;
	struct root_image_info root_image;
	size_t root_image_size;

	/* A single bounded record describes one live root/lower/loop observation. */
	if (strcmp(name, "kern.boot.root_image") == 0) {
		root_image_size = sizeof(root_image);
		if (sysctlbyname(name, &root_image, &root_image_size, NULL, 0) != 0)
			return -1;
		if (root_image_size != sizeof(root_image) || root_image.version != ROOT_IMAGE_VERSION) {
			errno = EIO;
			return -1;
		}
		printf("%s: %u:%u:%llu:%llu:%llu:%llu\n", name,
		    root_image.version, root_image.flags,
		    (unsigned long long)root_image.loop_device,
		    (unsigned long long)root_image.backing_device,
		    (unsigned long long)root_image.backing_inode,
		    (unsigned long long)root_image.backing_bytes);
		return 0;
	}

	/* Boot selectors are strings; the match count retains numeric
	 * rendering. */
	if (strcmp(name, "kern.boot.firmware_partition") == 0 ||
	    strcmp(name, "kern.boot.config_partition") == 0 ||
	    strcmp(name, "kern.boot.login") == 0) {
		char selector[64];
		size_t size = sizeof(selector);

		if (sysctlbyname(name, selector, &size, NULL, 0) != 0)
			return -1;
		if (size == 0 || size > sizeof(selector) ||
		    selector[size - 1] != '\0') {
			errno = EIO;
			return -1;
		}
		printf("%s: %s\n", name, selector);
		return 0;
	}

	/* Each CPU's ticks by what it was doing, a line a CPU. */
	if (strcmp(name, "hw.cputimes") == 0)
		return show_cputimes();

	/* Each physical disk's work, a line a disk. */
	if (strcmp(name, "hw.diskstats") == 0)
		return show_diskstats();

	/* Each GPU's work as its driver keeps it, a line a GPU. */
	if (strcmp(name, "hw.gputelemetry") == 0)
		return show_gputelemetry();

	/* Each temperature sensor, a line a sensor (ws134-p009). */
	if (strcmp(name, "hw.thermal") == 0)
		return show_thermal();

	/* Formats speculative observations separately from ordinary demand I/O.
	 */
	if (strcmp(name, "vfs.readahead.stats") == 0)
		return show_readahead();

	/* Formats the versioned mount policy report. */
	if (strcmp(name, "vfs.writeback.stats") == 0)
		return show_writeback();

	/* Reports shared physical ownership separately from free RAM and soft policy. */
	if (strcmp(name, "vfs.cache_memory.stats") == 0) {
		length_local = sizeof(cache);
		if (sysctlbyname(name, &cache, &length_local, NULL, 0) != 0)
			return -1;
		if (length_local != sizeof(cache) || cache.version != CACHE_MEMORY_VERSION ||
		    cache.count != CACHE_MEMORY_KINDS) {
			errno = EINVAL;
			return -1;
		}
		printf("%s: managed=%llu free=%llu reserve=%llu target=%llu "
		    "resident=%llu pending=%llu reclaimed=%llu refusals=%llu resizing=%u\n",
		    name, (unsigned long long)cache.managed_bytes,
		    (unsigned long long)cache.free_bytes,
		    (unsigned long long)cache.reserve_bytes,
		    (unsigned long long)cache.target_bytes,
		    (unsigned long long)cache.resident_bytes,
		    (unsigned long long)cache.pending_bytes,
		    (unsigned long long)cache.reclaimed_bytes,
		    (unsigned long long)cache.refusals, cache.resizing);
		for (event = 0; event < CACHE_MEMORY_KINDS; event++) {
			printf("%s: %s resident=%llu pending=%llu\n", name, categories[event],
			    (unsigned long long)cache.usage[event].resident_bytes,
			    (unsigned long long)cache.usage[event].pending_bytes);
		}
		return 0;
	}

	/* Reports versioned observations without treating them as scalars. */
	if (strcmp(name, "vfs.io.stats") == 0) {
		length_local = sizeof(io);
		if (sysctlbyname(name, &io, &length_local, NULL, 0) != 0)
			return -1;
		if (length_local != sizeof(io) || io.version != IO_STATS_VERSION ||
		    io.count != IO_STAT_COUNT) {
			errno = EINVAL;
			return -1;
		}
		for (event = 0; event < io.count; event++)
			printf("%s: event=%u calls=%llu bytes=%llu\n", name, event,
			    (unsigned long long)io.events[event].calls,
			    (unsigned long long)io.events[event].bytes);
		return 0;
	}
	if (strcmp(name, "hw.memory.stats") == 0) {
		length_local = sizeof(memory);
		if (sysctlbyname(name, &memory, &length_local, NULL, 0) != 0)
			return -1;
		if (length_local != sizeof(memory) || memory.version != MEMORY_STATS_VERSION) {
			errno = EINVAL;
			return -1;
		}
		printf("%s: ranges_valid=%u ranges=%llu usable=%llu highest_end=%llu "
		    "usable_highest_end=%llu mapped=%llu initial=%llu managed=%llu "
		    "reserved=%llu allocated=%llu free=%llu\n", name,
		    memory.boot_ranges_valid,
		    (unsigned long long)memory.boot_range_count,
		    (unsigned long long)memory.boot_usable_bytes,
		    (unsigned long long)memory.boot_highest_end,
		    (unsigned long long)memory.boot_usable_highest_end,
		    (unsigned long long)memory.direct_mapped_bytes,
		    (unsigned long long)memory.allocator_initial_bytes,
		    (unsigned long long)memory.physical_managed_bytes,
		    (unsigned long long)memory.physical_reserved_bytes,
		    (unsigned long long)memory.physical_allocated_bytes,
		    (unsigned long long)memory.physical_free_bytes);
		printf("%s: source=%u boot_reclaim=%llu metadata=%llu scan_words=%llu max_extent_scan_words=%llu max_irqoff_cycles=%llu\n",
		    name, memory.boot_memory_source, (unsigned long long)memory.boot_reclaim_bytes,
		    (unsigned long long)memory.allocator_metadata_bytes,
		    (unsigned long long)memory.allocator_scan_words,
		    (unsigned long long)memory.allocator_max_extent_scan_words,
		    (unsigned long long)memory.allocator_max_irqoff_cycles);
		return 0;
	}

	/* Selects the matching value. */
	if (strcmp(name, "vfs.bufcache.stats") == 0) {
		length_local = sizeof(stats);

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname(name, &stats, &length_local, NULL, 0) != 0)
			return -1;
		printf("%s: buffers=%llu hits=%llu misses=%llu read_bios=%llu "
		       "write_bios=%llu evictions=%llu waits=%llu "
		       "writeback_errors=%llu capacity_failures=%llu physical_failures=%llu\n",
		       name, (unsigned long long)stats.buffers,
		       (unsigned long long)stats.hits,
		       (unsigned long long)stats.misses,
		       (unsigned long long)stats.read_bios,
		       (unsigned long long)stats.write_bios,
		       (unsigned long long)stats.evictions,
		       (unsigned long long)stats.waits,
		       (unsigned long long)stats.writeback_errors,
		       (unsigned long long)stats.capacity_failures,
		       (unsigned long long)stats.physical_failures);

		/* Reports successful completion. */
		return 0;
	} else {
		length_local1 = sizeof(value);

		/* Handles a failed sysctlbyname operation. */
		if (sysctlbyname(name, &value, &length_local1, NULL, 0) != 0)
			return -1;
		printf("%s: %llu\n", name, (unsigned long long)value);

		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the set name operation. */
static int
set_name(
	const char *argument,
	const char *equal)
{
	int function_result;
	char name[NAME_MAX];
	char *end;
	unsigned long long parsed;
	uint64_t value;
	size_t name_length;

	name_length = (size_t)(equal - argument);

	/* Handles the name length condition. */
	if (name_length == 0 || name_length >= sizeof(name) || equal[1] == '\0')
		return EINVAL;
	memcpy(name, argument, name_length);
	name[name_length] = '\0';
	if (strcmp(name, "vfs.writeback.control") == 0)
		return set_writeback(equal + 1);
	errno = 0;
	parsed = strtoull(equal + 1, &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || *end != '\0')
		return EINVAL;
	value = (uint64_t)parsed;

	/* Validates the current value. */
	if ((unsigned long long)value != parsed)
		return ERANGE;

	/* Handles a failed sysctlbyname operation. */
	if (sysctlbyname(name, NULL, NULL, &value, sizeof(value)) != 0)
		return errno;

	/* Computes the function result. */
	function_result = show_name(name) == 0 ? 0 : errno;

	/* Returns the computed result. */
	return function_result;
}

/* Displays shared physical budgets beside each effective mount policy. */
static int
show_writeback(void)
{
	struct writeback_report *report;
	struct writeback_report_header *header;
	struct writeback_mount_info *entry;
	size_t size;
	unsigned index;
	int error;

	/* Keeps the bounded report off the user stack. */
	report = malloc(sizeof(*report));
	if (report == NULL)
		return -1;
	size = sizeof(*report);
	if (sysctlbyname("vfs.writeback.stats", report, &size, NULL, 0) != 0) {
		error = errno;
		free(report);
		errno = error;
		return -1;
	}
	header = &report->header;
	if (size != sizeof(*report) || header->version != WRITEBACK_REPORT_VERSION ||
	    header->count > WRITEBACK_REPORT_MOUNTS) {
		free(report);
		errno = EINVAL;
		return -1;
	}

	/* Reports global admission and reserved worker resources. */
	printf("vfs.writeback.stats: mounts=%u workers=%u busy=%u dirty=%llu reserved=%llu "
	    "tickets=%llu high=%llu low=%llu device_high=%llu memory=%llu passes=%llu errors=%llu last_error=%d\n",
	    header->count, header->workers, header->busy,
	    (unsigned long long)header->dirty, (unsigned long long)header->reserved,
	    (unsigned long long)header->tickets, (unsigned long long)header->high,
	    (unsigned long long)header->low, (unsigned long long)header->device_high,
	    (unsigned long long)header->memory_bytes, (unsigned long long)header->passes,
	    (unsigned long long)header->errors, header->last_error);
	for (index = 0; index < header->count; index++) {
		entry = &report->mounts[index];
		entry->path[sizeof(entry->path) - 1U] = '\0';
		entry->device[sizeof(entry->device) - 1U] = '\0';
		printf("vfs.writeback.stats: path=%s state=%s device=%s device_dirty=%llu "
		    "device_reserved=%llu device_tickets=%llu\n", entry->path,
		    entry->state == WRITEBACK_STATE_LIVE ? "on" : "paused", entry->device,
		    (unsigned long long)entry->device_dirty,
		    (unsigned long long)entry->device_reserved,
		    (unsigned long long)entry->device_tickets);
	}
	free(report);
	return 0;
}

/* Applies an exact mount path and explicit on/off action. */
static int
set_writeback(const char *value)
{
	struct writeback_control request;
	const char *action;
	size_t length;
	int error;

	/* Parses the final delimiter without truncating long mount names. */
	action = strrchr(value, ':');
	if (action == NULL || value[0] != '/')
		return EINVAL;
	length = (size_t)(action - value);
	if (length == 0 || length >= sizeof(request.path))
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.version = WRITEBACK_REPORT_VERSION;
	if (strcmp(action + 1, "on") == 0)
		request.enabled = 1;
	else if (strcmp(action + 1, "off") != 0)
		return EINVAL;
	memcpy(request.path, value, length);
	if (sysctlbyname("vfs.writeback.control", NULL, NULL, &request, sizeof(request)) != 0)
		return errno;
	error = show_writeback();
	return error == 0 ? 0 : errno;
}

/* Displays confirmed-use bounds without labeling uncredited data as measured waste. */
static int
show_readahead(
	void)
{
	struct readahead_report report;
	size_t size;

	/* Checks the ABI before interpreting cumulative observations. */
	size = sizeof(report);
	if (sysctlbyname("vfs.readahead.stats", &report, &size, NULL, 0) != 0)
		return -1;
	if (size != sizeof(report) || report.version != READAHEAD_REPORT_VERSION) {
		errno = EINVAL;
		return -1;
	}
	printf("vfs.readahead.stats: requested=%llu started=%llu published=%llu "
	    "confirmed_useful=%llu retired_uncredited=%llu discarded_fill=%llu "
	    "errors=%llu queue_refusals=%llu memory=%llu jobs=%u running=%u demand=%u\n",
	    (unsigned long long)report.requested_bytes,
	    (unsigned long long)report.started_bytes,
	    (unsigned long long)report.published_bytes,
	    (unsigned long long)report.confirmed_useful_bytes,
	    (unsigned long long)report.retired_uncredited_bytes,
	    (unsigned long long)report.discarded_fill_bytes,
	    (unsigned long long)report.errors,
	    (unsigned long long)report.queue_refusals,
	    (unsigned long long)report.memory_bytes,
	    report.jobs, report.running, report.demand);
	return 0;
}

/*
 * Prints hw.cputimes (ws134-p005): the ticks a second and the CPUs, then
 * a line a CPU with its user, system, idle and other ticks since boot.
 * Returns 0, or -1 with errno set.
 */
static int
show_cputimes(
	void)
{
	const struct cpu_times_header *header;
	const struct cpu_times_entry *entry;
	unsigned char *buffer;
	size_t length;
	uint32_t cpu;
	int status;
	int valid;

	/* The value, whole. */
	status = fetch_value("hw.cputimes", &buffer, &length);
	if (status != 0)
		return -1;

	/* A value of a layout this command does not know. */
	header = (const struct cpu_times_header *)(void *)buffer;
	valid = 1;
	if (length < sizeof(*header)) {
		valid = 0;
	} else if (header->version != CPU_TIMES_VERSION) {
		valid = 0;
	} else if (header->element_size != sizeof(*entry)) {
		valid = 0;
	} else if (header->struct_size + (size_t)header->count * header->element_size != length) {
		valid = 0;
	}

	/* Refuses what it cannot read. */
	if (!valid) {
		free(buffer);
		errno = EINVAL;
		return -1;
	}

	/* The clock's rate and the CPUs. */
	printf("hw.cputimes: hz=%u cpus=%u\n", header->hz, header->count);

	/* A line a CPU. */
	for (cpu = 0; cpu < header->count; cpu++) {
		entry = (const struct cpu_times_entry *)(void *)(buffer + header->struct_size + (size_t)cpu * header->element_size);
		printf("hw.cputimes: cpu=%u user=%llu system=%llu idle=%llu other=%llu\n", cpu,
		       (unsigned long long)entry->user, (unsigned long long)entry->system,
		       (unsigned long long)entry->idle, (unsigned long long)entry->other);
	}

	/* The value is not needed any more. */
	free(buffer);

	/* Succeeded: every CPU is printed. */
	return 0;
}

/*
 * Prints hw.diskstats (ws134-p006): the set's generation and the disks,
 * then a line a disk with its kind, flags, id, generation, work and
 * requests outstanding.  Returns 0, or -1 with errno set.
 */
static int
show_diskstats(
	void)
{
	const struct disk_stats_header *header;
	const struct disk_stats_entry *entry;
	unsigned char *buffer;
	size_t length;
	uint32_t disk;
	int status;
	int valid;

	/* The value, whole. */
	status = fetch_value("hw.diskstats", &buffer, &length);
	if (status != 0)
		return -1;

	/* A value of a layout this command does not know. */
	header = (const struct disk_stats_header *)(void *)buffer;
	valid = 1;
	if (length < sizeof(*header)) {
		valid = 0;
	} else if (header->version != DISK_STATS_VERSION) {
		valid = 0;
	} else if (header->element_size != sizeof(*entry)) {
		valid = 0;
	} else if (header->struct_size + (size_t)header->count * header->element_size != length) {
		valid = 0;
	}

	/* Refuses what it cannot read. */
	if (!valid) {
		free(buffer);
		errno = EINVAL;
		return -1;
	}

	/* The set of disks. */
	printf("hw.diskstats: generation=%u disks=%u\n", header->generation, header->count);

	/* A line a disk. */
	for (disk = 0; disk < header->count; disk++) {
		entry = (const struct disk_stats_entry *)(void *)(buffer + header->struct_size + (size_t)disk * header->element_size);
		printf("hw.diskstats: name=%.32s kind=%u flags=0x%x id=%llu generation=%llu read_ops=%llu write_ops=%llu "
		       "read_bytes=%llu write_bytes=%llu read_ns=%llu write_ns=%llu busy_ns=%llu inflight=%u\n",
		       entry->name, entry->kind, entry->flags, (unsigned long long)entry->id, (unsigned long long)entry->generation,
		       (unsigned long long)entry->read_ops, (unsigned long long)entry->write_ops,
		       (unsigned long long)entry->read_bytes, (unsigned long long)entry->write_bytes,
		       (unsigned long long)entry->read_ns, (unsigned long long)entry->write_ns,
		       (unsigned long long)entry->busy_ns, entry->inflight);
	}

	/* The value is not needed any more. */
	free(buffer);

	/* Succeeded: every disk is printed. */
	return 0;
}

/*
 * Prints hw.gputelemetry (ws134-p007): the GPUs, then a line a GPU with its
 * driver, the fields it filled (valid), its busy time at the driver's
 * clock, its frequencies and its objects' memory.  Returns 0, or -1 with
 * errno set.
 */
static int
show_gputelemetry(
	void)
{
	const struct gpu_telemetry_header *header;
	const struct gpu_telemetry_entry *entry;
	unsigned char *buffer;
	size_t length;
	uint32_t gpu;
	int status;
	int valid;

	/* The value, whole. */
	status = fetch_value("hw.gputelemetry", &buffer, &length);
	if (status != 0)
		return -1;

	/* A value of a layout this command does not know. */
	header = (const struct gpu_telemetry_header *)(void *)buffer;
	valid = 1;
	if (length < sizeof(*header)) {
		valid = 0;
	} else if (header->version != GPU_TELEMETRY_VERSION) {
		valid = 0;
	} else if (header->element_size != sizeof(*entry)) {
		valid = 0;
	} else if (header->struct_size + (size_t)header->count * header->element_size != length) {
		valid = 0;
	}

	/* Refuses what it cannot read. */
	if (!valid) {
		free(buffer);
		errno = EINVAL;
		return -1;
	}

	/* The GPUs. */
	printf("hw.gputelemetry: gpus=%u\n", header->count);

	/* A line a GPU. */
	for (gpu = 0; gpu < header->count; gpu++) {
		entry = (const struct gpu_telemetry_entry *)(void *)(buffer + header->struct_size + (size_t)gpu * header->element_size);
		printf("hw.gputelemetry: gpu=%u driver=%.16s valid=0x%x time_ns=%llu busy_ns=%llu cur_mhz=%u req_mhz=%u min_mhz=%u max_mhz=%u "
		       "objects_bytes=%llu objects_limit=%llu\n",
		       gpu, entry->driver, entry->valid, (unsigned long long)entry->time_ns, (unsigned long long)entry->busy_ns,
		       entry->cur_mhz, entry->req_mhz, entry->min_mhz, entry->max_mhz,
		       (unsigned long long)entry->objects_bytes, (unsigned long long)entry->objects_limit);
	}

	/* The value is not needed any more. */
	free(buffer);

	/* Succeeded: every GPU is printed. */
	return 0;
}

/*
 * Prints hw.thermal (ws134-p009): the sensors, then a line a sensor with
 * its ACPI path, its kind, whether it is the processor's, and the values it
 * has (in thousandths of a degree Celsius) with the time it was read.
 * Returns 0, or -1 with errno set.
 */
static int
show_thermal(
	void)
{
	const struct thermal_header *header;
	const struct thermal_entry *entry;
	unsigned char *buffer;
	const char *kind;
	size_t length;
	uint32_t sensor;
	int status;
	int valid;

	/* The value, whole. */
	status = fetch_value("hw.thermal", &buffer, &length);
	if (status != 0)
		return -1;

	/* A value of a layout this command does not know. */
	header = (const struct thermal_header *)(void *)buffer;
	valid = 1;
	if (length < sizeof(*header)) {
		valid = 0;
	} else if (header->version != THERMAL_VERSION) {
		valid = 0;
	} else if (header->element_size != sizeof(*entry)) {
		valid = 0;
	} else if (header->struct_size + (size_t)header->count * header->element_size != length) {
		valid = 0;
	}

	/* Refuses what it cannot read. */
	if (!valid) {
		free(buffer);
		errno = EINVAL;
		return -1;
	}

	/* The sensors. */
	printf("hw.thermal: sensors=%u\n", header->count);

	/* A line a sensor, with the values it has. */
	for (sensor = 0; sensor < header->count; sensor++) {
		entry = (const struct thermal_entry *)(void *)(buffer + header->struct_size + (size_t)sensor * header->element_size);
		kind = "device";
		if (entry->kind == THERMAL_KIND_ZONE)
			kind = "zone";
		printf("hw.thermal: sensor=%u name=%.32s kind=%s cpu=%u", sensor, entry->name, kind,
		       (entry->flags & THERMAL_FLAG_CPU) != 0U);
		if ((entry->valid & THERMAL_HAVE_TEMPERATURE) != 0U)
			printf(" temperature_mc=%d time_ns=%llu", entry->milli_celsius, (unsigned long long)entry->time_ns);
		if ((entry->valid & THERMAL_HAVE_PASSIVE) != 0U)
			printf(" passive_mc=%d", entry->passive_milli_celsius);
		if ((entry->valid & THERMAL_HAVE_CRITICAL) != 0U)
			printf(" critical_mc=%d", entry->critical_milli_celsius);
		printf("\n");
	}

	/* The value is not needed any more. */
	free(buffer);

	/* Succeeded: every sensor is printed. */
	return 0;
}

/*
 * Reads a sysctl value whose length the kernel decides (a header and its
 * entries) into a new buffer: the length first, then the value, again
 * with the new length when it grew in between (a CPU or a disk came).
 * Returns 0 with *buffer to free, or -1 with errno set.
 */
static int
fetch_value(
	const char *name,
	unsigned char **buffer,
	size_t *length)
{
	unsigned char *value;
	size_t size;
	int status;
	int attempt;

	/* A few tries: the value may grow between the length and the read. */
	value = NULL;
	size = 0;
	status = -1;
	for (attempt = 0; attempt < 4; attempt++) {
		/* The length the kernel needs now. */
		size = 0;
		status = sysctlbyname(name, NULL, &size, NULL, 0);
		if (status != 0)
			return -1;

		/* A buffer of that length. */
		value = malloc(size);
		if (value == NULL)
			return -1;

		/* The value; one that grew is asked for again. */
		status = sysctlbyname(name, value, &size, NULL, 0);
		if (status == 0)
			break;

		/* Any failure but a grown value ends the tries. */
		free(value);
		value = NULL;
		if (errno != ENOMEM)
			return -1;
	}

	/* Still growing after every try. */
	if (status != 0) {
		errno = EAGAIN;
		return -1;
	}

	/* Succeeded: the value, the caller's to free. */
	*buffer = value;
	*length = size;
	return 0;
}
