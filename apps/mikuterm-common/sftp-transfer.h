/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transfer queue and pipelined file transfer engine shared with
 * sftp-worker.c.
 */

#ifndef MIKUTERM_SFTP_TRANSFER_H
#define MIKUTERM_SFTP_TRANSFER_H

#include "sftp-worker.h"
#include "sftp-local.h"

#include "userland/base/ssh/sftp-client.h"

#include <stddef.h>
#include <stdint.h>

/* Overwrite policy for a batch of transfers. */
enum sftp_overwrite_policy {
	SFTP_OVERWRITE_ASK = 0,
	SFTP_OVERWRITE_ALL = 1,
	SFTP_SKIP_ALL = 2
};

struct sftp_transfer_queue;

int sftp_transfer_queue_create(int preserve_times, struct sftp_transfer_queue **queue_out);
void sftp_transfer_queue_destroy(struct sftp_transfer_queue *queue);

int sftp_transfer_enqueue(struct sftp_transfer_queue *queue, enum sftp_transfer_dir dir, const char *local_path, const char *remote_path, int is_dir, uint64_t *id_out);
void sftp_transfer_resolve_conflict(struct sftp_transfer_queue *queue, uint64_t id, enum sftp_conflict_action action);
void sftp_transfer_cancel(struct sftp_transfer_queue *queue, uint64_t id);
void sftp_transfer_cancel_all(struct sftp_transfer_queue *queue);
int sftp_transfer_snapshot_all(const struct sftp_transfer_queue *queue, struct sftp_transfer_snapshot **items, size_t *count, size_t *active_or_queued);

/* Worker-thread driving functions. */
int sftp_transfer_has_work(const struct sftp_transfer_queue *queue);
void sftp_transfer_step(struct sftp_transfer_queue *queue, struct sftp_client *client, unsigned max_requests, int *dirty);
int sftp_transfer_handle_reply(struct sftp_transfer_queue *queue, struct sftp_client *client, const struct sftp_client_reply *reply, int *taken, int *dirty);
void sftp_transfer_abort_active(struct sftp_transfer_queue *queue, const char *reason, int *dirty);

/* Pure filename validation for Windows downloads. */
int sftp_transfer_valid_download_name(const char *name, size_t size, const char **why);

/* Pure remote path joining helper. */
int sftp_transfer_join_remote(char *out, size_t out_cap, const char *dir, const char *name);

#endif
