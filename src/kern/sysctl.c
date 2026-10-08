/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sysctl tree.
 *
 * The kernel exposes a fixed set of leaves: CPU counts, the message buffer,
 * the host name, and the buffer cache knobs.  The meta operations translate
 * between names and object identifiers and enumerate the leaves.
 */

#include "kern/sysctl.h"
#include "kern/boot.h"
#include "kern/vfs.h"
#include "kern/buf.h"
#include "kern/io-stats.h"
#include "kern/cache-memory.h"
#include "kern/writeback.h"
#include "kern/readahead.h"
#include "kern/mount.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/clock.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <uapi/sysctl.h>

#define SYSCTL_NAME_MAX 64U

struct sysctl_leaf {
	int oid[3];
	unsigned oidlen;
	const char *name;
};

static const struct sysctl_leaf leaves[] = {
	{{ CTL_HW, HW_NCPU, 0 }, 2, "hw.ncpu"},
	{{ CTL_HW, HW_NCPUONLINE, 0 }, 2, "hw.ncpuonline"},
	{{ CTL_HW, HW_MEMORY_STATS, 0 }, 2, "hw.memory.stats"},
	{{ CTL_HW, HW_GPU_ATTACHING, 0 }, 2, "hw.gpu.attaching"},
	{{ CTL_HW, HW_GPU_START, 0 }, 2, "hw.gpu.start"},
	{{ CTL_HW, HW_CPUTIMES, 0 }, 2, "hw.cputimes"},
	{{ CTL_HW, HW_DISKSTATS, 0 }, 2, "hw.diskstats"},
	{{ CTL_HW, HW_GPUTELEMETRY, 0 }, 2, "hw.gputelemetry"},
	{{ CTL_HW, HW_THERMAL, 0 }, 2, "hw.thermal"},
	{{ CTL_KERN, KERN_MSGBUF, 0 }, 2, "kern.msgbuf"},
	{{ CTL_KERN, KERN_MSGBUF_SIZE, 0 }, 2, "kern.msgbuf_size"},
	{{ CTL_KERN, KERN_MSGBUF_DROPPED, 0 }, 2, "kern.msgbuf_dropped"},
	{{ CTL_KERN, KERN_HOSTNAME, 0 }, 2, "kern.hostname"},
	{{ CTL_KERN, KERN_BOOT_FIRMWARE, 0 }, 2, "kern.boot.firmware_partition"},
	{{ CTL_KERN, KERN_BOOT_CONFIGURATION, 0 }, 2, "kern.boot.config_partition"},
	{{ CTL_KERN, KERN_BOOT_CONFIG_MATCHES, 0 }, 2, "kern.boot.config_matches"},
	{{ CTL_KERN, KERN_BOOT_ROOT_IMAGE, 0 }, 2, "kern.boot.root_image"},
	{{ CTL_KERN, KERN_BOOT_LOGIN, 0 }, 2, "kern.boot.login"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_MAX_BYTES }, 3,
	 "vfs.bufcache.max_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_CURRENT_BYTES }, 3,
	 "vfs.bufcache.current_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_DIRTY_BYTES }, 3,
	 "vfs.bufcache.dirty_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_STATS }, 3,
	 "vfs.bufcache.stats"},
	{{ CTL_VFS, VFS_IO, VFS_IO_STATS }, 3, "vfs.io.stats"},
	{{ CTL_VFS, VFS_CACHE_MEMORY, VFS_CACHE_MEMORY_STATS }, 3, "vfs.cache_memory.stats"},
	{{ CTL_VFS, VFS_CACHE_MEMORY, VFS_CACHE_MEMORY_TARGET }, 3, "vfs.cache_memory.target_bytes"},
	{{ CTL_VFS, VFS_READAHEAD, VFS_READAHEAD_STATS }, 3, "vfs.readahead.stats"},
	{{ CTL_VFS, VFS_WRITEBACK, VFS_WRITEBACK_STATS }, 3, "vfs.writeback.stats"},
	{{ CTL_VFS, VFS_WRITEBACK, VFS_WRITEBACK_CONTROL }, 3, "vfs.writeback.control"},
};

static struct spinlock hostname_lock;

/*
 * How many GPU devices a driver has attached but not yet published a node
 * for or given up on (hw.gpu.attaching).
 *
 * A driver that finishes its bring-up after the attach (the i915 starts its
 * device on a worker) counts the device from the attach, which comes before
 * init, until its node is published or its start has failed.  The graphical
 * login waits for /dev/gpu0 only while this is nonzero, so a machine without
 * such a device falls back to the console at once.  It is changed only by
 * atomic operations and never goes below zero.
 */
static atomic_uint_t gpu_attaching;

/*
 * The operations behind hw.gpu.start, or NULL while no driver offers a
 * start root can ask for.
 *
 * A driver installs them once on the boot thread while it registers, before
 * user space runs, so a sysctl call never sees the pointer change.  They
 * stay for the whole kernel lifetime.
 */
static const struct kern_gpu_start_ops *gpu_start_ops;
static char hostname[KERN_HOST_NAME_MAX + 1U] = "zedbsd";

/* The most GPUs hw.gputelemetry lists. */
#define GPU_TELEMETRY_MAX	8U

/* One GPU hw.gputelemetry lists: its driver's name, the function that reads it, and its context. */
struct gpu_telemetry_source {
	char driver[16];
	kern_gpu_telemetry_read_t read;
	void *context;
};

/*
 * The GPUs hw.gputelemetry lists (ws134-p007).
 *
 * A driver appends one under gpu_telemetry_lock and publishes it by raising
 * gpu_telemetry_count with release; a reader loads the count with acquire
 * and reads the sources below it without the lock.  Nothing is ever taken
 * out: a source and its context live as long as the kernel.
 */
static struct gpu_telemetry_source gpu_telemetry_sources[GPU_TELEMETRY_MAX];
static atomic_uint_t gpu_telemetry_count;
static struct spinlock gpu_telemetry_lock;

/* The most sources hw.thermal lists, and the most sensors one source gives. */
#define THERMAL_SOURCES_MAX	4U
#define THERMAL_SOURCE_SENSORS	16U

/* One source hw.thermal lists: the function that reads its sensors, and its context. */
struct thermal_source {
	kern_thermal_read_t read;
	void *context;
};

/*
 * The sources hw.thermal lists (ws134-p009), appended and published as
 * the GPUs of hw.gputelemetry are: under thermal_lock, by raising
 * thermal_count with release; never taken out.
 */
static struct thermal_source thermal_sources[THERMAL_SOURCES_MAX];
static atomic_uint_t thermal_count;
static struct spinlock thermal_lock;

static int sysctl_thermal(void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
static int sysctl_gpu_start(void *oldp, size_t *oldlenp, const void *newp, size_t newlen, int superuser);
static int sysctl_cputimes(void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
static int sysctl_diskstats(void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
static int sysctl_gputelemetry(void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
static int sysctl_writeback(const int *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen, int superuser);
static int oid_compare(const int *a, unsigned alen, const int *b, unsigned blen);
static const struct sysctl_leaf *find_oid(const int *oid, unsigned oidlen);
static int sysctl_output(void *oldp, size_t *oldlenp, const void *value, size_t size);
static int sysctl_meta(int operation, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);

/*
 * Initializes the host name lock.
 */
void
sysctl_init(
	void)
{
	spin_init(&hostname_lock, LOCK_RANK_DEVICE, "hostname");
	spin_init(&gpu_telemetry_lock, LOCK_RANK_DEVICE, "gpu telemetry");
	spin_init(&thermal_lock, LOCK_RANK_DEVICE, "thermal");
}

/*
 * Lists a source of temperatures in hw.thermal (ws134-p009): the function
 * that reads its sensors and its context, which live as long as the
 * kernel.  A context already listed is not listed again.  Returns 0, or
 * ENOSPC when the list is full, EINVAL without a function.
 */
int
kern_thermal_register(
	kern_thermal_read_t read,
	void *context)
{
	struct thermal_source *source;
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* A source needs its function. */
	if (read == NULL)
		return EINVAL;

	/* Appended under the lock, published by the count. */
	irq = spin_lock_irqsave(&thermal_lock);

	/* A context already listed stays as it is. */
	count = atomic_load_acquire(&thermal_count);
	for (index = 0; index < count; index++) {
		if (thermal_sources[index].context == context) {
			spin_unlock_irqrestore(&thermal_lock, irq);
			return 0;
		}
	}

	/* A full list. */
	if (count >= THERMAL_SOURCES_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* The new source, whole before the count shows it. */
	source = &thermal_sources[count];
	source->read = read;
	source->context = context;
	atomic_store_release(&thermal_count, count + 1U);

	/* Another source may be listed now. */
	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the source is listed. */
	return 0;
}

/*
 * Lists a GPU in hw.gputelemetry (ws134-p007): its driver's name, the
 * function that reads it and its context, which live as long as the
 * kernel.  A context already listed is not listed again.  Returns 0, or
 * ENOSPC when the list is full, EINVAL without a function.
 */
int
kern_gpu_telemetry_register(
	const char *driver,
	kern_gpu_telemetry_read_t read,
	void *context)
{
	struct gpu_telemetry_source *source;
	unsigned long irq;
	unsigned count;
	unsigned index;
	size_t length;

	/* A source needs its function. */
	if (read == NULL || driver == NULL)
		return EINVAL;

	/* Appended under the lock, published by the count. */
	irq = spin_lock_irqsave(&gpu_telemetry_lock);

	/* A context already listed stays as it is. */
	count = atomic_load_acquire(&gpu_telemetry_count);
	for (index = 0; index < count; index++) {
		if (gpu_telemetry_sources[index].context == context) {
			spin_unlock_irqrestore(&gpu_telemetry_lock, irq);
			return 0;
		}
	}

	/* A full list. */
	if (count >= GPU_TELEMETRY_MAX) {
		spin_unlock_irqrestore(&gpu_telemetry_lock, irq);
		return ENOSPC;
	}

	/* The new source, whole before the count shows it. */
	source = &gpu_telemetry_sources[count];
	kern_memset(source, 0, sizeof(*source));
	length = kern_strlen(driver);
	if (length > sizeof(source->driver) - 1U)
		length = sizeof(source->driver) - 1U;
	kern_memcpy(source->driver, driver, length);
	source->read = read;
	source->context = context;
	atomic_store_release(&gpu_telemetry_count, count + 1U);

	/* Another driver may list its GPU now. */
	spin_unlock_irqrestore(&gpu_telemetry_lock, irq);

	/* Succeeded: the GPU is listed. */
	return 0;
}

/*
 * Counts a GPU device whose driver has attached it and will publish its node
 * later.
 */
void
kern_gpu_attach_begin(
	void)
{
	/* One more device is on its way to a published node. */
	(void)atomic_fetch_add_relaxed(&gpu_attaching, 1U);
}

/*
 * Uncounts a GPU device whose node is now published or never will be.
 *
 * Each call matches one kern_gpu_attach_begin(); an unmatched call leaves
 * the count at zero.
 */
void
kern_gpu_attach_end(
	void)
{
	unsigned expected;
	int exchanged;

	/* Takes one device off the count, unless the count is already zero. */
	expected = atomic_load_acquire(&gpu_attaching);
	while (expected != 0U) {
		exchanged = atomic_compare_exchange(&gpu_attaching, &expected, expected - 1U);
		if (exchanged)
			break;
	}
}

/*
 * Installs the operations behind hw.gpu.start.
 */
void
kern_gpu_start_ops_set(
	const struct kern_gpu_start_ops *ops)
{
	/* Publishes the driver's operations for every later sysctl call. */
	gpu_start_ops = ops;
}

/*
 * Reads or writes one sysctl leaf.
 *
 * Reads follow the usual protocol: with oldp NULL the required size is
 * reported, otherwise the value is copied when it fits.  Writes need the
 * superuser and an exact value size.
 */
int
kern_sysctl(
	const int *name,
	unsigned namelen,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser)
{
	struct bufcache_stats stats;
	struct io_stats io_stats;
	struct cache_memory_stats cache_memory;
	struct readahead_report readahead;
	struct memory_stats memory;
	struct root_image_info root_image;
	struct hal_memstat hal_memory;
	const char *new_name;
	const char *login;
	uint64_t value;
	uint32_t cpus;
	uint32_t attaching;
	unsigned long irq;
	size_t length;
	size_t i;
	size_t capacity;
	size_t needed;
	int error;

	/* Rejects an empty or overlong object identifier. */
	if (name == NULL || namelen == 0 || namelen > CTL_MAXNAME)
		return EINVAL;

	/* Routes the meta operations. */
	if (namelen == 2 && name[0] == CTL_SYSCTL) {
		error = sysctl_meta(name[1], oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Reports the CPU count for both hardware leaves. */
	if (namelen == 2 &&
	    name[0] == CTL_HW &&
	    (name[1] == HW_NCPU || name[1] == HW_NCPUONLINE)) {
		cpus = hal_cpu_count();
		if (newp != NULL || newlen != 0)
			return EPERM;
		error = sysctl_output(oldp, oldlenp, &cpus, sizeof(cpus));
		return error;
	}

	/* Reports the GPU devices whose node the graphical login may still wait for. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_GPU_ATTACHING) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		attaching = atomic_load_acquire(&gpu_attaching);
		error = sysctl_output(oldp, oldlenp, &attaching, sizeof(attaching));
		return error;
	}

	/* Reports or starts the GPU devices a driver holds for root's start. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_GPU_START) {
		error = sysctl_gpu_start(oldp, oldlenp, newp, newlen, superuser);
		return error;
	}

	/* Reports each CPU's ticks by what it was doing. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_CPUTIMES) {
		error = sysctl_cputimes(oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Reports each physical disk's work. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_DISKSTATS) {
		error = sysctl_diskstats(oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Reports each GPU's work as its driver keeps it. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_GPUTELEMETRY) {
		error = sysctl_gputelemetry(oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Reports the temperature sensors as their sources keep them. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_THERMAL) {
		error = sysctl_thermal(oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Distinguishes reported RAM from the allocator's address span. */
	if (namelen == 2 && name[0] == CTL_HW && name[1] == HW_MEMORY_STATS) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		kern_memset(&hal_memory, 0, sizeof(hal_memory));
		hal_get_memstat(&hal_memory);
		kern_memset(&memory, 0, sizeof(memory));
		memory.version = MEMORY_STATS_VERSION;
		memory.boot_ranges_valid = hal_memory.boot_ranges_valid;
		memory.boot_range_count = hal_memory.boot_range_count;
		memory.boot_usable_bytes = hal_memory.boot_usable_bytes;
		memory.boot_highest_end = hal_memory.boot_highest_end;
		memory.boot_usable_highest_end = hal_memory.boot_usable_highest_end;
		memory.direct_mapped_bytes = hal_memory.direct_mapped_bytes;
		memory.allocator_initial_bytes = hal_memory.allocator_initial_bytes;
		memory.boot_reclaim_bytes = hal_memory.boot_reclaim_bytes;
		memory.allocator_metadata_bytes = hal_memory.allocator_metadata_bytes;
		memory.allocator_scan_words = hal_memory.allocator_scan_words;
		memory.allocator_max_extent_scan_words = hal_memory.allocator_max_extent_scan_words;
		memory.allocator_max_irqoff_cycles = hal_memory.allocator_max_irqoff_cycles;
		memory.boot_memory_source = hal_memory.boot_memory_source;
		memory.physical_managed_bytes = hal_memory.physical_total;
		memory.physical_reserved_bytes = hal_memory.physical_reserved;
		memory.physical_allocated_bytes = hal_memory.physical_allocated;
		memory.physical_free_bytes = hal_memory.physical_free;
		return sysctl_output(oldp, oldlenp, &memory, sizeof(memory));
	}

	/* Root-image admission reads current referenced objects, not boot text. */
	if (namelen == 2 && name[0] == CTL_KERN && name[1] == KERN_BOOT_ROOT_IMAGE) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		error = kern_vfs_root_image_info(&root_image);
		if (error != 0)
			return error;
		return sysctl_output(oldp, oldlenp, &root_image, sizeof(root_image));
	}

	/* The login= boot parameter, for the graphical login's sessiond (ws035-p098). */
	if (namelen == 2 && name[0] == CTL_KERN && name[1] == KERN_BOOT_LOGIN) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		login = kern_boot_parameters_value(kern_boot_parameters_current(), KERN_BOOT_PARAMETER_LOGIN);
		if (login == NULL)
			login = "";
		return sysctl_output(oldp, oldlenp, login, kern_strlen(login) + 1);
	}

	/* Reports retained loader identities independently from boot parameters. */
	if (namelen == 2 && name[0] == CTL_KERN &&
	    name[1] >= KERN_BOOT_FIRMWARE && name[1] <= KERN_BOOT_CONFIG_MATCHES) {
		const char *selector;
		uint64_t matches;

		if (newp != NULL || newlen != 0)
			return EPERM;
		if (name[1] == KERN_BOOT_CONFIG_MATCHES) {
			matches = kern_boot_config_matches();
			return sysctl_output(oldp, oldlenp, &matches, sizeof(matches));
		}
		selector = kern_boot_source_selector(name[1] == KERN_BOOT_CONFIGURATION);
		return sysctl_output(oldp, oldlenp, selector, kern_strlen(selector) + 1);
	}

	/* Handles the kernel leaves. */
	if (namelen == 2 && name[0] == CTL_KERN) {
		/* The host name is the only writable kernel leaf. */
		if (name[1] == KERN_HOSTNAME) {
			if (newp != NULL) {
				new_name = newp;
				if (!superuser)
					return EPERM;
				if (newlen == 0 || newlen > KERN_HOST_NAME_MAX)
					return EINVAL;

				/* Rejects a name with a terminator, slash, or whitespace. */
				for (i = 0; i < newlen; i++) {
					if (new_name[i] == '\0' ||
					    new_name[i] == '/' ||
					    new_name[i] == ' ' ||
					    new_name[i] == '\t' ||
					    new_name[i] == '\n')
						return EINVAL;
				}

				/* Installs the new name. */
				irq = spin_lock_irqsave(&hostname_lock);
				kern_memcpy(hostname, new_name, newlen);
				hostname[newlen] = '\0';
				spin_unlock_irqrestore(&hostname_lock, irq);
			} else if (newlen != 0) {
				return EINVAL;
			}

			/* Reports the current name with its terminator. */
			irq = spin_lock_irqsave(&hostname_lock);
			length = kern_strlen(hostname) + 1U;
			error = sysctl_output(oldp, oldlenp, hostname, length);
			spin_unlock_irqrestore(&hostname_lock, irq);
			return error;
		}

		/* Every other kernel leaf is read-only. */
		if (newp != NULL || newlen != 0)
			return EPERM;

		/* Reports the message buffer capacity. */
		if (name[1] == KERN_MSGBUF_SIZE) {
			value = kern_log_capacity();
			error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
			return error;
		}

		/* Reports the bytes dropped from the message buffer. */
		if (name[1] == KERN_MSGBUF_DROPPED) {
			(void)kern_log_snapshot(NULL, 0, &value);
			error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
			return error;
		}

		/* Copies the message buffer, sizing it on a NULL buffer. */
		if (name[1] == KERN_MSGBUF) {
			if (oldlenp == NULL) {
				if (oldp == NULL)
					return 0;
				return EINVAL;
			}

			capacity = *oldlenp;
			needed = kern_log_snapshot(NULL, 0, NULL);
			*oldlenp = needed;
			if (oldp == NULL)
				return 0;
			if (capacity < needed)
				return ENOMEM;
			needed = kern_log_snapshot(oldp, capacity, NULL);
			*oldlenp = needed;
			if (needed > capacity)
				return ENOMEM;
			return 0;
		}

		/* Reports an unknown kernel leaf. */
		return ENOENT;
	}

	/* Exposes cumulative I/O events without allowing counter resets. */
	if (namelen == 3 && name[0] == CTL_VFS &&
	    name[1] == VFS_IO && name[2] == VFS_IO_STATS) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		io_stats_snapshot(&io_stats);
		return sysctl_output(oldp, oldlenp, &io_stats, sizeof(io_stats));
	}

	/* Exposes optional read accounting with explicit conservative byte attribution. */
	if (namelen == 3 && name[0] == CTL_VFS &&
	    name[1] == VFS_READAHEAD && name[2] == VFS_READAHEAD_STATS) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		readahead_report(&readahead);
		error = sysctl_output(oldp, oldlenp, &readahead, sizeof(readahead));
		return error;
	}

	/* Exposes shared ownership and commits only successful clean-cache shrinks. */
	if (namelen == 3 && name[0] == CTL_VFS && name[1] == VFS_CACHE_MEMORY) {
		cache_memory_get_stats(&cache_memory);
		if (name[2] == VFS_CACHE_MEMORY_STATS) {
			if (newp != NULL || newlen != 0)
				return EPERM;
			error = sysctl_output(oldp, oldlenp, &cache_memory, sizeof(cache_memory));
			return error;
		}

		if (name[2] != VFS_CACHE_MEMORY_TARGET)
			return ENOENT;
		value = cache_memory.target_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		if (error != 0)
			return error;
		if (newp == NULL)
			return newlen == 0 ? 0 : EINVAL;
		if (!superuser)
			return EPERM;
		if (newlen != sizeof(value))
			return EINVAL;
		kern_memcpy(&value, newp, sizeof(value));
		error = cache_memory_set_target(value);
		return error;
	}

	/* Dispatches the explicit per-mount writeback policy interface. */
	if (namelen == 3 && name[0] == CTL_VFS && name[1] == VFS_WRITEBACK)
		return sysctl_writeback(name, oldp, oldlenp, newp, newlen, superuser);

	/* Everything else must be a known buffer cache leaf. */
	if (namelen != 3 ||
	    name[0] != CTL_VFS ||
	    name[1] != VFS_BUFCACHE ||
	    find_oid(name, namelen) == NULL)
		return ENOENT;

	/* Serves the buffer cache leaves from one statistics snapshot. */
	buf_get_stats(&stats);
	switch (name[2]) {
	case VFS_BUFCACHE_MAX_BYTES:
		/* Reports the limit, then applies a superuser's new limit. */
		value = stats.max_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		if (error != 0)
			return error;
		if (newp == NULL) {
			if (newlen == 0)
				return 0;
			return EINVAL;
		}

		if (!superuser)
			return EPERM;
		if (newlen != sizeof(value))
			return EINVAL;
		kern_memcpy(&value, newp, sizeof(value));
		error = buf_set_max_bytes(value);
		return error;
	case VFS_BUFCACHE_CURRENT_BYTES:
	case VFS_BUFCACHE_DIRTY_BYTES:
		if (newp != NULL || newlen != 0)
			return EPERM;
		if (name[2] == VFS_BUFCACHE_CURRENT_BYTES)
			value = stats.current_bytes;
		else
			value = stats.dirty_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		return error;
	case VFS_BUFCACHE_STATS:
		if (newp != NULL || newlen != 0)
			return EPERM;
		error = sysctl_output(oldp, oldlenp, &stats, sizeof(stats));
		return error;
	default:
		break;
	}

	/* Reports an unknown buffer cache leaf. */
	return ENOENT;
}

/* Orders two object identifiers lexicographically. */
static int
oid_compare(
	const int *a,
	unsigned alen,
	const int *b,
	unsigned blen)
{
	unsigned i;
	unsigned count;

	/* Compares the common prefix element by element. */
	count = blen;
	if (alen < blen)
		count = alen;
	for (i = 0; i < count; i++) {
		if (a[i] < b[i])
			return -1;
		if (a[i] > b[i])
			return 1;
	}

	/* A shorter identifier with an equal prefix orders first. */
	if (alen < blen)
		return -1;
	if (alen > blen)
		return 1;

	/* Reports equal identifiers. */
	return 0;
}

/* Finds the leaf with an exact object identifier. */
static const struct sysctl_leaf *
find_oid(
	const int *oid,
	unsigned oidlen)
{
	unsigned i;

	/* Searches the fixed leaf table. */
	for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
		if (oid_compare(oid, oidlen, leaves[i].oid, leaves[i].oidlen) == 0)
			return &leaves[i];
	}

	/* Reports an unknown identifier. */
	return NULL;
}

/* Copies a value out following the sysctl sizing protocol. */
static int
sysctl_output(
	void *oldp,
	size_t *oldlenp,
	const void *value,
	size_t size)
{
	size_t capacity;

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* Reports the required size before copying. */
	capacity = *oldlenp;
	*oldlenp = size;
	if (oldp == NULL)
		return 0;
	if (capacity < size)
		return ENOMEM;

	/* Copies the value. */
	kern_memcpy(oldp, value, size);

	/* Reports the copied value. */
	return 0;
}

/*
 * Reads hw.cputimes: the header and each CPU's ticks (ws134-p005).
 *
 * The value is built straight into the caller's buffer, a CPU at a time,
 * so its size grows with the CPUs and not the stack.  A buffer too small
 * fails with ENOMEM and the length needed.
 */
static int
sysctl_cputimes(
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	struct cpu_times_header header;
	struct cpu_times_entry entry;
	struct sched_cpu_time time;
	uint8_t *output;
	uint32_t count;
	uint32_t cpu;
	size_t needed;
	size_t capacity;
	int error;

	/* Read-only. */
	if (newp != NULL || newlen != 0)
		return EPERM;

	/* The length a reader needs: the header and an entry a CPU. */
	count = hal_cpu_count();
	needed = sizeof(header) + (size_t)count * sizeof(entry);

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* The length needed, and nothing more without a buffer or with one too small. */
	capacity = *oldlenp;
	*oldlenp = needed;
	if (oldp == NULL)
		return 0;
	if (capacity < needed)
		return ENOMEM;

	/* The header. */
	kern_memset(&header, 0, sizeof(header));
	header.version = CPU_TIMES_VERSION;
	header.struct_size = sizeof(header);
	header.element_size = sizeof(entry);
	header.count = count;
	header.hz = KERN_CLOCK_HZ;
	output = oldp;
	kern_memcpy(output, &header, sizeof(header));

	/* Each CPU's ticks after it (a CPU the scheduler does not have reads zero). */
	for (cpu = 0; cpu < count; cpu++) {
		kern_memset(&entry, 0, sizeof(entry));
		error = sched_cpu_time(cpu, &time);
		if (error == 0) {
			entry.user = time.user;
			entry.system = time.system;
			entry.idle = time.idle;
			entry.other = time.other;
		}

		/* Its place after the header. */
		kern_memcpy(output + sizeof(header) + (size_t)cpu * sizeof(entry), &entry, sizeof(entry));
	}

	/* Succeeded: the value is in the buffer. */
	return 0;
}

/*
 * Reads hw.diskstats: the header and each physical whole disk's work
 * (ws134-p006), written by the disk registry under its lock.  A buffer too
 * small fails with ENOMEM and the length needed.
 */
static int
sysctl_diskstats(
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	size_t needed;
	size_t capacity;
	int error;

	/* Read-only. */
	if (newp != NULL || newlen != 0)
		return EPERM;

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* The value, or only its length without a buffer. */
	capacity = *oldlenp;
	error = disk_stats_copy(oldp, capacity, &needed);
	*oldlenp = needed;

	/* Reports a buffer too small. */
	if (error != 0)
		return error;

	/* Succeeded: the value (or its length) is given. */
	return 0;
}

/*
 * Reads hw.gputelemetry: the header and each listed GPU as its driver reads
 * it (ws134-p007).  A GPU whose driver fails to read it is left out.  A
 * buffer too small fails with ENOMEM and the length needed (every listed
 * GPU's, so a reader that asks again with it has room).
 */
static int
sysctl_gputelemetry(
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	struct gpu_telemetry_header header;
	struct gpu_telemetry_entry entry;
	const struct gpu_telemetry_source *source;
	uint8_t *output;
	unsigned listed;
	unsigned index;
	uint32_t count;
	size_t needed;
	size_t capacity;
	int error;

	/* Read-only. */
	if (newp != NULL || newlen != 0)
		return EPERM;

	/* The length every listed GPU takes. */
	listed = atomic_load_acquire(&gpu_telemetry_count);
	needed = sizeof(header) + (size_t)listed * sizeof(entry);

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* The length needed, and nothing more without a buffer or with one too small. */
	capacity = *oldlenp;
	*oldlenp = needed;
	if (oldp == NULL)
		return 0;
	if (capacity < needed)
		return ENOMEM;

	/* Each GPU its driver reads, after the header's place. */
	output = oldp;
	count = 0;
	for (index = 0; index < listed; index++) {
		/* The entry with its driver's name, the rest for the driver. */
		source = &gpu_telemetry_sources[index];
		kern_memset(&entry, 0, sizeof(entry));
		kern_memcpy(entry.driver, source->driver, sizeof(entry.driver));
		error = source->read(source->context, &entry);
		if (error != 0)
			continue;

		/* Into the next place. */
		kern_memcpy(output + sizeof(header) + (size_t)count * sizeof(entry), &entry, sizeof(entry));
		count++;
	}

	/* The header, with the GPUs read; the length is theirs. */
	kern_memset(&header, 0, sizeof(header));
	header.version = GPU_TELEMETRY_VERSION;
	header.struct_size = sizeof(header);
	header.element_size = sizeof(entry);
	header.count = count;
	kern_memcpy(output, &header, sizeof(header));
	*oldlenp = sizeof(header) + (size_t)count * sizeof(entry);

	/* Succeeded: the value is in the buffer. */
	return 0;
}

/*
 * Reads hw.thermal: the header and each listed source's sensors as it
 * keeps them (ws134-p009).  A source that fails is left out.  The length
 * asked for is room for every source's most sensors, so a reader that asks
 * again with it has room; the length given back is what was filled.
 */
static int
sysctl_thermal(
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	struct thermal_header header;
	struct thermal_entry *entries;
	const struct thermal_source *source;
	uint8_t *output;
	unsigned listed;
	unsigned index;
	unsigned filled;
	uint32_t count;
	size_t needed;
	size_t capacity;
	int error;

	/* Read-only. */
	if (newp != NULL || newlen != 0)
		return EPERM;

	/* The length every listed source's most sensors take. */
	listed = atomic_load_acquire(&thermal_count);
	needed = sizeof(header) + (size_t)listed * THERMAL_SOURCE_SENSORS * sizeof(struct thermal_entry);

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* The length needed, and nothing more without a buffer or with one too small. */
	capacity = *oldlenp;
	*oldlenp = needed;
	if (oldp == NULL)
		return 0;
	if (capacity < needed)
		return ENOMEM;

	/* Each source's sensors, after the header's place and the sensors before them. */
	output = oldp;
	count = 0;
	for (index = 0; index < listed; index++) {
		source = &thermal_sources[index];
		entries = (struct thermal_entry *)(void *)(output + sizeof(header) + (size_t)count * sizeof(*entries));
		kern_memset(entries, 0, THERMAL_SOURCE_SENSORS * sizeof(*entries));
		filled = 0;
		error = source->read(source->context, entries, THERMAL_SOURCE_SENSORS, &filled);
		if (error != 0)
			continue;
		if (filled > THERMAL_SOURCE_SENSORS)
			filled = THERMAL_SOURCE_SENSORS;
		count += filled;
	}

	/* The header, with the sensors read; the length is theirs. */
	kern_memset(&header, 0, sizeof(header));
	header.version = THERMAL_VERSION;
	header.struct_size = sizeof(header);
	header.element_size = sizeof(struct thermal_entry);
	header.count = count;
	kern_memcpy(output, &header, sizeof(header));
	*oldlenp = sizeof(header) + (size_t)count * sizeof(struct thermal_entry);

	/* Succeeded: the value is in the buffer. */
	return 0;
}

/*
 * Reads or writes hw.gpu.start.
 *
 * A write of 1 by the superuser starts the held devices first; the read
 * then reports how many are still held.
 */
static int
sysctl_gpu_start(
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser)
{
	uint64_t request;
	uint64_t held;
	int error;

	/* A length without a value asks for nothing that can be written. */
	if (newp == NULL && newlen != 0)
		return EINVAL;

	/* Starts the held devices when root asks for it. */
	if (newp != NULL) {
		/* Only the superuser may start a device. */
		if (!superuser)
			return EPERM;

		/* The request is one 64-bit word. */
		if (newlen != sizeof(request))
			return EINVAL;

		/* The only request is 1, "start them". */
		kern_memcpy(&request, newp, sizeof(request));
		if (request != 1U)
			return EINVAL;

		/* Without a driver that holds devices there is nothing to start. */
		if (gpu_start_ops == NULL)
			return ENODEV;

		/* Hands the held devices to the driver's start. */
		error = gpu_start_ops->start();
		if (error != 0)
			return error;
	}

	/* Counts the devices still held; without a driver none is. */
	held = 0U;
	if (gpu_start_ops != NULL)
		held = gpu_start_ops->held();

	/* Reports the count to a caller that asked for it. */
	error = sysctl_output(oldp, oldlenp, &held, sizeof(held));
	if (error != 0)
		return error;

	/* Succeeded: the count is reported and any requested start has begun. */
	return 0;
}

/* Handles the name, identifier, and enumeration meta operations. */
static int
sysctl_meta(
	int operation,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	const struct sysctl_leaf *leaf;
	const struct sysctl_leaf *next;
	const char *name;
	const int *current;
	unsigned current_len;
	unsigned i;
	int error;

	/* Translates a terminated name to its identifier. */
	if (operation == CTL_SYSCTL_NAME2OID) {
		name = newp;
		if (name == NULL ||
		    newlen == 0 ||
		    newlen > SYSCTL_NAME_MAX ||
		    name[newlen - 1U] != '\0')
			return EINVAL;
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
			if (!kern_strcmp(name, leaves[i].name)) {
				error = sysctl_output(oldp, oldlenp, leaves[i].oid,
				    leaves[i].oidlen * sizeof(int));
				return error;
			}
		}

		return ENOENT;
	}

	/* Translates an identifier to its terminated name. */
	if (operation == CTL_SYSCTL_OIDNAME) {
		if (newp == NULL ||
		    newlen == 0 ||
		    newlen % sizeof(int) != 0 ||
		    newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		leaf = find_oid(newp, (unsigned)(newlen / sizeof(int)));
		if (leaf == NULL)
			return ENOENT;
		error = sysctl_output(oldp, oldlenp, leaf->name, kern_strlen(leaf->name) + 1U);
		return error;
	}

	/* Reports the smallest identifier after the given one. */
	if (operation == CTL_SYSCTL_NEXT) {
		current = newp;
		next = NULL;
		if (newlen % sizeof(int) != 0 || newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		current_len = (unsigned)(newlen / sizeof(int));
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
			if (current_len != 0 &&
			    oid_compare(leaves[i].oid, leaves[i].oidlen,
			    current, current_len) <= 0)
				continue;
			if (next != NULL &&
			    oid_compare(leaves[i].oid, leaves[i].oidlen,
			    next->oid, next->oidlen) >= 0)
				continue;
			next = &leaves[i];
		}

		if (next == NULL)
			return ENOENT;
		error = sysctl_output(oldp, oldlenp, next->oid, next->oidlen * sizeof(int));
		return error;
	}

	/* Reports an unknown meta operation. */
	return ENOENT;
}

/* Validates control before changing policy and sizes reports before filling them. */
static int
sysctl_writeback(
	const int *name,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser)
{
	struct writeback_control request;
	struct mount *mount;
	size_t capacity;
	int error;

	/* Supports size queries without allocating a large intermediate report. */
	if (name[2] == VFS_WRITEBACK_STATS) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		if (oldlenp == NULL)
			return oldp == NULL ? 0 : EINVAL;
		capacity = *oldlenp;
		*oldlenp = sizeof(struct writeback_report);
		if (oldp == NULL)
			return 0;
		if (capacity < sizeof(struct writeback_report))
			return ENOMEM;
		writeback_policy_report(oldp);
		return 0;
	}

	/* Restricts mutations to exact versioned root-only requests on a live mount. */
	if (name[2] != VFS_WRITEBACK_CONTROL)
		return ENOENT;
	if (!superuser)
		return EPERM;
	if (newp == NULL)
		return EOPNOTSUPP;
	if (newlen != sizeof(request) || oldp != NULL || oldlenp != NULL)
		return EINVAL;
	kern_memcpy(&request, newp, sizeof(request));
	if (request.version != WRITEBACK_REPORT_VERSION || request.enabled > 1 ||
	    request.path[0] != '/' || kern_memchr(request.path, '\0', sizeof(request.path)) == NULL)
		return EINVAL;
	mount = mount_find_ref(request.path);
	if (mount == NULL)
		return ENOENT;
	error = writeback_mount_set(mount, (int)request.enabled);
	mount_release(mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
