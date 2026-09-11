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
	    "kern.boot.firmware_partition",
	    "kern.boot.config_partition",
	    "kern.boot.config_matches",
	    "kern.boot.root_image",
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
	    strcmp(name, "kern.boot.config_partition") == 0) {
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
