/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * System resource accounting.
 *
 * The snapshot gathers the live object counts of every kernel subsystem
 * into one record, so tests and diagnostics can detect leaks by comparing
 * two snapshots.
 */

#include "kern/resource.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/pipe.h"
#include "kern/process.h"
#include "kern/swap.h"
#include "kern/vm-object.h"
#include "kern/vmspace.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"

#include <string.h>

/*
 * Records the current live object count of every subsystem.
 */
void
kern_resource_snapshot(
	struct system_resource_info *out)
{
	struct swap_backend *swap;
	uint32_t total;
	uint32_t free;

	total = 0;
	free = 0;

	/* Ignores a missing record. */
	if (out == NULL)
		return;

	/* Counts the process, file, filesystem, and memory objects. */
	memset(out, 0, sizeof(*out));
	process_resource_count(&out->process, &out->thread);
	out->filedesc = filedesc_count();
	out->file = file_count();
	out->pipe = pipe_count();
	out->mount = mount_count();
	out->inode = inode_cache_count();
	out->namecache = namecache_count();
	out->vmspace = vmspace_count();
	out->vm_object = vm_object_count();
	out->vm_page = vm_object_page_count();

	/* Counts the swap slots in use when a swap backend exists. */
	swap = swap_system_backend();
	if (swap != NULL && swap_get_stats(swap, &total, &free) == 0)
		out->swap_slot = total - free;

	/*
	 * bio requests are caller-owned and synchronous; disk inflight is the
	 * meaningful live resource count.
	 */
	out->disk = disk_count();
	out->bio = disk_inflight_count();

	/* Counts the network objects. */
	out->socket = socket_count_current();
	out->packet = packet_buf_in_use();
	out->net_device = net_device_count();
}

/*
 * Tests whether two resource snapshots are identical.
 */
int
kern_resource_equal(
	const struct system_resource_info *a,
	const struct system_resource_info *b)
{
	/* A missing snapshot equals nothing. */
	if (a == NULL || b == NULL)
		return 0;

	/* Compares every count. */
	if (memcmp(a, b, sizeof(*a)) != 0)
		return 0;

	/* Reports identical snapshots. */
	return 1;
}
