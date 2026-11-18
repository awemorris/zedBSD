/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shutdown preparation.
 *
 * The first caller flushes the disks and quiesces the network, USB, and PCI
 * layers in dependency order; every later caller waits until that
 * preparation has finished.
 */

#include "kern/system-device.h"
#include "kern/disk.h"
#include "kern/atomic.h"
#include "kern/sched.h"
#include "kern/readahead.h"
#include "kern/klog.h"
#include "kern/platform.h"
#include <uapi/errno.h>

/*
 * The layers a kernel may be built without.
 *
 * Each is weak so a configuration that leaves the layer out still links,
 * and every call site tests the symbol before it calls.
 */
extern int net_shutdown_for_boot(void) __attribute__((weak));
extern void drv_usb_shutdown(void) __attribute__((weak));
extern void drv_pci_shutdown(void) __attribute__((weak));

extern int writeback_shutdown_begin(void) __attribute__((weak));
extern void writeback_shutdown_finish(int) __attribute__((weak));
extern int mount_sync_all(void) __attribute__((weak));

extern int readahead_boundary_begin(struct readahead_boundary *, struct mount *) __attribute__((weak));
extern void readahead_boundary_end(struct readahead_boundary *) __attribute__((weak));
extern int readahead_trim(void) __attribute__((weak));

static atomic_uint_t shutdown_preparation_state;
static struct readahead_boundary shutdown_readahead;

static void shutdown_readers_restore(void);
static void shutdown_disks_flush(void);
static int shutdown_disk_flushable(const struct disk *disk);

/*
 * Quiesces the device layers before a shutdown or reboot.
 *
 * A successful preparation runs once. Failed storage barriers leave device
 * layers available and permit a later retry. Callers stop userspace producers
 * before entering; this boundary joins optional filesystem syncers.
 */
int
system_shutdown_prepare(
	void)
{
	unsigned expected;
	int network_stopped;
	int error;

	/* Joins a successful shutdown or retries preparation after a reported failure. */
	for (;;) {
		expected = 0;
		if (atomic_compare_exchange(&shutdown_preparation_state, &expected, 1U))
			break;
		if (expected == 2U)
			return 0;
		sched_yield();
	}

	/* Joins speculative readers before VM durability barriers or device teardown. */
	if (readahead_boundary_begin != NULL) {
		if (readahead_boundary_end == NULL) {
			atomic_store_release(&shutdown_preparation_state, 0);
			return EOPNOTSUPP;
		}

		error = readahead_boundary_begin(&shutdown_readahead, NULL);
		if (error != 0) {
			kern_logf("shutdown: the speculative readers could not be stopped (error %d)\n",
				  error);
			atomic_store_release(&shutdown_preparation_state, 0);
			return error;
		}
	}

	/* Drops the pages read ahead, which a shutdown has no use for. */
	if (readahead_trim != NULL) {
		error = readahead_trim();
		if (error != 0) {
			kern_logf("shutdown: the read-ahead pages could not be dropped (error %d)\n",
				  error);
			shutdown_readers_restore();
			return error;
		}
	}

	/* Keeps every device alive if admission closure or writeback fails. */
	if (writeback_shutdown_begin != NULL) {
		if (writeback_shutdown_finish == NULL) {
			shutdown_readers_restore();
			return EOPNOTSUPP;
		}

		error = writeback_shutdown_begin();
		if (error != 0) {
			kern_logf("shutdown: the writeback could not be closed (error %d)\n",
				  error);
			shutdown_readers_restore();
			return error;
		}
	}

	/* Writes every mounted volume out; a failure keeps the devices alive for a retry. */
	if (mount_sync_all != NULL) {
		error = mount_sync_all();
		if (error != 0) {
			kern_logf("shutdown: the volumes could not be synced (error %d); the devices stay up for a retry\n",
				  error);
			if (writeback_shutdown_finish != NULL)
				writeback_shutdown_finish(0);
			shutdown_readers_restore();
			return error;
		}
	}

	/* Ends the writeback; from here on the volumes are on disk. */
	if (writeback_shutdown_finish != NULL)
		writeback_shutdown_finish(1);
	kern_logf("shutdown: the volumes are synced; flushing the disks\n");

	/*
	 * Flushes every disk's write cache before the network stops (BUG-305).
	 * A network that does not stop leaves PCI and USB to the reset, and with
	 * them the drivers' own shutdown flush, so raw writes and unmounted
	 * disks would otherwise depend on the network stopping in time.
	 */
	shutdown_disks_flush();
	kern_logf("shutdown: the disks are flushed; stopping the network\n");

	/*
	 * Stops the platform's device threads first, so none publishes a
	 * device while the network stops (the Raspberry Pi 4's onboard Wi-Fi).
	 */
	if (kern_platform_shutdown_prepare != NULL)
		kern_platform_shutdown_prepare();

	/*
	 * Network close joins driver RX/TX producers.  USB then disconnects
	 * class interfaces and drains HCD ownership before PCI shutdown runs.
	 * A network that does not stop in time may still have producers in
	 * USB and PCI memory, so their teardown is skipped: the data is
	 * already on disk, and the reset or halt that follows stops them.
	 */
	network_stopped = 1;
	if (net_shutdown_for_boot != NULL) {
		error = net_shutdown_for_boot();
		if (error != 0) {
			kern_logf("shutdown: the network did not stop (error %d); USB and PCI are left to the reset\n",
				  error);
			network_stopped = 0;
		}
	}

	/* Takes USB and then PCI out of service behind a stopped network. */
	if (network_stopped) {
		kern_logf("shutdown: stopping USB and PCI\n");
		if (drv_usb_shutdown != NULL)
			drv_usb_shutdown();
		if (drv_pci_shutdown != NULL)
			drv_pci_shutdown();
	}

	/* Publishes the finished preparation to the waiting callers. */
	kern_logf("shutdown: the devices are quiet\n");
	atomic_store_release(&shutdown_preparation_state, 2U);

	/* Succeeded: the machine may be reset, halted or turned off. */
	return 0;
}

/* Reopens speculative admission only when shutdown preparation remains retryable. */
static void
shutdown_readers_restore(
	void)
{
	/* The permanent global token remains installed after successful device shutdown. */
	if (readahead_boundary_end != NULL)
		readahead_boundary_end(&shutdown_readahead);
	atomic_store_release(&shutdown_preparation_state, 0);
}

/*
 * Writes back and flushes the cache of every whole physical disk.
 *
 * The disks still serve requests here, so each flush is an ordinary barrier
 * through its driver.  A failure is logged and the other disks still flush;
 * the shutdown goes on, as the drivers' own shutdown flush does.
 */
static void
shutdown_disks_flush(
	void)
{
	struct disk *disk;
	unsigned index;
	int flushable;
	int error;

	/* Visits the live disks in registration order. */
	index = 0;
	disk = disk_at(index);
	while (disk != NULL) {
		/*
		 * Flushes a whole writable disk, and logs one that did not flush
		 * unless its medium is gone; a partition shares its disk's cache.
		 */
		flushable = shutdown_disk_flushable(disk);
		if (flushable) {
			error = disk_sync(disk);
			if (error != 0 && error != ENXIO)
				kern_logf("shutdown: %s: the disk could not be flushed (error %d)\n",
					  disk->d_name,
					  error);
		}

		/* Drops the reference and moves to the next disk. */
		disk_release(disk);
		index++;
		disk = disk_at(index);
	}
}

/* Tests whether a disk is a whole writable device whose cache shutdown flushes. */
static int
shutdown_disk_flushable(
	const struct disk *disk)
{
	/* A partition is flushed through its whole disk. */
	if (disk->d_parent != NULL)
		return 0;
	if ((disk->d_flags & DISK_PARTITION) != 0)
		return 0;

	/* A read-only disk holds nothing to write. */
	if ((disk->d_flags & DISK_READ_ONLY) != 0)
		return 0;

	/* A loop disk lives in a file on a volume that is already synced. */
	if ((disk->d_flags & DISK_FILE_BACKED) != 0)
		return 0;

	/* Flushes every other disk. */
	return 1;
}
