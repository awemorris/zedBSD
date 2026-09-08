/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shutdown preparation.
 *
 * The first caller quiesces the network, USB, and PCI layers in dependency
 * order; every later caller waits until that preparation has finished.
 */

#include "kern/system-device.h"
#include "kern/atomic.h"
#include "kern/sched.h"
#include "kern/readahead.h"
#include <errno.h>

extern void net_shutdown_for_boot(void) __attribute__((weak));
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
			atomic_store_release(&shutdown_preparation_state, 0);
			return error;
		}
	}

	if (readahead_trim != NULL) {
		error = readahead_trim();
		if (error != 0) {
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
			shutdown_readers_restore();
			return error;
		}
	}

	if (mount_sync_all != NULL) {
		error = mount_sync_all();
		if (error != 0) {
			if (writeback_shutdown_finish != NULL)
				writeback_shutdown_finish(0);
			shutdown_readers_restore();
			return error;
		}
	}

	if (writeback_shutdown_finish != NULL)
		writeback_shutdown_finish(1);

	/*
	 * Network close joins driver RX/TX producers.  USB then disconnects
	 * class interfaces and drains HCD ownership before PCI shutdown runs.
	 */
	if (net_shutdown_for_boot != NULL)
		net_shutdown_for_boot();
	if (drv_usb_shutdown != NULL)
		drv_usb_shutdown();
	if (drv_pci_shutdown != NULL)
		drv_pci_shutdown();

	/* Publishes the finished preparation to the waiting callers. */
	atomic_store_release(&shutdown_preparation_state, 2U);
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
