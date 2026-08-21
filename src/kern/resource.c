/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

void
kern_resource_snapshot(struct system_resource_info *out)
{
	struct swap_backend *swap;
	uint32_t total = 0, free = 0;
	if (out == NULL)
		return;
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
	swap = swap_system_backend();
	if (swap != NULL && swap_get_stats(swap, &total, &free) == 0)
		out->swap_slot = total - free;
	out->disk = disk_count();
	/* bio requests are caller-owned and synchronous; disk inflight is the
	 * meaningful live resource count. */
	out->bio = disk_inflight_count();
	out->socket = socket_count_current();
	out->packet = packet_buf_in_use();
	out->net_device = net_device_count();
}

int
kern_resource_equal(const struct system_resource_info *a,
	const struct system_resource_info *b)
{
	return a != NULL && b != NULL && memcmp(a, b, sizeof(*a)) == 0;
}
