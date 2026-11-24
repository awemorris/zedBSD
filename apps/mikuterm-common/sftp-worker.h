/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-tab SFTP worker thread: owns sftp_client, the directory listing
 * cache, two request lanes (urgent UI/stream requests ahead of the
 * transfer queue), and recursive operations.
 */

#ifndef MIKUTERM_SFTP_WORKER_H
#define MIKUTERM_SFTP_WORKER_H

#include "sftp-link.h"
#include "mikuterm.h"

#include "userland/base/ssh/sftp-client.h"

#include <stddef.h>
#include <stdint.h>

/* Directory listing cache limits. */
#define SFTP_CACHE_ENTRIES_MAX		100000U
#define SFTP_CACHE_BYTES_MAX		(64U * 1024U * 1024U)

/* Maximum in-flight transfer requests when an urgent stream read is active. */
#define SFTP_TRANSFER_SHARED_REQUESTS	32U

/* Maximum SFTP file/directory handle length (draft-ietf-secsh-filexfer-02 section 4). */
#define SFTP_WORKER_HANDLE_MAX		256U

/* Worker session state. */
enum sftp_worker_state {
	SFTP_WORKER_IDLE = 0,
	SFTP_WORKER_OPENING = 1,
	SFTP_WORKER_READY = 2,
	SFTP_WORKER_FAILED = 3,
	SFTP_WORKER_ENDED = 4
};

/* One entry in a remote directory listing. */
struct sftp_dir_entry {
	char name[SSH_SFTP_PATH_MAX];
	char long_name[512];
	char link_target[SSH_SFTP_PATH_MAX];
	uint64_t size;
	uint32_t mtime;
	uint32_t permissions;
	int is_dir;
	int is_link;
	int link_is_dir;
};

/* One remote directory listing returned by sftp_worker_list_dir. */
struct sftp_dir_listing {
	char path[SSH_SFTP_PATH_MAX];
	struct sftp_dir_entry *entries;
	size_t count;
	int truncated;
	int error;
	char error_text[256];
};

/* Transfer direction. */
enum sftp_transfer_dir {
	SFTP_TRANSFER_DOWNLOAD = 0,
	SFTP_TRANSFER_UPLOAD = 1
};

/* Transfer item state. */
enum sftp_transfer_state {
	SFTP_TRANSFER_QUEUED = 0,
	SFTP_TRANSFER_WAITING_OVERWRITE = 1,
	SFTP_TRANSFER_ACTIVE = 2,
	SFTP_TRANSFER_DONE = 3,
	SFTP_TRANSFER_SKIPPED = 4,
	SFTP_TRANSFER_CANCELLED = 5,
	SFTP_TRANSFER_FAILED = 6
};

/* User resolution for an overwrite conflict. */
enum sftp_conflict_action {
	SFTP_CONFLICT_OVERWRITE = 0,
	SFTP_CONFLICT_SKIP = 1,
	SFTP_CONFLICT_OVERWRITE_ALL = 2,
	SFTP_CONFLICT_SKIP_ALL = 3,
	SFTP_CONFLICT_CANCEL = 4
};

/* Snapshot of one transfer item for the UI. */
struct sftp_transfer_snapshot {
	uint64_t id;
	enum sftp_transfer_dir direction;
	enum sftp_transfer_state state;
	char local_path[MIKUTERM_PATH_MAX];
	char remote_path[SSH_SFTP_PATH_MAX];
	uint64_t bytes_done;
	uint64_t bytes_total;
	uint64_t conflict_remote_size;
	uint32_t conflict_remote_mtime;
	uint64_t conflict_local_size;
	uint32_t conflict_local_mtime;
	char error_text[256];
};

struct sftp_worker;

int sftp_worker_start(struct sftp_link *link, int preserve_times, void (*notify)(void *data), void *notify_data, struct sftp_worker **worker_out);
void sftp_worker_stop(struct sftp_worker *worker);
void sftp_worker_retry(struct sftp_worker *worker);
void sftp_worker_status(struct sftp_worker *worker, enum sftp_worker_state *state, char *home, size_t home_cap, char *error_text, size_t err_cap);

int sftp_worker_list_dir(struct sftp_worker *worker, const char *path, int force_refresh, struct sftp_dir_listing **listing_out);
void sftp_dir_listing_free(struct sftp_dir_listing *listing);

int sftp_worker_mkdir(struct sftp_worker *worker, const char *path, char *err, size_t err_cap);
int sftp_worker_rename(struct sftp_worker *worker, const char *old_path, const char *new_path, char *err, size_t err_cap);
int sftp_worker_delete_count(struct sftp_worker *worker, const char *path, size_t *items, size_t *folders, int *is_symlink);
int sftp_worker_delete(struct sftp_worker *worker, const char *path, char *err, size_t err_cap);
int sftp_worker_stat(struct sftp_worker *worker, const char *path, int follow_links, struct sftp_dir_entry *entry);

int sftp_worker_enqueue_download(struct sftp_worker *worker, const char *remote_path, const char *local_dir, int is_dir, uint64_t *id_out);
int sftp_worker_enqueue_upload(struct sftp_worker *worker, const char *local_path, const char *remote_dir, int is_dir, uint64_t *id_out);
void sftp_worker_resolve_conflict(struct sftp_worker *worker, uint64_t id, enum sftp_conflict_action action);
void sftp_worker_cancel_transfer(struct sftp_worker *worker, uint64_t id);
void sftp_worker_cancel_all(struct sftp_worker *worker);
int sftp_worker_transfers(struct sftp_worker *worker, struct sftp_transfer_snapshot **items, size_t *count, size_t *active_or_queued);
void sftp_worker_transfers_free(struct sftp_transfer_snapshot *items);

int sftp_worker_stream_open(struct sftp_worker *worker, const char *remote_path, uint64_t *stream_id);
int sftp_worker_stream_read(struct sftp_worker *worker, uint64_t stream_id, uint64_t offset, void *buf, size_t size, size_t *got);
void sftp_worker_stream_close(struct sftp_worker *worker, uint64_t stream_id);

#endif
