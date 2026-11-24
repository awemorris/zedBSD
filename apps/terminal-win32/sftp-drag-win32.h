/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Virtual-file OLE drag-and-drop out of an SFTP view tab to Windows
 * Explorer and other drop targets.
 */

#ifndef MIKUTERM_SFTP_DRAG_WIN32_H
#define MIKUTERM_SFTP_DRAG_WIN32_H

#include "apps/mikuterm-common/sftp-worker.h"

#include <stddef.h>
#include <stdint.h>

/* Maximum number of entries expanded into a virtual-file drag descriptor. */
#define SFTP_DRAG_ITEMS_MAX		10000U

/* Maximum total byte size allowed for synchronous (non-async) drop targets. */
#define SFTP_DRAG_SYNC_LIMIT_BYTES	(16ULL * 1024ULL * 1024ULL)

/* One top-level selected remote item to drag out. */
struct sftp_drag_item {
	char remote_path[SSH_SFTP_PATH_MAX];
	char name[SSH_SFTP_PATH_MAX];
	uint64_t size;
	uint32_t mtime;
	int is_dir;
	int is_link;
};

struct sftp_drag_context;
struct IDataObject;

struct sftp_drag_context *sftp_drag_context_create(struct sftp_worker *worker, void (*notify)(void *data, const char *error_text), void *notify_data);
void sftp_drag_context_detach(struct sftp_drag_context *ctx);
size_t sftp_drag_context_active_count(struct sftp_drag_context *ctx);
void sftp_drag_context_cancel_all(struct sftp_drag_context *ctx);

int sftp_drag_create_object(struct sftp_drag_context *ctx, const struct sftp_drag_item *items, size_t count, struct IDataObject **object_out);
int sftp_drag_run(struct sftp_drag_context *ctx, const struct sftp_drag_item *items, size_t count);

#endif
