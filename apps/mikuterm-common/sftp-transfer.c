/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transfer queue and pipelined file transfer state machine.
 *
 * Transfers run one file at a time, pipelining up to SFTP_CLIENT_REQUESTS
 * (or SFTP_TRANSFER_SHARED_REQUESTS when an Explorer stream read is active)
 * 32 KiB READ or WRITE requests.  Downloads write to a hidden temporary
 * file and replace the target only on completion; uploads truncate the
 * target directly and remove newly created files on cancellation.
 */

#include "sftp-transfer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal phase of the currently active transfer item. */
enum xfer_phase {
	XFER_NONE = 0,
	XFER_DL_STAT,
	XFER_DL_WAIT_CONFLICT,
	XFER_DL_OPEN,
	XFER_DL_DATA,
	XFER_DL_CLOSE,
	XFER_DL_OPENDIR,
	XFER_DL_READDIR,
	XFER_DL_CLOSEDIR,
	XFER_UL_STAT,
	XFER_UL_WAIT_CONFLICT,
	XFER_UL_OPEN,
	XFER_UL_DATA,
	XFER_UL_FSETSTAT,
	XFER_UL_CLOSE,
	XFER_UL_CLEANUP,
	XFER_UL_MKDIR
};

/* One queued or completed transfer item. */
struct xfer_item {
	uint64_t id;
	enum sftp_transfer_dir direction;
	enum sftp_transfer_state state;
	int is_dir;
	char local_path[MIKUTERM_PATH_MAX];
	char local_dir[MIKUTERM_PATH_MAX];
	char remote_path[SSH_SFTP_PATH_MAX];
	char remote_dir[SSH_SFTP_PATH_MAX];
	uint64_t bytes_done;
	uint64_t bytes_total;
	uint64_t conflict_remote_size;
	uint32_t conflict_remote_mtime;
	uint64_t conflict_local_size;
	uint32_t conflict_local_mtime;
	char error_text[256];
};

/* One in-flight READ or WRITE request. */
struct xfer_inflight {
	int used;
	uint32_t id;
	uint64_t offset;
	uint32_t length;
};

/* One short-read follow-up range queued for re-issue. */
struct xfer_retry_range {
	int used;
	uint64_t offset;
	uint32_t length;
};

/*
 * Pipelined SFTP transfer queue.
 *
 * Tracks queued, waiting-overwrite, active, and completed file/folder
 * transfers and drives one active item at a time over sftp_client.
 */
struct sftp_transfer_queue {
	int preserve_times;
	enum sftp_overwrite_policy policy;
	uint64_t next_id;

	struct xfer_item *items;
	size_t count;
	size_t capacity;

	/* Active item state. */
	size_t active_index;
	int has_active;
	enum xfer_phase phase;
	uint32_t pending_req_id;
	int has_pending_req;
	int cancel_requested;
	int failed;
	int existed;
	int eof_seen;
	uint64_t eof_offset;
	uint64_t next_offset;
	uint32_t remote_mtime;
	uint32_t local_mtime;
	uint8_t handle[SFTP_WORKER_HANDLE_MAX];
	size_t handle_size;
	struct sftp_local_file *local_file;
	char temp_path[MIKUTERM_PATH_MAX];
	struct xfer_inflight inflight[SFTP_CLIENT_REQUESTS];
	unsigned inflight_count;
	struct xfer_retry_range retries[SFTP_CLIENT_REQUESTS];
};

static const char *path_base(const char *path);
static void path_parent(const char *path, char *out, size_t cap);
static int valid_utf8(const char *s, size_t len);
static int is_reserved_win32_name(const char *name, size_t len);
static int is_8_3_tilde_name(const char *name, size_t len);
static int ascii_icmp_n(const char *a, const char *b, size_t n);
static void start_next_item(struct sftp_transfer_queue *q, struct sftp_client *client, int *dirty);
static void finish_active(struct sftp_transfer_queue *q, enum sftp_transfer_state state, const char *err, int *dirty);
static void pump_dl_data(struct sftp_transfer_queue *q, struct sftp_client *client, unsigned max_requests, int *dirty);
static void pump_ul_data(struct sftp_transfer_queue *q, struct sftp_client *client, unsigned max_requests, int *dirty);
static int add_inflight(struct sftp_transfer_queue *q, uint32_t id, uint64_t offset, uint32_t length);
static int take_inflight(struct sftp_transfer_queue *q, uint32_t id, uint64_t *offset, uint32_t *length);
static void add_retry_range(struct sftp_transfer_queue *q, uint64_t offset, uint32_t length);
static int take_retry_range(struct sftp_transfer_queue *q, uint64_t *offset, uint32_t *length);
static void begin_dl_open(struct sftp_transfer_queue *q, struct sftp_client *client, int *dirty);
static void begin_ul_open(struct sftp_transfer_queue *q, struct sftp_client *client, int *dirty);
static void begin_close_handle(struct sftp_transfer_queue *q, struct sftp_client *client, enum xfer_phase next_phase, int *dirty);
static void expand_local_dir(struct sftp_transfer_queue *q, const struct xfer_item *parent);

/* Creates a transfer queue. */
int
sftp_transfer_queue_create(
	int preserve_times,
	struct sftp_transfer_queue **queue_out)
{
	struct sftp_transfer_queue *q;

	/* Allocates and initializes the transfer queue. */
	*queue_out = NULL;
	q = calloc(1U, sizeof(*q));
	if (q == NULL)
		return ENOMEM;
	q->preserve_times = preserve_times;
	q->next_id = 1U;
	*queue_out = q;

	/* Succeeded: returns the new transfer queue. */
	return 0;
}

/*
 * Destroys a transfer queue and closes any open local file.
 */
void
sftp_transfer_queue_destroy(
	struct sftp_transfer_queue *q)
{
	/* Ignores a NULL queue pointer. */
	if (q == NULL)
		return;

	/* Closes any open local file before freeing the item array. */
	if (q->local_file != NULL) {
		sftp_local_close(q->local_file);
		q->local_file = NULL;
	}

	/* Frees the items array and queue structure. */
	free(q->items);
	free(q);
}

/*
 * Enqueues a file or folder transfer item.
 */
int
sftp_transfer_enqueue(
	struct sftp_transfer_queue *q,
	enum sftp_transfer_dir dir,
	const char *local_path,
	const char *remote_path,
	int is_dir,
	uint64_t *id_out)
{
	struct xfer_item *grown;
	struct xfer_item *item;
	size_t new_cap;

	/* Grows the item array when at capacity. */
	if (q->count == q->capacity) {
		if (q->capacity == 0U)
			new_cap = 16U;
		else
			new_cap = q->capacity * 2U;
		grown = realloc(q->items, new_cap * sizeof(*q->items));
		if (grown == NULL)
			return ENOMEM;
		q->items = grown;
		q->capacity = new_cap;
	}

	/* Initializes the new queued transfer item. */
	item = &q->items[q->count++];
	memset(item, 0, sizeof(*item));
	item->id = q->next_id++;
	item->direction = dir;
	item->state = SFTP_TRANSFER_QUEUED;
	item->is_dir = is_dir;
	(void)snprintf(item->local_path, sizeof(item->local_path), "%s", local_path);
	path_parent(local_path, item->local_dir, sizeof(item->local_dir));
	(void)snprintf(item->remote_path, sizeof(item->remote_path), "%s", remote_path);
	path_parent(remote_path, item->remote_dir, sizeof(item->remote_dir));

	/* Succeeded: reports the assigned transfer id if requested. */
	if (id_out != NULL)
		*id_out = item->id;
	return 0;
}

/*
 * Applies the user's overwrite decision to a waiting transfer item.
 */
void
sftp_transfer_resolve_conflict(
	struct sftp_transfer_queue *q,
	uint64_t id,
	enum sftp_conflict_action action)
{
	struct xfer_item *item;

	/* Ignores the decision if no matching item is waiting for overwrite input. */
	if (!q->has_active)
		return;
	item = &q->items[q->active_index];
	if (item->id != id || item->state != SFTP_TRANSFER_WAITING_OVERWRITE)
		return;

	/* Applies the chosen conflict action and updates batch policy if requested. */
	switch (action) {
	case SFTP_CONFLICT_OVERWRITE_ALL:
		q->policy = SFTP_OVERWRITE_ALL;
		/* FALLTHROUGH */
	case SFTP_CONFLICT_OVERWRITE:
		item->state = SFTP_TRANSFER_ACTIVE;
		break;
	case SFTP_CONFLICT_SKIP_ALL:
		q->policy = SFTP_SKIP_ALL;
		/* FALLTHROUGH */
	case SFTP_CONFLICT_SKIP:
		if (q->local_file != NULL) {
			sftp_local_close(q->local_file);
			q->local_file = NULL;
		}

		/* Marks the item skipped and clears active state. */
		item->state = SFTP_TRANSFER_SKIPPED;
		q->has_active = 0;
		q->phase = XFER_NONE;
		break;
	case SFTP_CONFLICT_CANCEL:
		sftp_transfer_cancel_all(q);
		break;
	}
}

/*
 * Cancels a queued or active transfer item.
 */
void
sftp_transfer_cancel(
	struct sftp_transfer_queue *q,
	uint64_t id)
{
	size_t i;

	/* Finds the matching item and cancels it immediately if not in flight. */
	for (i = 0U; i < q->count; i++) {
		if (q->items[i].id != id)
			continue;
		if (q->has_active && q->active_index == i) {
			q->cancel_requested = 1;
			if (q->items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE) {
				if (q->local_file != NULL) {
					sftp_local_close(q->local_file);
					q->local_file = NULL;
				}

				/* Marks the waiting item cancelled and clears active state. */
				q->items[i].state = SFTP_TRANSFER_CANCELLED;
				q->has_active = 0;
				q->phase = XFER_NONE;
			}
		} else if (q->items[i].state == SFTP_TRANSFER_QUEUED) {
			q->items[i].state = SFTP_TRANSFER_CANCELLED;
		}

		/* Stops after handling the target id. */
		break;
	}
}

/*
 * Cancels all queued and active transfer items.
 */
void
sftp_transfer_cancel_all(
	struct sftp_transfer_queue *q)
{
	size_t i;

	/* Cancels every queued item and marks any active item for cancellation. */
	for (i = 0U; i < q->count; i++) {
		if (q->has_active && q->active_index == i) {
			q->cancel_requested = 1;
			if (q->items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE) {
				if (q->local_file != NULL) {
					sftp_local_close(q->local_file);
					q->local_file = NULL;
				}

				/* Marks the waiting item cancelled and clears active state. */
				q->items[i].state = SFTP_TRANSFER_CANCELLED;
				q->has_active = 0;
				q->phase = XFER_NONE;
			}
		} else if (q->items[i].state == SFTP_TRANSFER_QUEUED) {
			q->items[i].state = SFTP_TRANSFER_CANCELLED;
		}
	}
}

/*
 * Copies a snapshot of all transfer items for the UI.
 */
int
sftp_transfer_snapshot_all(
	const struct sftp_transfer_queue *q,
	struct sftp_transfer_snapshot **items_out,
	size_t *count_out,
	size_t *active_or_queued_out)
{
	struct sftp_transfer_snapshot *snap;
	size_t i;
	size_t active_count;

	/* Clears output pointers and returns early if the queue is empty. */
	*items_out = NULL;
	*count_out = 0U;
	if (active_or_queued_out != NULL)
		*active_or_queued_out = 0U;
	if (q->count == 0U)
		return 0;

	/* Allocates the snapshot array. */
	snap = calloc(q->count, sizeof(*snap));
	if (snap == NULL)
		return ENOMEM;

	/* Copies each transfer item and counts active or queued entries. */
	active_count = 0U;
	for (i = 0U; i < q->count; i++) {
		snap[i].id = q->items[i].id;
		snap[i].direction = q->items[i].direction;
		snap[i].state = q->items[i].state;
		(void)snprintf(snap[i].local_path, sizeof(snap[i].local_path), "%s", q->items[i].local_path);
		(void)snprintf(snap[i].remote_path, sizeof(snap[i].remote_path), "%s", q->items[i].remote_path);
		snap[i].bytes_done = q->items[i].bytes_done;
		snap[i].bytes_total = q->items[i].bytes_total;
		snap[i].conflict_remote_size = q->items[i].conflict_remote_size;
		snap[i].conflict_remote_mtime = q->items[i].conflict_remote_mtime;
		snap[i].conflict_local_size = q->items[i].conflict_local_size;
		snap[i].conflict_local_mtime = q->items[i].conflict_local_mtime;
		(void)snprintf(snap[i].error_text, sizeof(snap[i].error_text), "%s", q->items[i].error_text);
		if (q->items[i].state == SFTP_TRANSFER_QUEUED ||
		    q->items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE ||
		    q->items[i].state == SFTP_TRANSFER_ACTIVE)
			active_count++;
	}

	/* Succeeded: publishes the snapshot array and counts. */
	*items_out = snap;
	*count_out = q->count;
	if (active_or_queued_out != NULL)
		*active_or_queued_out = active_count;
	return 0;
}

/*
 * Returns 1 if any transfer is active or queued.
 */
int
sftp_transfer_has_work(
	const struct sftp_transfer_queue *q)
{
	size_t i;

	/* Checks the active item and scans for any queued item. */
	if (q->has_active)
		return 1;
	for (i = 0U; i < q->count; i++) {
		if (q->items[i].state == SFTP_TRANSFER_QUEUED)
			return 1;
	}

	/* No active or queued transfers remain. */
	return 0;
}

/*
 * Advances the active transfer or starts the next queued item.
 */
void
sftp_transfer_step(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	unsigned max_requests,
	int *dirty)
{
	/* Starts the next queued item when idle. */
	if (!q->has_active) {
		start_next_item(q, client, dirty);
		return;
	}

	/* Resumes download after overwrite resolution. */
	if (q->phase == XFER_DL_WAIT_CONFLICT) {
		if (q->items[q->active_index].state == SFTP_TRANSFER_ACTIVE)
			begin_dl_open(q, client, dirty);
		return;
	}

	/* Resumes upload after overwrite resolution. */
	if (q->phase == XFER_UL_WAIT_CONFLICT) {
		if (q->items[q->active_index].state == SFTP_TRANSFER_ACTIVE)
			begin_ul_open(q, client, dirty);
		return;
	}

	/* Pumps pipelined download READ requests. */
	if (q->phase == XFER_DL_DATA) {
		pump_dl_data(q, client, max_requests, dirty);
		return;
	}

	/* Pumps pipelined upload WRITE requests. */
	if (q->phase == XFER_UL_DATA) {
		pump_ul_data(q, client, max_requests, dirty);
		return;
	}
}

/* Processes an SFTP reply if it belongs to the active transfer. */
int
sftp_transfer_handle_reply(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	const struct sftp_client_reply *reply,
	int *taken,
	int *dirty)
{
	struct xfer_item *item;
	struct sftp_local_entry lentry;
	uint64_t offset;
	uint32_t length;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *long_name;
	size_t long_size;
	struct ssh_sftp_attrs attrs;
	char entry_name[SSH_SFTP_PATH_MAX];
	char local_child[MIKUTERM_PATH_MAX];
	char child_remote[SSH_SFTP_PATH_MAX];
	const char *why;
	const char *msg;
	enum sftp_transfer_state final_state;
	uint64_t skip_id;
	int entry_is_dir;
	int entry_is_link;
	int cmp_dot;
	int cmp_dotdot;
	int valid_name;
	int error;

	/* Ignores replies when no transfer is active. */
	*taken = 0;
	if (!q->has_active)
		return 0;
	item = &q->items[q->active_index];

	/* Handles pipelined READ/WRITE replies during data transfer phases. */
	if (q->phase == XFER_DL_DATA || q->phase == XFER_UL_DATA) {
		error = take_inflight(q, reply->id, &offset, &length);
		if (error != 0)
			return 0;
		*taken = 1;

		/* Processes a download READ reply. */
		if (q->phase == XFER_DL_DATA) {
			if (reply->error != 0) {
				q->failed = 1;
				(void)snprintf(item->error_text, sizeof(item->error_text), "Malformed SFTP DATA reply.");
			} else if (reply->type == SSH_FXP_DATA) {
				if (reply->data_size != 0U) {
					error = sftp_local_write_at(q->local_file, offset, reply->data, reply->data_size);
					if (error != 0) {
						q->failed = 1;
						(void)snprintf(item->error_text, sizeof(item->error_text), "Local write failed (errno %d).", error);
					} else {
						item->bytes_done += reply->data_size;
						*dirty = 1;
					}
				}

				/* Re-queues a short read or marks EOF on a zero-byte read. */
				if (!q->failed && !q->cancel_requested && !q->eof_seen && reply->data_size < length) {
					if (reply->data_size == 0U) {
						q->eof_seen = 1;
						q->eof_offset = offset;
					} else {
						add_retry_range(q, offset + reply->data_size, (uint32_t)(length - reply->data_size));
					}
				}
			} else if (reply->type == SSH_FXP_STATUS) {
				if (reply->code == SSH_FX_EOF) {
					if (!q->eof_seen || offset < q->eof_offset)
						q->eof_offset = offset;
					q->eof_seen = 1;
				} else {
					q->failed = 1;
					(void)snprintf(item->error_text, sizeof(item->error_text), "Remote read failed (status %u).", reply->code);
				}
			}

			/* Download DATA reply handled. */
			return 0;
		}

		/* Processes an upload WRITE reply (XFER_UL_DATA). */
		if (reply->error != 0 || reply->type != SSH_FXP_STATUS || reply->code != SSH_FX_OK) {
			q->failed = 1;
			(void)snprintf(item->error_text, sizeof(item->error_text), "Remote write failed (status %u).", reply->code);
		} else {
			item->bytes_done += length;
			*dirty = 1;
		}

		/* Upload WRITE reply handled. */
		return 0;
	}

	/* Matches single-request control phases by pending_req_id. */
	if (!q->has_pending_req || reply->id != q->pending_req_id)
		return 0;
	*taken = 1;
	q->has_pending_req = 0;

	/* Advances the active transfer state machine for the completed request. */
	switch (q->phase) {
	case XFER_DL_STAT:
		if (reply->error != 0 || reply->type != SSH_FXP_ATTRS) {
			finish_active(q, SFTP_TRANSFER_FAILED, "Remote file could not be inspected.", dirty);
			return 0;
		}

		/* Creates the local folder and opens the remote directory if downloading a folder. */
		if ((reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
		    (reply->attrs.permissions & 0170000U) == 0040000U) {
			item->is_dir = 1;
			error = sftp_local_mkdir(item->local_path);
			if (error != 0) {
				finish_active(q, SFTP_TRANSFER_FAILED, "Local directory could not be created.", dirty);
				return 0;
			}

			/* Opens the remote directory to enumerate its children. */
			error = sftp_client_opendir(client, (const uint8_t *)item->remote_path, strlen(item->remote_path), &q->pending_req_id);
			if (error != 0) {
				finish_active(q, SFTP_TRANSFER_FAILED, "Could not open remote directory.", dirty);
				return 0;
			}

			/* Waits for OPENDIR completion. */
			q->has_pending_req = 1;
			q->phase = XFER_DL_OPENDIR;
			return 0;
		}

		/* Records remote file size and modification time. */
		if ((reply->attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0U)
			item->bytes_total = reply->attrs.size;
		if ((reply->attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0U)
			q->remote_mtime = reply->attrs.mtime;

		/* Checks whether the local destination already exists. */
		error = sftp_local_stat(item->local_path, &lentry);
		if (error == 0) {
			if (lentry.is_dir) {
				finish_active(q, SFTP_TRANSFER_FAILED, "Destination is a directory.", dirty);
				return 0;
			}

			/* Skips existing files under SKIP_ALL policy. */
			if (q->policy == SFTP_SKIP_ALL) {
				finish_active(q, SFTP_TRANSFER_SKIPPED, "Skipped existing file.", dirty);
				return 0;
			}

			/* Pauses for user overwrite confirmation under OVERWRITE_ASK policy. */
			if (q->policy == SFTP_OVERWRITE_ASK) {
				item->conflict_remote_size = item->bytes_total;
				item->conflict_remote_mtime = q->remote_mtime;
				item->conflict_local_size = lentry.size;
				item->conflict_local_mtime = lentry.mtime;
				item->state = SFTP_TRANSFER_WAITING_OVERWRITE;
				q->phase = XFER_DL_WAIT_CONFLICT;
				*dirty = 1;
				return 0;
			}
		}

		/* Opens the local temporary file and remote file for reading. */
		begin_dl_open(q, client, dirty);
		return 0;

	case XFER_DL_OPEN:
		if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(q->handle)) {
			if (q->local_file != NULL) {
				sftp_local_close(q->local_file);
				q->local_file = NULL;
				(void)sftp_local_remove(q->temp_path);
			}

			/* Fails the transfer when the remote file could not be opened. */
			finish_active(q, SFTP_TRANSFER_FAILED, "Remote file could not be opened.", dirty);
			return 0;
		}

		/* Saves the handle and starts pipelined download reads. */
		memcpy(q->handle, reply->handle, reply->handle_size);
		q->handle_size = reply->handle_size;
		q->phase = XFER_DL_DATA;
		return 0;

	case XFER_DL_CLOSE:
		if (q->local_file != NULL) {
			if (!q->failed && !q->cancel_requested && q->preserve_times && q->remote_mtime != 0U)
				(void)sftp_local_set_mtime(q->local_file, q->remote_mtime);
			sftp_local_close(q->local_file);
			q->local_file = NULL;
		}

		/* Commits the temporary file on success, or removes it on cancel/failure. */
		if (q->cancel_requested) {
			(void)sftp_local_remove(q->temp_path);
			finish_active(q, SFTP_TRANSFER_CANCELLED, "Cancelled.", dirty);
		} else if (q->failed) {
			(void)sftp_local_remove(q->temp_path);
			finish_active(q, SFTP_TRANSFER_FAILED, item->error_text, dirty);
		} else {
			error = sftp_local_commit_temp(q->temp_path, item->local_path);
			if (error != 0) {
				(void)sftp_local_remove(q->temp_path);
				finish_active(q, SFTP_TRANSFER_FAILED, "Could not replace local file.", dirty);
			} else {
				finish_active(q, SFTP_TRANSFER_DONE, "", dirty);
			}
		}

		/* Download CLOSE complete. */
		return 0;

	case XFER_DL_OPENDIR:
		if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(q->handle)) {
			finish_active(q, SFTP_TRANSFER_FAILED, "Could not open remote directory.", dirty);
			return 0;
		}

		/* Saves the directory handle and requests the first READDIR batch. */
		memcpy(q->handle, reply->handle, reply->handle_size);
		q->handle_size = reply->handle_size;
		error = sftp_client_readdir(client, q->handle, q->handle_size, &q->pending_req_id);
		if (error != 0) {
			begin_close_handle(q, client, XFER_DL_CLOSEDIR, dirty);
			return 0;
		}

		/* Waits for READDIR completion. */
		q->has_pending_req = 1;
		q->phase = XFER_DL_READDIR;
		return 0;

	case XFER_DL_READDIR:
		if (reply->type == SSH_FXP_STATUS) {
			begin_close_handle(q, client, XFER_DL_CLOSEDIR, dirty);
			return 0;
		}

		/* Closes the handle with failure on malformed directory replies. */
		if (reply->error != 0 || reply->type != SSH_FXP_NAME) {
			q->failed = 1;
			begin_close_handle(q, client, XFER_DL_CLOSEDIR, dirty);
			return 0;
		}

		/* Enqueues each valid non-symlink child entry for download. */
		for (;;) {
			error = sftp_client_name_next(client, &name, &name_size, &long_name, &long_size, &attrs);
			if (error != 0)
				break;
			if (name_size == 0U || name_size >= sizeof(entry_name))
				continue;
			memcpy(entry_name, name, name_size);
			entry_name[name_size] = '\0';
			cmp_dot = strcmp(entry_name, ".");
			cmp_dotdot = strcmp(entry_name, "..");
			if (cmp_dot == 0 || cmp_dotdot == 0)
				continue;

			/* Never traverse symlinked entries during recursive folder download. */
			entry_is_dir = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (attrs.permissions & 0170000U) == 0040000U;
			entry_is_link = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (attrs.permissions & 0170000U) == 0120000U;
			if (entry_is_link)
				continue;

			/* Validates the child filename for Windows and records skipped items. */
			valid_name = sftp_transfer_valid_download_name(entry_name, name_size, &why);
			if (!valid_name) {
				error = sftp_transfer_join_remote(child_remote, sizeof(child_remote), item->remote_path, entry_name);
				if (error == 0)
					error = sftp_local_join_path(local_child, sizeof(local_child), item->local_path, entry_name);
				if (error == 0)
					error = sftp_transfer_enqueue(q, SFTP_TRANSFER_DOWNLOAD, local_child, child_remote, entry_is_dir, &skip_id);
				if (error == 0) {
					q->items[q->count - 1U].state = SFTP_TRANSFER_SKIPPED;
					(void)snprintf(q->items[q->count - 1U].error_text, sizeof(q->items[q->count - 1U].error_text), "%s", why);
				}

				/* Continues scanning remaining entries. */
				continue;
			}

			/* Enqueues valid child items for download. */
			error = sftp_transfer_join_remote(child_remote, sizeof(child_remote), item->remote_path, entry_name);
			if (error == 0)
				error = sftp_local_join_path(local_child, sizeof(local_child), item->local_path, entry_name);
			if (error == 0)
				(void)sftp_transfer_enqueue(q, SFTP_TRANSFER_DOWNLOAD, local_child, child_remote, entry_is_dir, NULL);
		}

		/* Closes the directory handle if cancelled, otherwise requests the next batch. */
		if (q->cancel_requested) {
			begin_close_handle(q, client, XFER_DL_CLOSEDIR, dirty);
			return 0;
		}

		/* Issues the next READDIR request. */
		error = sftp_client_readdir(client, q->handle, q->handle_size, &q->pending_req_id);
		if (error != 0) {
			q->failed = 1;
			begin_close_handle(q, client, XFER_DL_CLOSEDIR, dirty);
			return 0;
		}

		/* Waits for the next READDIR reply. */
		q->has_pending_req = 1;
		return 0;

	case XFER_DL_CLOSEDIR:
		if (q->cancel_requested)
			finish_active(q, SFTP_TRANSFER_CANCELLED, "Cancelled.", dirty);
		else if (q->failed)
			finish_active(q, SFTP_TRANSFER_FAILED, "Could not list remote directory.", dirty);
		else
			finish_active(q, SFTP_TRANSFER_DONE, "", dirty);
		return 0;

	case XFER_UL_MKDIR:
		/* Proceed to list and enqueue children even if the directory already existed. */
		if (q->cancel_requested) {
			finish_active(q, SFTP_TRANSFER_CANCELLED, "Cancelled.", dirty);
			return 0;
		}

		/* Expands local children and completes the directory upload item. */
		expand_local_dir(q, item);
		finish_active(q, SFTP_TRANSFER_DONE, "", dirty);
		return 0;

	case XFER_UL_STAT:
		q->existed = 0;
		if (reply->type == SSH_FXP_ATTRS && reply->error == 0) {
			if ((reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (reply->attrs.permissions & 0170000U) == 0040000U) {
				sftp_local_close(q->local_file);
				q->local_file = NULL;
				finish_active(q, SFTP_TRANSFER_FAILED, "Destination is a directory.", dirty);
				return 0;
			}

			/* Records that the remote file existed and applies overwrite policy. */
			q->existed = 1;
			if (q->policy == SFTP_SKIP_ALL) {
				sftp_local_close(q->local_file);
				q->local_file = NULL;
				finish_active(q, SFTP_TRANSFER_SKIPPED, "Skipped existing file.", dirty);
				return 0;
			}

			/* Pauses for user overwrite confirmation under OVERWRITE_ASK policy. */
			if (q->policy == SFTP_OVERWRITE_ASK) {
				if ((reply->attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0U)
					item->conflict_remote_size = reply->attrs.size;
				if ((reply->attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0U)
					item->conflict_remote_mtime = reply->attrs.mtime;
				item->conflict_local_size = item->bytes_total;
				item->conflict_local_mtime = q->local_mtime;
				item->state = SFTP_TRANSFER_WAITING_OVERWRITE;
				q->phase = XFER_UL_WAIT_CONFLICT;
				*dirty = 1;
				return 0;
			}
		}

		/* Opens the remote file for writing. */
		begin_ul_open(q, client, dirty);
		return 0;

	case XFER_UL_OPEN:
		if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(q->handle)) {
			sftp_local_close(q->local_file);
			q->local_file = NULL;
			finish_active(q, SFTP_TRANSFER_FAILED, "Remote file could not be opened for writing.", dirty);
			return 0;
		}

		/* Saves the handle and starts pipelined upload writes. */
		memcpy(q->handle, reply->handle, reply->handle_size);
		q->handle_size = reply->handle_size;
		q->phase = XFER_UL_DATA;
		return 0;

	case XFER_UL_FSETSTAT:
		begin_close_handle(q, client, XFER_UL_CLOSE, dirty);
		return 0;

	case XFER_UL_CLOSE:
		if (q->local_file != NULL) {
			sftp_local_close(q->local_file);
			q->local_file = NULL;
		}

		/* Removes newly created files on cancellation or failure. */
		if (q->cancel_requested || q->failed) {
			if (!q->existed) {
				error = sftp_client_remove(client, (const uint8_t *)item->remote_path, strlen(item->remote_path), &q->pending_req_id);
				if (error == 0) {
					q->has_pending_req = 1;
					q->phase = XFER_UL_CLEANUP;
					return 0;
				}
			}

			/* Reports the final cancelled or failed state. */
			if (q->cancel_requested)
				final_state = SFTP_TRANSFER_CANCELLED;
			else
				final_state = SFTP_TRANSFER_FAILED;
			if (q->existed)
				finish_active(q, final_state, "partially written", dirty);
			else
				finish_active(q, final_state, item->error_text, dirty);
		} else {
			finish_active(q, SFTP_TRANSFER_DONE, "", dirty);
		}

		/* Upload CLOSE complete. */
		return 0;

	case XFER_UL_CLEANUP:
		if (q->cancel_requested) {
			final_state = SFTP_TRANSFER_CANCELLED;
			msg = "Cancelled.";
		} else {
			final_state = SFTP_TRANSFER_FAILED;
			msg = item->error_text;
		}

		/* Completes the upload after cleanup. */
		finish_active(q, final_state, msg, dirty);
		return 0;

	default:
		return 0;
	}
}

/*
 * Aborts the active transfer and marks all remaining items failed when the
 * channel closes.
 */
void
sftp_transfer_abort_active(
	struct sftp_transfer_queue *q,
	const char *reason,
	int *dirty)
{
	size_t i;

	/* Closes any open local file and removes incomplete download temps. */
	if (q->local_file != NULL) {
		sftp_local_close(q->local_file);
		q->local_file = NULL;
		if (q->has_active && q->items[q->active_index].direction == SFTP_TRANSFER_DOWNLOAD &&
		    q->temp_path[0] != '\0')
			(void)sftp_local_remove(q->temp_path);
	}

	/* Fails the active item if one was running. */
	if (q->has_active)
		finish_active(q, SFTP_TRANSFER_FAILED, reason, dirty);

	/* Marks all remaining queued or waiting items as failed. */
	for (i = 0U; i < q->count; i++) {
		if (q->items[i].state == SFTP_TRANSFER_QUEUED ||
		    q->items[i].state == SFTP_TRANSFER_WAITING_OVERWRITE) {
			q->items[i].state = SFTP_TRANSFER_FAILED;
			(void)snprintf(q->items[i].error_text, sizeof(q->items[i].error_text), "%s", reason);
			*dirty = 1;
		}
	}
}

/*
 * Validates a remote filename for safe creation on Windows: checks
 * UTF-8, path separators, ADS colons, forbidden characters, trailing
 * dots or spaces, reserved DOS device names, and 8.3 short-name patterns.
 */
int
sftp_transfer_valid_download_name(
	const char *name,
	size_t size,
	const char **why)
{
	size_t i;
	unsigned char c;
	int ok;

	/* Rejects empty names and dot entries. */
	if (size == 0U || (size == 1U && name[0] == '.') ||
	    (size == 2U && name[0] == '.' && name[1] == '.')) {
		*why = "invalid filename";
		return 0;
	}

	/* Rejects invalid UTF-8 sequences. */
	ok = valid_utf8(name, size);
	if (!ok) {
		*why = "invalid UTF-8 filename";
		return 0;
	}

	/* Rejects control characters and Windows-forbidden path characters. */
	for (i = 0U; i < size; i++) {
		c = (unsigned char)name[i];
		if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\' ||
		    c == ':' || c == '*' || c == '?' || c == '"' ||
		    c == '<' || c == '>' || c == '|') {
			*why = "filename contains characters forbidden on Windows";
			return 0;
		}
	}

	/* Rejects trailing dots or spaces. */
	if (name[size - 1U] == '.' || name[size - 1U] == ' ') {
		*why = "filename ends with a dot or space";
		return 0;
	}

	/* Rejects reserved DOS device names. */
	ok = is_reserved_win32_name(name, size);
	if (ok) {
		*why = "filename is a reserved Windows name";
		return 0;
	}

	/* Rejects Windows 8.3 tilde short names. */
	ok = is_8_3_tilde_name(name, size);
	if (ok) {
		*why = "filename resembles a Windows 8.3 short name";
		return 0;
	}

	/* Succeeded: filename is safe on Windows. */
	*why = NULL;
	return 1;
}

/*
 * Joins dir and name with a single '/' for remote SFTP paths.
 */
int
sftp_transfer_join_remote(
	char *out,
	size_t out_cap,
	const char *dir,
	const char *name)
{
	size_t len;
	int written;

	/* Formats the joined remote path without duplicating a trailing slash. */
	len = strlen(dir);
	if (len != 0U && dir[len - 1U] == '/')
		written = snprintf(out, out_cap, "%s%s", dir, name);
	else if (len == 0U)
		written = snprintf(out, out_cap, "%s", name);
	else
		written = snprintf(out, out_cap, "%s/%s", dir, name);

	/* Reports ENAMETOOLONG if the joined path did not fit. */
	if (written < 0 || (size_t)written >= out_cap)
		return ENAMETOOLONG;

	/* Succeeded: out holds the joined remote path. */
	return 0;
}

/* Returns the final path component. */
static const char *
path_base(
	const char *path)
{
	const char *slash;
	const char *bslash;
	const char *last;

	/* Finds the rightmost forward or backward slash. */
	slash = strrchr(path, '/');
	bslash = strrchr(path, '\\');
	last = slash;
	if (bslash != NULL && (last == NULL || bslash > last))
		last = bslash;
	if (last != NULL)
		return last + 1;
	return path;
}

/* Copies the parent directory of path into out. */
static void
path_parent(
	const char *path,
	char *out,
	size_t cap)
{
	const char *base;
	size_t len;

	/* Returns empty string when path has no directory separator. */
	base = path_base(path);
	if (base == path) {
		out[0] = '\0';
		return;
	}

	/* Strips the trailing separator and copies the parent prefix. */
	len = (size_t)(base - path);
	if (len > 1U && (path[len - 1U] == '/' || path[len - 1U] == '\\'))
		len--;
	if (len >= cap)
		len = cap - 1U;
	memcpy(out, path, len);
	out[len] = '\0';
}

/* Checks strict UTF-8 encoding. */
static int
valid_utf8(
	const char *s,
	size_t len)
{
	size_t i;
	unsigned char b0;
	unsigned char b1;
	unsigned char b2;
	unsigned char b3;
	uint32_t cp;

	/* Validates each UTF-8 code point sequence and rejects overlongs and surrogates. */
	i = 0U;
	while (i < len) {
		b0 = (unsigned char)s[i];
		if (b0 < 0x80U) {
			i++;
			continue;
		}

		/* Two-byte UTF-8 sequence. */
		if (b0 >= 0xc2U && b0 <= 0xdfU) {
			if (i + 1U >= len)
				return 0;
			b1 = (unsigned char)s[i + 1U];
			if ((b1 & 0xc0U) != 0x80U)
				return 0;
			i += 2U;
			continue;
		}

		/* Three-byte UTF-8 sequence. */
		if (b0 >= 0xe0U && b0 <= 0xefU) {
			if (i + 2U >= len)
				return 0;
			b1 = (unsigned char)s[i + 1U];
			b2 = (unsigned char)s[i + 2U];
			if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U)
				return 0;
			cp = ((uint32_t)(b0 & 0x0fU) << 12) | ((uint32_t)(b1 & 0x3fU) << 6) | (uint32_t)(b2 & 0x3fU);
			if (cp < 0x800U || (cp >= 0xd800U && cp <= 0xdfffU))
				return 0;
			i += 3U;
			continue;
		}

		/* Four-byte UTF-8 sequence. */
		if (b0 >= 0xf0U && b0 <= 0xf4U) {
			if (i + 3U >= len)
				return 0;
			b1 = (unsigned char)s[i + 1U];
			b2 = (unsigned char)s[i + 2U];
			b3 = (unsigned char)s[i + 3U];
			if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U || (b3 & 0xc0U) != 0x80U)
				return 0;
			cp = ((uint32_t)(b0 & 0x07U) << 18) | ((uint32_t)(b1 & 0x3fU) << 12) |
			    ((uint32_t)(b2 & 0x3fU) << 6) | (uint32_t)(b3 & 0x3fU);
			if (cp < 0x10000U || cp > 0x10ffffU)
				return 0;
			i += 4U;
			continue;
		}

		/* Invalid leading UTF-8 byte. */
		return 0;
	}

	/* Valid UTF-8 string. */
	return 1;
}

/* Case-insensitive ASCII comparison of n bytes. */
static int
ascii_icmp_n(
	const char *a,
	const char *b,
	size_t n)
{
	size_t i;
	unsigned char ca;
	unsigned char cb;

	/* Compares each byte after folding ASCII uppercase to lowercase. */
	for (i = 0U; i < n; i++) {
		ca = (unsigned char)a[i];
		cb = (unsigned char)b[i];
		if (ca >= 'A' && ca <= 'Z')
			ca = (unsigned char)(ca + ('a' - 'A'));
		if (cb >= 'A' && cb <= 'Z')
			cb = (unsigned char)(cb + ('a' - 'A'));
		if (ca != cb)
			return (int)ca - (int)cb;
	}

	/* Equal up to n bytes. */
	return 0;
}

/* Checks whether the stem (before the first '.') is a reserved Windows device name. */
static int
is_reserved_win32_name(
	const char *name,
	size_t len)
{
	static const char *const fixed[] = {
		"con", "prn", "aux", "nul", "conin$", "conout$"
	};
	size_t stem_len;
	size_t i;
	size_t fixed_len;
	int cmp1;
	int cmp2;

	/* Measures the filename stem before the first dot. */
	stem_len = 0U;
	while (stem_len < len && name[stem_len] != '.')
		stem_len++;

	/* Matches fixed DOS device names. */
	for (i = 0U; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		fixed_len = strlen(fixed[i]);
		if (stem_len == fixed_len) {
			cmp1 = ascii_icmp_n(name, fixed[i], stem_len);
			if (cmp1 == 0)
				return 1;
		}
	}

	/* Matches COM0..COM9 and LPT0..LPT9. */
	if (stem_len == 4U) {
		cmp1 = ascii_icmp_n(name, "com", 3U);
		cmp2 = ascii_icmp_n(name, "lpt", 3U);
		if ((cmp1 == 0 || cmp2 == 0) && name[3] >= '0' && name[3] <= '9')
			return 1;
	}

	/* Superscript digits 1, 2, 3 in UTF-8: U+00B9 (C2 B9), U+00B2 (C2 B2), U+00B3 (C2 B3). */
	if (stem_len == 5U) {
		cmp1 = ascii_icmp_n(name, "com", 3U);
		cmp2 = ascii_icmp_n(name, "lpt", 3U);
		if ((cmp1 == 0 || cmp2 == 0) && (unsigned char)name[3] == 0xc2U &&
		    ((unsigned char)name[4] == 0xb9U || (unsigned char)name[4] == 0xb2U || (unsigned char)name[4] == 0xb3U))
			return 1;
	}

	/* Not a reserved device name. */
	return 0;
}

/* Checks whether name matches an 8.3 short-name form containing '~<digits>'. */
static int
is_8_3_tilde_name(
	const char *name,
	size_t len)
{
	size_t dot;
	size_t stem_len;
	size_t ext_len;
	size_t i;

	/* Splits stem and extension lengths around the first dot. */
	dot = 0U;
	while (dot < len && name[dot] != '.')
		dot++;
	stem_len = dot;
	if (dot < len)
		ext_len = len - dot - 1U;
	else
		ext_len = 0U;

	/* Checks 8.3 length bounds and scans the stem for '~<digit>'. */
	if (stem_len == 0U || stem_len > 8U || ext_len > 3U)
		return 0;
	for (i = 0U; i + 1U < stem_len; i++) {
		if (name[i] == '~' && name[i + 1U] >= '0' && name[i + 1U] <= '9')
			return 1;
	}

	/* Does not match an 8.3 tilde pattern. */
	return 0;
}

/* Starts the next queued transfer item if any. */
static void
start_next_item(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	int *dirty)
{
	size_t i;
	struct xfer_item *item;
	const char *base;
	const char *why;
	uint64_t size;
	uint32_t mtime;
	unsigned room;
	int valid_name;
	int error;

	/* Finds the next queued item. */
	for (i = 0U; i < q->count; i++) {
		if (q->items[i].state == SFTP_TRANSFER_QUEUED)
			break;
	}

	/* Returns when no queued items remain. */
	if (i == q->count)
		return;

	/* Waits if the SFTP client pipeline is full. */
	room = sftp_client_room(client);
	if (room == 0U)
		return;

	/* Resets per-item transfer state. */
	q->active_index = i;
	q->has_active = 1;
	q->cancel_requested = 0;
	q->failed = 0;
	q->existed = 0;
	q->eof_seen = 0;
	q->eof_offset = 0U;
	q->next_offset = 0U;
	q->remote_mtime = 0U;
	q->local_mtime = 0U;
	q->handle_size = 0U;
	q->temp_path[0] = '\0';
	q->inflight_count = 0U;
	memset(q->inflight, 0, sizeof(q->inflight));
	memset(q->retries, 0, sizeof(q->retries));

	/* Activates the selected item and marks the UI dirty. */
	item = &q->items[i];
	item->state = SFTP_TRANSFER_ACTIVE;
	*dirty = 1;

	/* Starts a download by validating the filename and statting the remote path. */
	if (item->direction == SFTP_TRANSFER_DOWNLOAD) {
		base = path_base(item->remote_path);
		valid_name = sftp_transfer_valid_download_name(base, strlen(base), &why);
		if (!valid_name) {
			finish_active(q, SFTP_TRANSFER_SKIPPED, why, dirty);
			return;
		}

		/* Queries the remote path attributes. */
		error = sftp_client_stat(client, (const uint8_t *)item->remote_path, strlen(item->remote_path), &q->pending_req_id);
		if (error != 0) {
			finish_active(q, SFTP_TRANSFER_FAILED, "Could not inspect remote path.", dirty);
			return;
		}

		/* Waits for STAT completion. */
		q->has_pending_req = 1;
		q->phase = XFER_DL_STAT;
		return;
	}

	/* Starts a folder upload by creating the remote directory. */
	if (item->is_dir) {
		error = sftp_client_mkdir(client, (const uint8_t *)item->remote_path, strlen(item->remote_path), NULL, &q->pending_req_id);
		if (error != 0) {
			finish_active(q, SFTP_TRANSFER_FAILED, "Could not create remote directory.", dirty);
			return;
		}

		/* Waits for MKDIR completion. */
		q->has_pending_req = 1;
		q->phase = XFER_UL_MKDIR;
		return;
	}

	/* Opens the local file for reading before checking the remote destination. */
	error = sftp_local_open_read(item->local_path, &q->local_file, &size, &mtime);
	if (error != 0) {
		finish_active(q, SFTP_TRANSFER_FAILED, "Local file could not be opened.", dirty);
		return;
	}

	/* Records local file size and mtime, then stats the remote destination. */
	item->bytes_total = size;
	q->local_mtime = mtime;
	error = sftp_client_lstat(client, (const uint8_t *)item->remote_path, strlen(item->remote_path), &q->pending_req_id);
	if (error != 0) {
		sftp_local_close(q->local_file);
		q->local_file = NULL;
		finish_active(q, SFTP_TRANSFER_FAILED, "Could not inspect remote destination.", dirty);
		return;
	}

	/* Waits for LSTAT completion. */
	q->has_pending_req = 1;
	q->phase = XFER_UL_STAT;
}

/* Completes the active transfer item and clears active state. */
static void
finish_active(
	struct sftp_transfer_queue *q,
	enum sftp_transfer_state state,
	const char *err,
	int *dirty)
{
	struct xfer_item *item;

	/* Records final state and error message on the active item. */
	item = &q->items[q->active_index];
	item->state = state;
	if (err != NULL)
		(void)snprintf(item->error_text, sizeof(item->error_text), "%s", err);
	q->has_active = 0;
	q->phase = XFER_NONE;
	q->has_pending_req = 0;
	*dirty = 1;
}

/* Issues pipelined READ requests for an active download. */
static void
pump_dl_data(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	unsigned max_requests,
	int *dirty)
{
	struct xfer_item *item;
	uint64_t offset;
	uint64_t rem;
	uint32_t length;
	uint32_t req_id;
	unsigned room;
	int is_retry;
	int error;

	/* Pipelines READ requests up to max_requests and available client room. */
	item = &q->items[q->active_index];
	while (!q->failed && !q->cancel_requested && q->inflight_count < max_requests) {
		room = sftp_client_room(client);
		if (room == 0U)
			break;
		is_retry = take_retry_range(q, &offset, &length);
		if (is_retry) {
			if (q->eof_seen && offset >= q->eof_offset)
				continue;
		} else {
			if (q->eof_seen)
				break;
			if (item->bytes_total != 0U && q->next_offset >= item->bytes_total)
				break;
			offset = q->next_offset;
			if (item->bytes_total == 0U) {
				length = SFTP_CLIENT_CHUNK;
				/* One probe read for 0-byte files; EOF stops further reads. */
				q->next_offset = SFTP_CLIENT_CHUNK;
			} else {
				rem = item->bytes_total - offset;
				if (rem > SFTP_CLIENT_CHUNK)
					length = SFTP_CLIENT_CHUNK;
				else
					length = (uint32_t)rem;
				q->next_offset += length;
			}
		}

		/* Sends the READ request and tracks it in flight. */
		error = sftp_client_read(client, q->handle, q->handle_size, offset, length, &req_id);
		if (error != 0)
			break;
		(void)add_inflight(q, req_id, offset, length);
	}

	/* Closes the remote handle once all in-flight reads drain at EOF or error. */
	if (q->inflight_count == 0U && (q->failed || q->cancel_requested || q->eof_seen ||
	    (item->bytes_total != 0U && q->next_offset >= item->bytes_total)))
		begin_close_handle(q, client, XFER_DL_CLOSE, dirty);
}

/* Issues pipelined WRITE requests for an active upload. */
static void
pump_ul_data(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	unsigned max_requests,
	int *dirty)
{
	struct xfer_item *item;
	struct ssh_sftp_attrs attrs;
	uint8_t buf[SFTP_CLIENT_CHUNK];
	uint64_t rem;
	size_t want;
	size_t got;
	uint32_t req_id;
	unsigned room;
	int error;

	/* Pipelines WRITE requests up to max_requests and available client room. */
	item = &q->items[q->active_index];
	while (!q->failed && !q->cancel_requested && q->inflight_count < max_requests &&
	    q->next_offset < item->bytes_total) {
		room = sftp_client_room(client);
		if (room == 0U)
			break;

		/* Reads the next chunk from the local file. */
		rem = item->bytes_total - q->next_offset;
		if (rem > sizeof(buf))
			want = sizeof(buf);
		else
			want = (size_t)rem;
		error = sftp_local_read_at(q->local_file, q->next_offset, buf, want, &got);
		if (error != 0 || got == 0U) {
			q->failed = 1;
			(void)snprintf(item->error_text, sizeof(item->error_text), "Local read failed.");
			break;
		}

		/* Sends the WRITE request and advances next_offset. */
		error = sftp_client_write(client, q->handle, q->handle_size, q->next_offset, buf, got, &req_id);
		if (error != 0)
			break;
		(void)add_inflight(q, req_id, q->next_offset, (uint32_t)got);
		q->next_offset += got;
	}

	/* Preserves timestamps via FSETSTAT or closes the remote handle once writes drain. */
	if (q->inflight_count == 0U && (q->failed || q->cancel_requested || q->next_offset >= item->bytes_total)) {
		if (!q->failed && !q->cancel_requested && q->preserve_times && q->local_mtime != 0U) {
			memset(&attrs, 0, sizeof(attrs));
			attrs.flags = SSH_FILEXFER_ATTR_ACMODTIME;
			attrs.atime = q->local_mtime;
			attrs.mtime = q->local_mtime;
			error = sftp_client_fsetstat(client, q->handle, q->handle_size, &attrs, &q->pending_req_id);
			if (error == 0) {
				q->has_pending_req = 1;
				q->phase = XFER_UL_FSETSTAT;
				return;
			}
		}

		/* Closes the remote handle. */
		begin_close_handle(q, client, XFER_UL_CLOSE, dirty);
	}
}

/* Records an in-flight request. */
static int
add_inflight(
	struct sftp_transfer_queue *q,
	uint32_t id,
	uint64_t offset,
	uint32_t length)
{
	size_t i;

	/* Finds a free slot in the in-flight table. */
	for (i = 0U; i < SFTP_CLIENT_REQUESTS; i++) {
		if (!q->inflight[i].used) {
			q->inflight[i].used = 1;
			q->inflight[i].id = id;
			q->inflight[i].offset = offset;
			q->inflight[i].length = length;
			q->inflight_count++;
			return 0;
		}
	}

	/* No free slot available. */
	return ENOSPC;
}

/* Finds and removes an in-flight request by id. */
static int
take_inflight(
	struct sftp_transfer_queue *q,
	uint32_t id,
	uint64_t *offset,
	uint32_t *length)
{
	size_t i;

	/* Searches for the matching in-flight request id. */
	for (i = 0U; i < SFTP_CLIENT_REQUESTS; i++) {
		if (q->inflight[i].used && q->inflight[i].id == id) {
			*offset = q->inflight[i].offset;
			*length = q->inflight[i].length;
			q->inflight[i].used = 0;
			q->inflight_count--;
			return 0;
		}
	}

	/* Request id not found. */
	return ENOENT;
}

/* Queues a short-read remainder for re-issue. */
static void
add_retry_range(
	struct sftp_transfer_queue *q,
	uint64_t offset,
	uint32_t length)
{
	size_t i;

	/* Stores the remainder range in the first unused retry slot. */
	for (i = 0U; i < SFTP_CLIENT_REQUESTS; i++) {
		if (!q->retries[i].used) {
			q->retries[i].used = 1;
			q->retries[i].offset = offset;
			q->retries[i].length = length;
			return;
		}
	}
}

/* Pops a short-read remainder if one is queued. */
static int
take_retry_range(
	struct sftp_transfer_queue *q,
	uint64_t *offset,
	uint32_t *length)
{
	size_t i;

	/* Returns the first pending retry range. */
	for (i = 0U; i < SFTP_CLIENT_REQUESTS; i++) {
		if (q->retries[i].used) {
			*offset = q->retries[i].offset;
			*length = q->retries[i].length;
			q->retries[i].used = 0;
			return 1;
		}
	}

	/* No retry range queued. */
	return 0;
}

/* Opens the local temporary file and sends SSH_FXP_OPEN for a download. */
static void
begin_dl_open(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	int *dirty)
{
	struct xfer_item *item;
	const char *base;
	int error;

	/* Creates the local temporary file first. */
	item = &q->items[q->active_index];
	base = path_base(item->local_path);
	error = sftp_local_open_temp_write(item->local_dir, base, q->temp_path, sizeof(q->temp_path), &q->local_file);
	if (error != 0) {
		finish_active(q, SFTP_TRANSFER_FAILED, "Could not create local temporary file.", dirty);
		return;
	}

	/* Opens the remote file for reading. */
	error = sftp_client_open_file(
		client,
		(const uint8_t *)item->remote_path,
		strlen(item->remote_path),
		SSH_FXF_READ,
		NULL,
		&q->pending_req_id);
	if (error != 0) {
		sftp_local_close(q->local_file);
		q->local_file = NULL;
		(void)sftp_local_remove(q->temp_path);
		finish_active(q, SFTP_TRANSFER_FAILED, "Could not open remote file.", dirty);
		return;
	}

	/* Waits for OPEN completion. */
	q->has_pending_req = 1;
	q->phase = XFER_DL_OPEN;
}

/* Sends SSH_FXP_OPEN (CREAT | TRUNC, empty attrs) for an upload. */
static void
begin_ul_open(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	int *dirty)
{
	struct xfer_item *item;
	int error;

	/* Opens and truncates the remote destination file. */
	item = &q->items[q->active_index];
	error = sftp_client_open_file(
		client,
		(const uint8_t *)item->remote_path,
		strlen(item->remote_path),
		SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC,
		NULL,
		&q->pending_req_id);
	if (error != 0) {
		sftp_local_close(q->local_file);
		q->local_file = NULL;
		finish_active(q, SFTP_TRANSFER_FAILED, "Could not open remote file for writing.", dirty);
		return;
	}

	/* Waits for OPEN completion. */
	q->has_pending_req = 1;
	q->phase = XFER_UL_OPEN;
}

/* Sends SSH_FXP_CLOSE for the current remote handle. */
static void
begin_close_handle(
	struct sftp_transfer_queue *q,
	struct sftp_client *client,
	enum xfer_phase next_phase,
	int *dirty)
{
	int error;

	/* Sends CLOSE for the active remote file or directory handle. */
	error = sftp_client_close_handle(client, q->handle, q->handle_size, &q->pending_req_id);
	if (error != 0) {
		if (q->local_file != NULL) {
			sftp_local_close(q->local_file);
			q->local_file = NULL;
		}

		/* Fails the transfer if CLOSE could not be queued. */
		finish_active(q, SFTP_TRANSFER_FAILED, "Could not close remote handle.", dirty);
		return;
	}

	/* Waits for CLOSE completion in next_phase. */
	q->has_pending_req = 1;
	q->phase = next_phase;
}

/* Lists a local directory and enqueues its non-reparse children for upload. */
static void
expand_local_dir(
	struct sftp_transfer_queue *q,
	const struct xfer_item *parent)
{
	struct sftp_local_entry *entries;
	size_t count;
	size_t i;
	char local_child[MIKUTERM_PATH_MAX];
	char child_remote[SSH_SFTP_PATH_MAX];
	int error;

	/* Lists the local directory; ignores errors silently if unreadable. */
	error = sftp_local_list(parent->local_path, &entries, &count);
	if (error != 0)
		return;

	/* Enqueues each non-reparse child for upload. */
	for (i = 0U; i < count; i++) {
		if (entries[i].is_reparse)
			continue;
		error = sftp_local_join_path(local_child, sizeof(local_child), parent->local_path, entries[i].name);
		if (error != 0)
			continue;
		error = sftp_transfer_join_remote(child_remote, sizeof(child_remote), parent->remote_path, entries[i].name);
		if (error != 0)
			continue;
		(void)sftp_transfer_enqueue(q, SFTP_TRANSFER_UPLOAD, local_child, child_remote, entries[i].is_dir, NULL);
	}

	/* Frees the local directory listing. */
	sftp_local_list_free(entries, count);
}
