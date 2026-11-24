/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-tab SFTP worker thread.
 *
 * Runs the sftp_client state machine over sftp_link, keeps a bounded
 * directory listing cache, serves urgent UI and Explorer stream
 * requests ahead of the transfer queue, and drives sftp_transfer_step.
 */

#include "sftp-worker.h"
#include "sftp-transfer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum number of cached directory listings kept at once. */
#define WORKER_CACHE_SLOTS	64U

/* Maximum number of open Explorer streams at once. */
#define WORKER_STREAMS_MAX	8U

/* Minimum interval between progress notifications to the UI, in milliseconds. */
#define WORKER_NOTIFY_MS	100U

/* Urgent request types processed ahead of the transfer queue. */
enum urgent_kind {
	URGENT_LIST_DIR = 1,
	URGENT_MKDIR,
	URGENT_RENAME,
	URGENT_DELETE_COUNT,
	URGENT_DELETE,
	URGENT_STAT,
	URGENT_STREAM_OPEN,
	URGENT_STREAM_READ,
	URGENT_STREAM_CLOSE
};

/* One urgent request submitted by the UI or an Explorer stream. */
struct urgent_req {
	enum urgent_kind kind;
	int done;
	int error;
	char path[SSH_SFTP_PATH_MAX];
	char path2[SSH_SFTP_PATH_MAX];
	int flag;
	uint64_t stream_id;
	uint64_t offset;
	void *buf;
	size_t size;
	size_t got;
	struct sftp_dir_listing *listing;
	struct sftp_dir_entry entry;
	size_t items;
	size_t folders;
	int is_symlink;
	char error_text[256];
	struct urgent_req *next;
};

/* One cached remote directory listing. */
struct cache_slot {
	int used;
	char path[SSH_SFTP_PATH_MAX];
	struct sftp_dir_entry *entries;
	size_t count;
	int truncated;
	uint64_t stamp;
};

/* One open remote file handle for an Explorer IStream. */
struct worker_stream {
	int used;
	uint64_t id;
	uint8_t handle[SFTP_WORKER_HANDLE_MAX];
	size_t handle_size;
};

/* Recursive delete traversal frame. */
struct del_frame {
	char path[SSH_SFTP_PATH_MAX];
	uint8_t handle[SFTP_WORKER_HANDLE_MAX];
	size_t handle_size;
	int handle_open;
	char **children;
	int *child_is_dir;
	int *child_is_link;
	size_t child_count;
	size_t child_cap;
	size_t child_index;
};

/*
 * Per-tab SFTP worker state.
 *
 * Owns the background thread, the non-blocking sftp_client state machine,
 * the bounded directory listing cache, the urgent UI request queue, and
 * the pipelined transfer queue.
 */
struct sftp_worker {
	struct sftp_link *link;
	struct sftp_client *client;
	struct sftp_transfer_queue *transfers;
	void (*notify)(void *data);
	void *notify_data;

	struct kl_mutex lock;
	struct kl_cond done_cond;
	struct kl_thread *thread;
	int stop;
	int retry_wanted;
	int version_seen;
	int home_pending;
	uint32_t home_req_id;

	enum sftp_worker_state state;
	char home[SSH_SFTP_PATH_MAX];
	char error_text[256];

	/* Urgent request queue (protected by lock). */
	struct urgent_req *urgent_head;
	struct urgent_req *urgent_tail;
	struct urgent_req *active_urgent;
	int urgent_phase;
	uint32_t urgent_req_id;
	uint8_t urgent_handle[SFTP_WORKER_HANDLE_MAX];
	size_t urgent_handle_size;
	struct sftp_dir_listing *building_listing;
	size_t building_cap;

	/* Recursive delete state. */
	struct del_frame *del_stack;
	size_t del_depth;
	size_t del_cap;

	/* Open Explorer streams. */
	struct worker_stream streams[WORKER_STREAMS_MAX];
	uint64_t next_stream_id;
	unsigned active_stream_reads;

	/* Directory listing cache. */
	struct cache_slot cache[WORKER_CACHE_SLOTS];
	size_t cache_entries_total;
	size_t cache_bytes_total;
	uint64_t cache_clock;

	uint64_t last_notify_ms;
	int notify_dirty;
};

static void worker_thread_main(void *arg);
static void worker_step(struct sftp_worker *w);
static void worker_pump_io(struct sftp_worker *w);
static void worker_handle_reply(struct sftp_worker *w, const struct sftp_client_reply *reply);
static void worker_start_urgent(struct sftp_worker *w);
static void worker_urgent_reply(struct sftp_worker *w, const struct sftp_client_reply *reply);
static void worker_advance_delete(struct sftp_worker *w, struct urgent_req *req);
static void worker_finish_urgent(struct sftp_worker *w, int error, const char *err_text);
static int worker_submit_urgent(struct sftp_worker *w, struct urgent_req *req);
static struct cache_slot *cache_find(struct sftp_worker *w, const char *path);
static void cache_store(struct sftp_worker *w, const struct sftp_dir_listing *listing);
static void cache_invalidate(struct sftp_worker *w, const char *path);
static void cache_clear(struct sftp_worker *w);
static void cache_evict_oldest(struct sftp_worker *w);
static struct sftp_dir_listing *listing_clone(const struct cache_slot *slot);
static void del_stack_clear(struct sftp_worker *w);
static int del_stack_push(struct sftp_worker *w, const char *path);
static void del_stack_pop(struct sftp_worker *w);
static int del_frame_add_child(struct del_frame *f, const char *path, int is_dir, int is_link);
static void worker_reset_client(struct sftp_worker *w);
static void worker_maybe_notify(struct sftp_worker *w, int force);
static uint64_t worker_now_ms(void);

/*
 * Starts the SFTP worker thread for link, retaining a reference to link.
 * Returns 0 or an errno.
 */
int
sftp_worker_start(
	struct sftp_link *link,
	int preserve_times,
	void (*notify)(void *data),
	void *notify_data,
	struct sftp_worker **worker_out)
{
	struct sftp_worker *w;
	struct kl_thread_options opts;
	int error;

	/* Allocates the worker state. */
	*worker_out = NULL;
	w = calloc(1U, sizeof(*w));
	if (w == NULL)
		return ENOMEM;

	/* Creates the transfer queue owned by the worker. */
	error = sftp_transfer_queue_create(preserve_times, &w->transfers);
	if (error != 0) {
		free(w);
		return error;
	}

	/* Initializes the worker mutex. */
	error = kl_mutex_init(&w->lock);
	if (error != 0) {
		sftp_transfer_queue_destroy(w->transfers);
		free(w);
		return error;
	}

	/* Initializes the urgent-request completion condition variable. */
	error = kl_cond_init(&w->done_cond);
	if (error != 0) {
		kl_mutex_destroy(&w->lock);
		sftp_transfer_queue_destroy(w->transfers);
		free(w);
		return error;
	}

	/* Retains the relay and requests opening the SFTP subsystem channel. */
	sftp_link_retain(link);
	w->link = link;
	w->notify = notify;
	w->notify_data = notify_data;
	w->next_stream_id = 1U;
	w->state = SFTP_WORKER_OPENING;
	sftp_link_request_open(link);

	/* Starts the background worker thread. */
	memset(&opts, 0, sizeof(opts));
	opts.run = worker_thread_main;
	opts.argument = w;
	opts.name = "sftp-worker";
	error = kl_thread_start(&opts, &w->thread);
	if (error != 0) {
		sftp_link_release(w->link);
		kl_cond_destroy(&w->done_cond);
		kl_mutex_destroy(&w->lock);
		sftp_transfer_queue_destroy(w->transfers);
		free(w);
		return error;
	}

	/* Succeeded: publishes the started worker. */
	*worker_out = w;
	return 0;
}

/*
 * Stops the worker thread, joins it, and frees all resources.
 */
void
sftp_worker_stop(
	struct sftp_worker *w)
{
	struct urgent_req *req;

	/* Ignores a NULL worker pointer. */
	if (w == NULL)
		return;

	/* Signals the worker thread to exit and wakes it. */
	kl_mutex_lock(&w->lock);

	w->stop = 1;

	kl_mutex_unlock(&w->lock);
	sftp_link_request_close(w->link);
	sftp_link_wake_worker(w->link);

	/* Waits for the worker thread to finish. */
	(void)kl_thread_join(w->thread);

	/* Fails any remaining queued urgent requests as cancelled. */
	while (w->urgent_head != NULL) {
		req = w->urgent_head;
		w->urgent_head = req->next;
		req->done = 1;
		req->error = ECANCELED;
	}

	/* Frees all worker-owned state and synchronization primitives. */
	del_stack_clear(w);
	cache_clear(w);
	worker_reset_client(w);
	sftp_transfer_queue_destroy(w->transfers);
	sftp_link_release(w->link);
	kl_cond_destroy(&w->done_cond);
	kl_mutex_destroy(&w->lock);
	free(w);
}

/*
 * Retries opening the SFTP subsystem channel after a failure.
 */
void
sftp_worker_retry(
	struct sftp_worker *w)
{
	int ended;

	/* Schedules a channel reopen if the underlying SSH connection is still alive. */
	ended = sftp_link_is_ended(w->link);
	kl_mutex_lock(&w->lock);

	if (w->state == SFTP_WORKER_FAILED && !ended) {
		w->retry_wanted = 1;
		w->state = SFTP_WORKER_OPENING;
		w->error_text[0] = '\0';
	}

	kl_mutex_unlock(&w->lock);

	/* Wakes the worker thread to perform the retry. */
	sftp_link_wake_worker(w->link);
}

/*
 * Reads the current worker state, home directory, and error message.
 */
void
sftp_worker_status(
	struct sftp_worker *w,
	enum sftp_worker_state *state,
	char *home,
	size_t home_cap,
	char *error_text,
	size_t err_cap)
{
	/* Copies worker status fields under the worker lock. */
	kl_mutex_lock(&w->lock);

	*state = w->state;
	if (home != NULL && home_cap != 0U)
		(void)snprintf(home, home_cap, "%s", w->home);
	if (error_text != NULL && err_cap != 0U)
		(void)snprintf(error_text, err_cap, "%s", w->error_text);

	kl_mutex_unlock(&w->lock);
}

/*
 * Lists a remote directory, using the cache unless force_refresh is set.
 */
int
sftp_worker_list_dir(
	struct sftp_worker *w,
	const char *path,
	int force_refresh,
	struct sftp_dir_listing **listing_out)
{
	struct cache_slot *slot;
	struct urgent_req req;
	int error;

	/* Checks the directory cache first unless a refresh was requested. */
	*listing_out = NULL;
	if (!force_refresh) {
		kl_mutex_lock(&w->lock);

		slot = cache_find(w, path);
		if (slot != NULL) {
			*listing_out = listing_clone(slot);
			kl_mutex_unlock(&w->lock);
			if (*listing_out == NULL)
				return ENOMEM;

			/* Succeeded: returned a cached directory listing. */
			return 0;
		}

		kl_mutex_unlock(&w->lock);
	}

	/* Submits an urgent directory-listing request to the worker thread. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_LIST_DIR;
	req.flag = force_refresh;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error != 0) {
		sftp_dir_listing_free(req.listing);
		return error;
	}

	/* Succeeded: returns the freshly fetched directory listing. */
	*listing_out = req.listing;
	return 0;
}

/*
 * Frees a directory listing returned by sftp_worker_list_dir.
 */
void
sftp_dir_listing_free(
	struct sftp_dir_listing *listing)
{
	/* Ignores a NULL listing pointer. */
	if (listing == NULL)
		return;

	/* Frees the entries array and listing header. */
	free(listing->entries);
	free(listing);
}

/*
 * Creates a remote directory.
 */
int
sftp_worker_mkdir(
	struct sftp_worker *w,
	const char *path,
	char *err,
	size_t err_cap)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent mkdir request and copies any error text. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_MKDIR;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	if (error != 0)
		return error;

	/* Succeeded: the remote directory was created. */
	return 0;
}

/*
 * Renames a remote file or directory.
 */
int
sftp_worker_rename(
	struct sftp_worker *w,
	const char *old_path,
	const char *new_path,
	char *err,
	size_t err_cap)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent rename request and copies any error text. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_RENAME;
	(void)snprintf(req.path, sizeof(req.path), "%s", old_path);
	(void)snprintf(req.path2, sizeof(req.path2), "%s", new_path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	if (error != 0)
		return error;

	/* Succeeded: the remote path was renamed. */
	return 0;
}

/*
 * Counts items and folders under path before recursive deletion (does not
 * follow symlinks).
 */
int
sftp_worker_delete_count(
	struct sftp_worker *w,
	const char *path,
	size_t *items,
	size_t *folders,
	int *is_symlink)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent delete-count traversal request. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_DELETE_COUNT;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error != 0)
		return error;

	/* Succeeded: reports counted items, folders, and symlink status. */
	*items = req.items;
	*folders = req.folders;
	*is_symlink = req.is_symlink;
	return 0;
}

/*
 * Recursively deletes path (symlinks are removed without following their
 * target).
 */
int
sftp_worker_delete(
	struct sftp_worker *w,
	const char *path,
	char *err,
	size_t err_cap)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent recursive delete request and copies any error text. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_DELETE;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	if (error != 0)
		return error;

	/* Succeeded: the remote path was deleted. */
	return 0;
}

/*
 * Queries attributes of path (follow_links=1 uses STAT, 0 uses LSTAT).
 */
int
sftp_worker_stat(
	struct sftp_worker *w,
	const char *path,
	int follow_links,
	struct sftp_dir_entry *entry)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent stat/lstat request. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STAT;
	req.flag = follow_links;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error != 0)
		return error;

	/* Succeeded: returns the queried attributes. */
	*entry = req.entry;
	return 0;
}

/*
 * Enqueues a download into local_dir.
 */
int
sftp_worker_enqueue_download(
	struct sftp_worker *w,
	const char *remote_path,
	const char *local_dir,
	int is_dir,
	uint64_t *id_out)
{
	char local_path[MIKUTERM_PATH_MAX];
	const char *slash;
	const char *base;
	int error;

	/* Builds the destination local path from the remote basename. */
	slash = strrchr(remote_path, '/');
	if (slash != NULL)
		base = slash + 1;
	else
		base = remote_path;
	error = sftp_local_join_path(local_path, sizeof(local_path), local_dir, base);
	if (error != 0)
		return error;

	/* Enqueues the download item under the worker lock. */
	kl_mutex_lock(&w->lock);

	error = sftp_transfer_enqueue(w->transfers, SFTP_TRANSFER_DOWNLOAD, local_path, remote_path, is_dir, id_out);

	kl_mutex_unlock(&w->lock);

	/* Wakes the worker thread to start the transfer. */
	sftp_link_wake_worker(w->link);
	if (error != 0)
		return error;

	/* Succeeded: the download is queued. */
	return 0;
}

/*
 * Enqueues an upload into remote_dir.
 */
int
sftp_worker_enqueue_upload(
	struct sftp_worker *w,
	const char *local_path,
	const char *remote_dir,
	int is_dir,
	uint64_t *id_out)
{
	char remote_path[SSH_SFTP_PATH_MAX];
	const char *slash;
	const char *bslash;
	const char *base;
	int error;

	/* Extracts the local basename across both slash styles. */
	slash = strrchr(local_path, '/');
	bslash = strrchr(local_path, '\\');
	base = slash;
	if (bslash != NULL && (base == NULL || bslash > base))
		base = bslash;
	if (base != NULL)
		base = base + 1;
	else
		base = local_path;

	/* Builds the destination remote path. */
	error = sftp_transfer_join_remote(remote_path, sizeof(remote_path), remote_dir, base);
	if (error != 0)
		return error;

	/* Enqueues the upload item under the worker lock. */
	kl_mutex_lock(&w->lock);

	error = sftp_transfer_enqueue(w->transfers, SFTP_TRANSFER_UPLOAD, local_path, remote_path, is_dir, id_out);

	kl_mutex_unlock(&w->lock);

	/* Wakes the worker thread to start the transfer. */
	sftp_link_wake_worker(w->link);
	if (error != 0)
		return error;

	/* Succeeded: the upload is queued. */
	return 0;
}

/*
 * Resolves an overwrite conflict for transfer id.
 */
void
sftp_worker_resolve_conflict(
	struct sftp_worker *w,
	uint64_t id,
	enum sftp_conflict_action action)
{
	/* Forwards the overwrite decision under the worker lock and wakes the worker. */
	kl_mutex_lock(&w->lock);

	sftp_transfer_resolve_conflict(w->transfers, id, action);

	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/*
 * Cancels one transfer item.
 */
void
sftp_worker_cancel_transfer(
	struct sftp_worker *w,
	uint64_t id)
{
	/* Marks the transfer item cancelled under the worker lock and wakes the worker. */
	kl_mutex_lock(&w->lock);

	sftp_transfer_cancel(w->transfers, id);

	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/*
 * Cancels all queued and active transfers.
 */
void
sftp_worker_cancel_all(
	struct sftp_worker *w)
{
	/* Cancels all transfers under the worker lock and wakes the worker. */
	kl_mutex_lock(&w->lock);

	sftp_transfer_cancel_all(w->transfers);

	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/*
 * Copies a snapshot of the transfer queue.
 */
int
sftp_worker_transfers(
	struct sftp_worker *w,
	struct sftp_transfer_snapshot **items,
	size_t *count,
	size_t *active_or_queued)
{
	int error;

	/* Takes a snapshot of the transfer queue under the worker lock. */
	kl_mutex_lock(&w->lock);

	error = sftp_transfer_snapshot_all(w->transfers, items, count, active_or_queued);

	kl_mutex_unlock(&w->lock);
	if (error != 0)
		return error;

	/* Succeeded: returns the snapshot array. */
	return 0;
}

/*
 * Frees a transfer snapshot array.
 */
void
sftp_worker_transfers_free(
	struct sftp_transfer_snapshot *items)
{
	/* Frees the snapshot buffer. */
	free(items);
}

/*
 * Opens a remote file for urgent Explorer stream reads.
 */
int
sftp_worker_stream_open(
	struct sftp_worker *w,
	const char *remote_path,
	uint64_t *stream_id)
{
	struct urgent_req req;
	int error;

	/* Submits an urgent stream-open request. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STREAM_OPEN;
	(void)snprintf(req.path, sizeof(req.path), "%s", remote_path);
	error = worker_submit_urgent(w, &req);
	if (error != 0)
		return error;

	/* Succeeded: returns the assigned stream identifier. */
	*stream_id = req.stream_id;
	return 0;
}

/*
 * Reads bytes at offset from an open Explorer stream (prioritized ahead
 * of transfers).
 */
int
sftp_worker_stream_read(
	struct sftp_worker *w,
	uint64_t stream_id,
	uint64_t offset,
	void *buf,
	size_t size,
	size_t *got)
{
	struct urgent_req req;
	int error;

	/* Marks an active stream read so transfers yield pipeline slots. */
	*got = 0U;
	kl_mutex_lock(&w->lock);

	w->active_stream_reads++;

	kl_mutex_unlock(&w->lock);

	/* Submits the urgent stream-read request. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STREAM_READ;
	req.stream_id = stream_id;
	req.offset = offset;
	req.buf = buf;
	req.size = size;
	error = worker_submit_urgent(w, &req);
	if (error == 0)
		*got = req.got;

	/* Decrements the active stream read counter under the worker lock. */
	kl_mutex_lock(&w->lock);

	w->active_stream_reads--;

	kl_mutex_unlock(&w->lock);
	if (error != 0)
		return error;

	/* Succeeded: *got holds the number of bytes read. */
	return 0;
}

/*
 * Closes an open Explorer stream handle.
 */
void
sftp_worker_stream_close(
	struct sftp_worker *w,
	uint64_t stream_id)
{
	struct urgent_req req;

	/* Submits an urgent stream-close request. */
	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STREAM_CLOSE;
	req.stream_id = stream_id;
	(void)worker_submit_urgent(w, &req);
}

/* Main loop of the worker thread. */
static void
worker_thread_main(
	void *arg)
{
	struct sftp_worker *w;

	/* Polls for stop/retry signals and drives the SFTP state machine until stopped. */
	w = arg;
	for (;;) {
		kl_mutex_lock(&w->lock);

		if (w->stop) {
			kl_mutex_unlock(&w->lock);
			break;
		}

		/* Resets the client and requests a fresh channel if retry was asked. */
		if (w->retry_wanted) {
			w->retry_wanted = 0;
			w->state = SFTP_WORKER_OPENING;
			w->error_text[0] = '\0';
			worker_reset_client(w);
			w->version_seen = 0;
			w->home_pending = 0;
			sftp_link_request_open(w->link);
		}

		kl_mutex_unlock(&w->lock);

		/* Advances I/O, urgent requests, and transfers, then waits briefly. */
		worker_step(w);
		sftp_link_wait(w->link, 50U);
	}
}

/* Advances the worker state machine, urgent requests, and transfer queue. */
static void
worker_step(
	struct sftp_worker *w)
{
	int ready;
	int accepted;
	uint32_t code;
	char reason[SFTP_LINK_REASON_MAX];
	unsigned max_reqs;
	unsigned room;
	int has_work;
	int ended;
	int error;

	/* Marks the worker ended if the underlying SSH connection closed. */
	ended = sftp_link_is_ended(w->link);
	if (ended) {
		kl_mutex_lock(&w->lock);

		if (w->state != SFTP_WORKER_ENDED) {
			w->state = SFTP_WORKER_ENDED;
			(void)snprintf(w->error_text, sizeof(w->error_text), "The connection ended.");
			sftp_transfer_abort_active(w->transfers, "The connection ended.", &w->notify_dirty);
			if (w->active_urgent != NULL)
				worker_finish_urgent(w, ECONNRESET, "The connection ended.");
			w->notify_dirty = 1;
		}

		kl_mutex_unlock(&w->lock);

		/* Notifies the UI that the connection ended. */
		worker_maybe_notify(w, 1);
		return;
	}

	/* Waits for the subsystem channel open reply and opens sftp_client on acceptance. */
	if (w->state == SFTP_WORKER_OPENING) {
		ready = 0;
		accepted = 0;
		code = 0U;
		reason[0] = '\0';
		(void)sftp_link_reply(w->link, &ready, &accepted, &code, reason, sizeof(reason));
		if (!ready)
			return;
		if (!accepted) {
			kl_mutex_lock(&w->lock);

			w->state = SFTP_WORKER_FAILED;
			if (code != 0U) {
				(void)snprintf(w->error_text, sizeof(w->error_text),
				    "The server refused another session (MaxSessions on the server may be 1).");
			} else if (reason[0] != '\0') {
				(void)snprintf(w->error_text, sizeof(w->error_text), "%s", reason);
			} else {
				(void)snprintf(w->error_text, sizeof(w->error_text),
				    "The server has no SFTP subsystem.");
			}

			/* Marks the UI dirty so the failure is shown. */
			w->notify_dirty = 1;

			kl_mutex_unlock(&w->lock);

			/* Notifies the UI of the subsystem open failure. */
			worker_maybe_notify(w, 1);
			return;
		}

		/* Creates the sftp_client protocol state machine if not yet allocated. */
		if (w->client == NULL) {
			error = sftp_client_open(&w->client);
			if (error != 0) {
				kl_mutex_lock(&w->lock);

				w->state = SFTP_WORKER_FAILED;
				(void)snprintf(w->error_text, sizeof(w->error_text), "Out of memory.");
				w->notify_dirty = 1;

				kl_mutex_unlock(&w->lock);

				/* Notifies the UI of the allocation failure. */
				worker_maybe_notify(w, 1);
				return;
			}
		}
	}

	/* Waits until the SFTP client is initialized. */
	if (w->client == NULL)
		return;

	/* Pumps outgoing and incoming SFTP bytes. */
	worker_pump_io(w);

	/* Starts urgent requests ahead of pipelined transfers when ready. */
	if (w->state == SFTP_WORKER_READY) {
		kl_mutex_lock(&w->lock);

		room = sftp_client_room(w->client);
		if (w->active_urgent == NULL && w->urgent_head != NULL && room > 0U)
			worker_start_urgent(w);
		has_work = sftp_transfer_has_work(w->transfers);
		if (has_work) {
			if (w->active_stream_reads > 0U)
				max_reqs = SFTP_TRANSFER_SHARED_REQUESTS;
			else
				max_reqs = SFTP_CLIENT_REQUESTS;
			sftp_transfer_step(w->transfers, w->client, max_reqs, &w->notify_dirty);
		}

		kl_mutex_unlock(&w->lock);

		/* Flushes any newly queued requests and updates the UI if needed. */
		worker_pump_io(w);
		worker_maybe_notify(w, 0);
	}
}

/* Moves bytes between sftp_link and sftp_client and dispatches replies. */
static void
worker_pump_io(
	struct sftp_worker *w)
{
	uint8_t buf[32768];
	size_t got;
	size_t offset;
	size_t taken;
	size_t out_size;
	size_t written;
	const uint8_t *out_bytes;
	int eof;
	int closed;
	int ended;
	struct sftp_client_reply reply;
	int error;

	/* Sends queued sftp_client output to the relay. */
	for (;;) {
		out_bytes = sftp_client_output(w->client, &out_size);
		if (out_bytes == NULL || out_size == 0U)
			break;
		written = 0U;
		error = sftp_link_write(w->link, out_bytes, out_size, &written);
		if (written != 0U)
			sftp_client_written(w->client, written);
		if (error != 0 || written < out_size)
			break;
	}

	/* Reads incoming bytes from the relay and parses replies. */
	for (;;) {
		got = 0U;
		eof = 0;
		closed = 0;
		(void)sftp_link_read(w->link, buf, sizeof(buf), &got, &eof, &closed);
		if (got != 0U) {
			offset = 0U;
			while (offset < got) {
				taken = 0U;
				error = sftp_client_input(w->client, buf + offset, got - offset, &taken);
				if (error != 0)
					break;
				offset += taken;
				for (;;) {
					error = sftp_client_next(w->client, &reply);
					if (error == EAGAIN)
						break;
					if (error != 0) {
						kl_mutex_lock(&w->lock);

						w->state = SFTP_WORKER_FAILED;
						if (!w->version_seen) {
							(void)snprintf(w->error_text, sizeof(w->error_text),
							    "The server did not answer as an SFTP server (a shell startup file may print text).");
						} else {
							(void)snprintf(w->error_text, sizeof(w->error_text),
							    "The SFTP protocol stream failed.");
						}

						/* Aborts any active transfer and urgent request. */
						sftp_transfer_abort_active(w->transfers, w->error_text, &w->notify_dirty);
						if (w->active_urgent != NULL)
							worker_finish_urgent(w, EPROTO, w->error_text);
						w->notify_dirty = 1;

						kl_mutex_unlock(&w->lock);

						/* Closes the broken channel and notifies the UI. */
						sftp_link_request_close(w->link);
						worker_maybe_notify(w, 1);
						return;
					}

					/* Dispatches the parsed SFTP reply. */
					worker_handle_reply(w, &reply);
				}

				/* Stops if the client input buffer could not accept more bytes. */
				if (taken == 0U)
					break;
			}
		}

		/* Transitions to FAILED or ENDED if the channel closed after draining input. */
		if ((eof || closed) && got == 0U) {
			if (w->state != SFTP_WORKER_FAILED && w->state != SFTP_WORKER_ENDED) {
				ended = sftp_link_is_ended(w->link);
				kl_mutex_lock(&w->lock);

				if (ended)
					w->state = SFTP_WORKER_ENDED;
				else
					w->state = SFTP_WORKER_FAILED;
				if (w->error_text[0] == '\0')
					(void)snprintf(w->error_text, sizeof(w->error_text), "The SFTP channel closed.");
				sftp_transfer_abort_active(w->transfers, w->error_text, &w->notify_dirty);
				if (w->active_urgent != NULL)
					worker_finish_urgent(w, ECONNRESET, w->error_text);
				w->notify_dirty = 1;

				kl_mutex_unlock(&w->lock);

				/* Notifies the UI that the SFTP channel closed. */
				worker_maybe_notify(w, 1);
			}

			/* Nothing more can arrive on a closed channel. */
			break;
		}

		/* Stops reading when the relay returned less than a full buffer. */
		if (got < sizeof(buf))
			break;
	}
}

/* Dispatches one parsed SFTP reply to initialization, urgent requests, or transfers. */
static void
worker_handle_reply(
	struct sftp_worker *w,
	const struct sftp_client_reply *reply)
{
	const uint8_t *name;
	size_t name_size;
	const uint8_t *long_name;
	size_t long_size;
	struct ssh_sftp_attrs attrs;
	int taken;
	int error;

	/* Queries the remote home directory once SSH_FXP_VERSION arrives. */
	if (reply->type == SSH_FXP_VERSION) {
		w->version_seen = 1;
		error = sftp_client_realpath(w->client, (const uint8_t *)".", 1U, &w->home_req_id);
		if (error == 0) {
			w->home_pending = 1;
		} else {
			kl_mutex_lock(&w->lock);

			w->state = SFTP_WORKER_READY;
			(void)snprintf(w->home, sizeof(w->home), "/");
			w->notify_dirty = 1;

			kl_mutex_unlock(&w->lock);
			worker_maybe_notify(w, 1);
		}

		/* Version handshake reply handled. */
		return;
	}

	/* Records the initial home directory from the REALPATH reply. */
	if (w->home_pending && reply->id == w->home_req_id) {
		w->home_pending = 0;
		kl_mutex_lock(&w->lock);

		if (reply->error == 0 && reply->type == SSH_FXP_NAME) {
			error = sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs);
			if (error == 0 && name_size < sizeof(w->home)) {
				memcpy(w->home, name, name_size);
				w->home[name_size] = '\0';
			} else {
				(void)snprintf(w->home, sizeof(w->home), "/");
			}
		} else {
			(void)snprintf(w->home, sizeof(w->home), "/");
		}

		/* Marks the worker ready for UI requests and transfers. */
		w->state = SFTP_WORKER_READY;
		w->notify_dirty = 1;

		kl_mutex_unlock(&w->lock);
		worker_maybe_notify(w, 1);
		return;
	}

	/* Routes the reply to the active urgent request or to the transfer queue. */
	kl_mutex_lock(&w->lock);

	if (w->active_urgent != NULL && reply->id == w->urgent_req_id) {
		worker_urgent_reply(w, reply);
		kl_mutex_unlock(&w->lock);
		return;
	}

	/* Passes non-urgent replies to the transfer state machine. */
	taken = 0;
	(void)sftp_transfer_handle_reply(w->transfers, w->client, reply, &taken, &w->notify_dirty);

	kl_mutex_unlock(&w->lock);
}

/* Starts the next queued urgent request (caller holds w->lock). */
static void
worker_start_urgent(
	struct sftp_worker *w)
{
	struct urgent_req *req;
	size_t i;
	uint32_t read_len;
	int error;

	/* Dequeues the head urgent request and marks it active. */
	req = w->urgent_head;
	w->urgent_head = req->next;
	if (w->urgent_head == NULL)
		w->urgent_tail = NULL;
	req->next = NULL;
	w->active_urgent = req;
	w->urgent_phase = 0;

	/* Dispatches the initial SFTP request for this urgent operation. */
	switch (req->kind) {
	case URGENT_LIST_DIR:
		w->building_listing = calloc(1U, sizeof(*w->building_listing));
		if (w->building_listing == NULL) {
			worker_finish_urgent(w, ENOMEM, "Out of memory.");
			return;
		}

		/* Records the target path and opens the remote directory. */
		(void)snprintf(w->building_listing->path, sizeof(w->building_listing->path), "%s", req->path);
		w->building_cap = 0U;
		error = sftp_client_opendir(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		if (error != 0) {
			sftp_dir_listing_free(w->building_listing);
			w->building_listing = NULL;
			worker_finish_urgent(w, error, "Could not open directory.");
			return;
		}

		/* Waits for the OPENDIR handle reply. */
		w->urgent_phase = 1;
		break;

	case URGENT_MKDIR:
		cache_invalidate(w, req->path);
		error = sftp_client_mkdir(w->client, (const uint8_t *)req->path, strlen(req->path), NULL, &w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not create directory.");
		break;

	case URGENT_RENAME:
		cache_invalidate(w, req->path);
		cache_invalidate(w, req->path2);
		error = sftp_client_rename(
			w->client,
			(const uint8_t *)req->path,
			strlen(req->path),
			(const uint8_t *)req->path2,
			strlen(req->path2),
			&w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not rename.");
		break;

	case URGENT_DELETE_COUNT:
	case URGENT_DELETE:
		del_stack_clear(w);
		cache_invalidate(w, req->path);
		error = del_stack_push(w, req->path);
		if (error != 0) {
			worker_finish_urgent(w, ENOMEM, "Out of memory.");
			return;
		}

		/* Stats the root path with LSTAT so symlinks are not followed. */
		error = sftp_client_lstat(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		if (error != 0) {
			del_stack_clear(w);
			worker_finish_urgent(w, error, "Could not inspect path.");
			return;
		}

		/* Waits for the initial LSTAT reply. */
		w->urgent_phase = 1;
		break;

	case URGENT_STAT:
		if (req->flag)
			error = sftp_client_stat(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		else
			error = sftp_client_lstat(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not stat path.");
		break;

	case URGENT_STREAM_OPEN:
		error = sftp_client_open_file(
			w->client,
			(const uint8_t *)req->path,
			strlen(req->path),
			SSH_FXF_READ,
			NULL,
			&w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not open stream.");
		break;

	case URGENT_STREAM_READ:
		for (i = 0U; i < WORKER_STREAMS_MAX; i++) {
			if (w->streams[i].used && w->streams[i].id == req->stream_id)
				break;
		}

		/* Reports EBADF if the stream handle was closed or invalid. */
		if (i == WORKER_STREAMS_MAX) {
			worker_finish_urgent(w, EBADF, "Invalid stream.");
			return;
		}

		/* Clamps the read length to SFTP_CLIENT_CHUNK and issues READ. */
		if (req->size > SFTP_CLIENT_CHUNK)
			read_len = SFTP_CLIENT_CHUNK;
		else
			read_len = (uint32_t)req->size;
		error = sftp_client_read(
			w->client,
			w->streams[i].handle,
			w->streams[i].handle_size,
			req->offset,
			read_len,
			&w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not read stream.");
		break;

	case URGENT_STREAM_CLOSE:
		for (i = 0U; i < WORKER_STREAMS_MAX; i++) {
			if (w->streams[i].used && w->streams[i].id == req->stream_id)
				break;
		}

		/* Finishes immediately if the stream was already closed. */
		if (i == WORKER_STREAMS_MAX) {
			worker_finish_urgent(w, 0, "");
			return;
		}

		/* Closes the remote handle and frees the stream slot. */
		error = sftp_client_close_handle(w->client, w->streams[i].handle, w->streams[i].handle_size, &w->urgent_req_id);
		w->streams[i].used = 0;
		if (error != 0)
			worker_finish_urgent(w, error, "");
		break;
	}
}

/* Handles a reply for the active urgent request (caller holds w->lock). */
static void
worker_urgent_reply(
	struct sftp_worker *w,
	const struct sftp_client_reply *reply)
{
	struct urgent_req *req;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *long_name;
	size_t long_size;
	struct ssh_sftp_attrs attrs;
	struct del_frame *f;
	struct sftp_dir_entry *e;
	struct sftp_dir_entry *grown;
	char nbuf[SSH_SFTP_PATH_MAX];
	char child[SSH_SFTP_PATH_MAX];
	size_t new_cap;
	size_t copy;
	size_t i;
	int is_dir;
	int is_link;
	int cdir;
	int clink;
	int cmp_dot;
	int cmp_dotdot;
	int error;

	/* Dispatches the reply according to the active urgent request kind. */
	req = w->active_urgent;
	switch (req->kind) {
	case URGENT_LIST_DIR:
		if (w->urgent_phase == 1) {
			if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(w->urgent_handle)) {
				sftp_dir_listing_free(w->building_listing);
				w->building_listing = NULL;
				worker_finish_urgent(w, EIO, "Could not open directory.");
				return;
			}

			/* Saves the directory handle and requests the first READDIR batch. */
			memcpy(w->urgent_handle, reply->handle, reply->handle_size);
			w->urgent_handle_size = reply->handle_size;
			w->urgent_phase = 2;
			error = sftp_client_readdir(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
			if (error != 0) {
				(void)sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
				w->urgent_phase = 3;
			}

			/* Waits for READDIR or CLOSEDIR. */
			return;
		}

		/* Processes a READDIR reply batch. */
		if (w->urgent_phase == 2) {
			if (reply->type == SSH_FXP_STATUS) {
				w->urgent_phase = 3;
				error = sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
				if (error != 0) {
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, 0, "");
				}

				/* Directory listing finished at EOF. */
				return;
			}

			/* Aborts on malformed directory listing replies. */
			if (reply->error != 0 || reply->type != SSH_FXP_NAME) {
				w->building_listing->error = EPROTO;
				w->urgent_phase = 3;
				error = sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
				if (error != 0) {
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, EPROTO, "Malformed directory listing.");
				}

				/* Waiting for CLOSEDIR or finished with error. */
				return;
			}

			/* Appends each entry in the NAME reply to the building listing. */
			for (;;) {
				error = sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs);
				if (error != 0)
					break;
				if (name_size == 0U || name_size >= sizeof(nbuf))
					continue;
				memcpy(nbuf, name, name_size);
				nbuf[name_size] = '\0';
				cmp_dot = strcmp(nbuf, ".");
				cmp_dotdot = strcmp(nbuf, "..");
				if (cmp_dot == 0 || cmp_dotdot == 0)
					continue;
				if (w->building_listing->count >= SFTP_CACHE_ENTRIES_MAX) {
					w->building_listing->truncated = 1;
					continue;
				}

				/* Grows the entries array if at capacity. */
				if (w->building_listing->count == w->building_cap) {
					if (w->building_cap == 0U)
						new_cap = 64U;
					else
						new_cap = w->building_cap * 2U;
					grown = realloc(w->building_listing->entries, new_cap * sizeof(*grown));
					if (grown == NULL) {
						w->building_listing->truncated = 1;
						continue;
					}

					/* Commits the grown entries array. */
					w->building_listing->entries = grown;
					w->building_cap = new_cap;
				}

				/* Populates the new directory entry. */
				e = &w->building_listing->entries[w->building_listing->count++];
				memset(e, 0, sizeof(*e));
				(void)snprintf(e->name, sizeof(e->name), "%s", nbuf);
				if (long_name != NULL && long_size != 0U) {
					if (long_size < sizeof(e->long_name) - 1U)
						copy = long_size;
					else
						copy = sizeof(e->long_name) - 1U;
					memcpy(e->long_name, long_name, copy);
					e->long_name[copy] = '\0';
				}

				/* Copies size, modification time, and permissions when present. */
				if ((attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0U)
					e->size = attrs.size;
				if ((attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0U)
					e->mtime = attrs.mtime;
				if ((attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U) {
					e->permissions = attrs.permissions;
					e->is_dir = (attrs.permissions & 0170000U) == 0040000U;
					e->is_link = (attrs.permissions & 0170000U) == 0120000U;
				}
			}

			/* Requests the next READDIR batch or closes the handle if truncated. */
			if (!w->building_listing->truncated)
				error = sftp_client_readdir(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
			if (w->building_listing->truncated || error != 0) {
				w->urgent_phase = 3;
				error = sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
				if (error != 0) {
					cache_store(w, w->building_listing);
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, 0, "");
				}
			}

			/* Waits for the next READDIR or CLOSEDIR reply. */
			return;
		}

		/* Completes the listing after CLOSEDIR (urgent_phase == 3). */
		cache_store(w, w->building_listing);
		req->listing = w->building_listing;
		w->building_listing = NULL;
		worker_finish_urgent(w, 0, "");
		return;

	case URGENT_MKDIR:
	case URGENT_RENAME:
		if (reply->error == 0 && reply->type == SSH_FXP_STATUS && reply->code == SSH_FX_OK) {
			worker_finish_urgent(w, 0, "");
		} else {
			worker_finish_urgent(w, EIO, "Operation failed.");
		}

		/* MKDIR or RENAME complete. */
		return;

	case URGENT_STAT:
		if (reply->error == 0 && reply->type == SSH_FXP_ATTRS) {
			memset(&req->entry, 0, sizeof(req->entry));
			if ((reply->attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0U)
				req->entry.size = reply->attrs.size;
			if ((reply->attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0U)
				req->entry.mtime = reply->attrs.mtime;
			if ((reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U) {
				req->entry.permissions = reply->attrs.permissions;
				req->entry.is_dir = (reply->attrs.permissions & 0170000U) == 0040000U;
				req->entry.is_link = (reply->attrs.permissions & 0170000U) == 0120000U;
			}

			/* Reports the populated entry to the caller. */
			worker_finish_urgent(w, 0, "");
		} else {
			worker_finish_urgent(w, ENOENT, "Stat failed.");
		}

		/* STAT complete. */
		return;

	case URGENT_DELETE_COUNT:
	case URGENT_DELETE:
		f = &w->del_stack[w->del_depth - 1U];
		if (w->urgent_phase == 1) {
			if (reply->error != 0 || reply->type != SSH_FXP_ATTRS) {
				del_stack_clear(w);
				worker_finish_urgent(w, ENOENT, "Could not inspect path.");
				return;
			}

			/* Classifies the root item from its LSTAT attributes. */
			is_dir = (reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (reply->attrs.permissions & 0170000U) == 0040000U;
			is_link = (reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (reply->attrs.permissions & 0170000U) == 0120000U;
			req->items++;
			if (w->del_depth == 1U)
				req->is_symlink = is_link;
			if (is_dir && !is_link) {
				req->folders++;
				error = sftp_client_opendir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not open directory.");
					return;
				}

				/* Waits for OPENDIR on the directory. */
				w->urgent_phase = 2;
				return;
			}

			/* Removes a non-directory or symlink directly when deleting. */
			if (req->kind == URGENT_DELETE) {
				error = sftp_client_remove(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not remove item.");
					return;
				}

				/* Waits for REMOVE completion. */
				w->urgent_phase = 5;
				return;
			}

			/* Pops the counted non-directory item and advances any parent. */
			del_stack_pop(w);
			if (w->del_depth == 0U) {
				worker_finish_urgent(w, 0, "");
				return;
			}

			/* Continues traversing remaining children on the stack. */
			worker_advance_delete(w, req);
			return;
		}

		/* Processes OPENDIR reply for a directory frame. */
		if (w->urgent_phase == 2) {
			if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(f->handle)) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not open directory.");
				return;
			}

			/* Saves the handle and starts reading directory entries. */
			memcpy(f->handle, reply->handle, reply->handle_size);
			f->handle_size = reply->handle_size;
			f->handle_open = 1;
			w->urgent_phase = 3;
			error = sftp_client_readdir(w->client, f->handle, f->handle_size, &w->urgent_req_id);
			if (error != 0) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not read directory.");
			}

			/* Waits for READDIR. */
			return;
		}

		/* Processes READDIR reply for a directory frame. */
		if (w->urgent_phase == 3) {
			if (reply->type == SSH_FXP_STATUS) {
				f->handle_open = 0;
				w->urgent_phase = 4;
				error = sftp_client_close_handle(w->client, f->handle, f->handle_size, &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not close directory.");
				}

				/* Waits for CLOSEDIR before descending into children. */
				return;
			}

			/* Aborts on malformed directory listing replies. */
			if (reply->error != 0 || reply->type != SSH_FXP_NAME) {
				del_stack_clear(w);
				worker_finish_urgent(w, EPROTO, "Malformed directory listing.");
				return;
			}

			/* Collects child paths in the current directory frame. */
			for (;;) {
				error = sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs);
				if (error != 0)
					break;
				if (name_size == 0U || name_size >= sizeof(nbuf))
					continue;
				memcpy(nbuf, name, name_size);
				nbuf[name_size] = '\0';
				cmp_dot = strcmp(nbuf, ".");
				cmp_dotdot = strcmp(nbuf, "..");
				if (cmp_dot == 0 || cmp_dotdot == 0)
					continue;
				error = sftp_transfer_join_remote(child, sizeof(child), f->path, nbuf);
				if (error != 0)
					continue;
				cdir = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
				    (attrs.permissions & 0170000U) == 0040000U;
				clink = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
				    (attrs.permissions & 0170000U) == 0120000U;
				(void)del_frame_add_child(f, child, cdir, clink);
			}

			/* Requests the next READDIR batch. */
			error = sftp_client_readdir(w->client, f->handle, f->handle_size, &w->urgent_req_id);
			if (error != 0) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not read directory.");
			}

			/* Waits for the next READDIR reply. */
			return;
		}

		/* Advances into children after CLOSEDIR (phase 4) or after REMOVE/RMDIR (phase 5). */
		if (w->urgent_phase == 5) {
			if (reply->error != 0 || reply->type != SSH_FXP_STATUS || reply->code != SSH_FX_OK) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Delete failed.");
				return;
			}

			/* Pops the removed child or directory frame. */
			del_stack_pop(w);
			if (w->del_depth == 0U) {
				worker_finish_urgent(w, 0, "");
				return;
			}
		}

		/* Continues processing remaining delete traversal frames. */
		worker_advance_delete(w, req);
		return;

	case URGENT_STREAM_OPEN:
		if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(w->streams[0].handle)) {
			worker_finish_urgent(w, EIO, "Could not open stream.");
			return;
		}

		/* Allocates a free stream slot and records the open handle. */
		for (i = 0U; i < WORKER_STREAMS_MAX; i++) {
			if (!w->streams[i].used) {
				w->streams[i].used = 1;
				w->streams[i].id = w->next_stream_id++;
				memcpy(w->streams[i].handle, reply->handle, reply->handle_size);
				w->streams[i].handle_size = reply->handle_size;
				req->stream_id = w->streams[i].id;
				worker_finish_urgent(w, 0, "");
				return;
			}
		}

		/* Reports ENOSPC when all stream slots are occupied. */
		worker_finish_urgent(w, ENOSPC, "Too many open streams.");
		return;

	case URGENT_STREAM_READ:
		if (reply->error == 0 && reply->type == SSH_FXP_DATA) {
			if (reply->data_size < req->size)
				copy = reply->data_size;
			else
				copy = req->size;
			if (copy != 0U)
				memcpy(req->buf, reply->data, copy);
			req->got = copy;
			worker_finish_urgent(w, 0, "");
		} else if (reply->type == SSH_FXP_STATUS && reply->code == SSH_FX_EOF) {
			req->got = 0U;
			worker_finish_urgent(w, 0, "");
		} else {
			worker_finish_urgent(w, EIO, "Stream read failed.");
		}

		/* STREAM_READ complete. */
		return;

	case URGENT_STREAM_CLOSE:
		worker_finish_urgent(w, 0, "");
		return;
	}
}

/* Advances the iterative delete/count traversal stack (caller holds w->lock). */
static void
worker_advance_delete(
	struct sftp_worker *w,
	struct urgent_req *req)
{
	struct del_frame *f;
	size_t idx;
	int cdir;
	int clink;
	const char *cpath;
	int error;

	/* Walks the delete stack until an async SFTP request is issued or the stack empties. */
	while (w->del_depth > 0U) {
		f = &w->del_stack[w->del_depth - 1U];
		if (f->child_index < f->child_count) {
			idx = f->child_index++;
			cdir = f->child_is_dir[idx];
			clink = f->child_is_link[idx];
			cpath = f->children[idx];

			/* Descends into non-symlink subdirectories. */
			if (cdir && !clink) {
				req->items++;
				req->folders++;
				error = del_stack_push(w, cpath);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, ENOMEM, "Out of memory.");
					return;
				}

				/* Opens the subdirectory for listing. */
				f = &w->del_stack[w->del_depth - 1U];
				error = sftp_client_opendir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not open directory.");
					return;
				}

				/* Waits for OPENDIR on the subdirectory. */
				w->urgent_phase = 2;
				return;
			}

			/* Counts and optionally removes a file or symlink child. */
			req->items++;
			if (req->kind == URGENT_DELETE) {
				error = del_stack_push(w, cpath);
				if (error == 0)
					error = sftp_client_remove(w->client, (const uint8_t *)cpath, strlen(cpath), &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not remove item.");
					return;
				}

				/* Waits for REMOVE on the child. */
				w->urgent_phase = 5;
				return;
			}
		} else {
			if (req->kind == URGENT_DELETE) {
				error = sftp_client_rmdir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id);
				if (error != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not remove directory.");
					return;
				}

				/* Waits for RMDIR on the emptied directory. */
				w->urgent_phase = 5;
				return;
			}

			/* Pops the counted directory frame. */
			del_stack_pop(w);
		}
	}

	/* All frames processed: finishes the delete or count request. */
	worker_finish_urgent(w, 0, "");
}

/* Marks the active urgent request complete and wakes its caller. */
static void
worker_finish_urgent(
	struct sftp_worker *w,
	int error,
	const char *err_text)
{
	struct urgent_req *req;

	/* Records the outcome on the active urgent request and wakes waiters. */
	req = w->active_urgent;
	if (req == NULL)
		return;
	req->error = error;
	if (err_text != NULL)
		(void)snprintf(req->error_text, sizeof(req->error_text), "%s", err_text);
	req->done = 1;
	w->active_urgent = NULL;
	kl_cond_broadcast(&w->done_cond);
}

/* Submits an urgent request and waits for the worker thread to finish it. */
static int
worker_submit_urgent(
	struct sftp_worker *w,
	struct urgent_req *req)
{
	/* Queues the request under the lock and waits for completion. */
	kl_mutex_lock(&w->lock);

	if (w->stop || w->state != SFTP_WORKER_READY) {
		kl_mutex_unlock(&w->lock);
		return ECONNRESET;
	}

	/* Appends the request to the tail of the urgent queue. */
	req->next = NULL;
	if (w->urgent_tail != NULL)
		w->urgent_tail->next = req;
	else
		w->urgent_head = req;
	w->urgent_tail = req;
	sftp_link_wake_worker(w->link);

	/* Waits until the worker marks the request done or stops. */
	while (!req->done && !w->stop)
		kl_cond_wait(&w->done_cond, &w->lock);

	kl_mutex_unlock(&w->lock);
	if (!req->done)
		return ECANCELED;
	if (req->error != 0)
		return req->error;

	/* Succeeded: the urgent request completed without error. */
	return 0;
}

/* Finds a cached directory listing by path and bumps its LRU stamp. */
static struct cache_slot *
cache_find(
	struct sftp_worker *w,
	const char *path)
{
	size_t i;
	int match;

	/* Scans cache slots for an active matching path. */
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (!w->cache[i].used)
			continue;
		match = strcmp(w->cache[i].path, path);
		if (match == 0) {
			w->cache[i].stamp = ++w->cache_clock;
			return &w->cache[i];
		}
	}

	/* Not cached. */
	return NULL;
}

/* Stores a directory listing in the cache, evicting old slots if over budget. */
static void
cache_store(
	struct sftp_worker *w,
	const struct sftp_dir_listing *listing)
{
	size_t i;
	size_t bytes;
	struct sftp_dir_entry *copy;
	struct cache_slot *slot;

	/* Ignores failed listings and invalidates any previous entry for the path. */
	if (listing == NULL || listing->error != 0)
		return;
	cache_invalidate(w, listing->path);

	/* Skips caching listings that exceed the global cache budget. */
	bytes = listing->count * sizeof(*listing->entries);
	if (listing->count > SFTP_CACHE_ENTRIES_MAX || bytes > SFTP_CACHE_BYTES_MAX)
		return;

	/* Evicts oldest slots until the new listing fits within budget. */
	while (w->cache_entries_total + listing->count > SFTP_CACHE_ENTRIES_MAX ||
	    w->cache_bytes_total + bytes > SFTP_CACHE_BYTES_MAX) {
		cache_evict_oldest(w);
	}

	/* Finds an unused cache slot, evicting one if all slots are full. */
	slot = NULL;
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (!w->cache[i].used) {
			slot = &w->cache[i];
			break;
		}
	}

	/* Evicts the oldest slot if no free slot was found. */
	if (slot == NULL) {
		cache_evict_oldest(w);
		for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
			if (!w->cache[i].used) {
				slot = &w->cache[i];
				break;
			}
		}
	}

	/* Gives up if no slot is available. */
	if (slot == NULL)
		return;

	/* Copies the directory entries into cache-owned memory. */
	copy = NULL;
	if (listing->count != 0U) {
		copy = malloc(bytes);
		if (copy == NULL)
			return;
		memcpy(copy, listing->entries, bytes);
	}

	/* Populates the cache slot and updates cache totals. */
	slot->used = 1;
	(void)snprintf(slot->path, sizeof(slot->path), "%s", listing->path);
	slot->entries = copy;
	slot->count = listing->count;
	slot->truncated = listing->truncated;
	slot->stamp = ++w->cache_clock;
	w->cache_entries_total += listing->count;
	w->cache_bytes_total += bytes;
}

/* Invalidates any cached listing matching path or its parent. */
static void
cache_invalidate(
	struct sftp_worker *w,
	const char *path)
{
	size_t i;
	int exact;
	int prefix;

	/* Clears any slot whose path equals or prefixes the modified path. */
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (!w->cache[i].used)
			continue;
		exact = strcmp(w->cache[i].path, path);
		prefix = strncmp(w->cache[i].path, path, strlen(w->cache[i].path));
		if (exact == 0 || prefix == 0) {
			w->cache_entries_total -= w->cache[i].count;
			w->cache_bytes_total -= w->cache[i].count * sizeof(*w->cache[i].entries);
			free(w->cache[i].entries);
			memset(&w->cache[i], 0, sizeof(w->cache[i]));
		}
	}
}

/* Frees all cached directory listings. */
static void
cache_clear(
	struct sftp_worker *w)
{
	size_t i;

	/* Releases every active cache slot. */
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (w->cache[i].used) {
			free(w->cache[i].entries);
			memset(&w->cache[i], 0, sizeof(w->cache[i]));
		}
	}

	/* Resets the global cache counters. */
	w->cache_entries_total = 0U;
	w->cache_bytes_total = 0U;
}

/* Evicts the least recently used cache slot. */
static void
cache_evict_oldest(
	struct sftp_worker *w)
{
	size_t i;
	size_t oldest;

	/* Finds the used slot with the smallest LRU stamp. */
	oldest = WORKER_CACHE_SLOTS;
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (w->cache[i].used && (oldest == WORKER_CACHE_SLOTS || w->cache[i].stamp < w->cache[oldest].stamp))
			oldest = i;
	}

	/* Frees the oldest slot if one was found. */
	if (oldest < WORKER_CACHE_SLOTS) {
		w->cache_entries_total -= w->cache[oldest].count;
		w->cache_bytes_total -= w->cache[oldest].count * sizeof(*w->cache[oldest].entries);
		free(w->cache[oldest].entries);
		memset(&w->cache[oldest], 0, sizeof(w->cache[oldest]));
	}
}

/* Clones a cached directory listing for a caller. */
static struct sftp_dir_listing *
listing_clone(
	const struct cache_slot *slot)
{
	struct sftp_dir_listing *out;

	/* Allocates the listing header and copies metadata. */
	out = calloc(1U, sizeof(*out));
	if (out == NULL)
		return NULL;
	(void)snprintf(out->path, sizeof(out->path), "%s", slot->path);
	out->count = slot->count;
	out->truncated = slot->truncated;

	/* Allocates and copies the entries array when non-empty. */
	if (slot->count != 0U) {
		out->entries = malloc(slot->count * sizeof(*out->entries));
		if (out->entries == NULL) {
			free(out);
			return NULL;
		}

		/* Copies the cached entry array into the caller's listing. */
		memcpy(out->entries, slot->entries, slot->count * sizeof(*out->entries));
	}

	/* Returns the cloned listing. */
	return out;
}

/* Clears the recursive delete traversal stack. */
static void
del_stack_clear(
	struct sftp_worker *w)
{
	size_t i;
	size_t j;

	/* Frees every frame's child lists. */
	for (i = 0U; i < w->del_cap; i++) {
		for (j = 0U; j < w->del_stack[i].child_count; j++)
			free(w->del_stack[i].children[j]);
		free(w->del_stack[i].children);
		free(w->del_stack[i].child_is_dir);
		free(w->del_stack[i].child_is_link);
	}

	/* Frees the stack array and resets stack counters. */
	free(w->del_stack);
	w->del_stack = NULL;
	w->del_depth = 0U;
	w->del_cap = 0U;
}

/* Pushes a directory or item onto the recursive delete stack. */
static int
del_stack_push(
	struct sftp_worker *w,
	const char *path)
{
	struct del_frame *grown;
	struct del_frame *f;
	size_t new_cap;

	/* Grows the delete stack if full. */
	if (w->del_depth == w->del_cap) {
		if (w->del_cap == 0U)
			new_cap = 8U;
		else
			new_cap = w->del_cap * 2U;
		grown = realloc(w->del_stack, new_cap * sizeof(*w->del_stack));
		if (grown == NULL)
			return ENOMEM;
		memset(grown + w->del_cap, 0, (new_cap - w->del_cap) * sizeof(*w->del_stack));
		w->del_stack = grown;
		w->del_cap = new_cap;
	}

	/* Initializes the new top frame with path. */
	f = &w->del_stack[w->del_depth++];
	memset(f, 0, sizeof(*f));
	(void)snprintf(f->path, sizeof(f->path), "%s", path);
	return 0;
}

/* Pops and frees the top frame of the recursive delete stack. */
static void
del_stack_pop(
	struct sftp_worker *w)
{
	struct del_frame *f;
	size_t j;

	/* Ignores an empty stack. */
	if (w->del_depth == 0U)
		return;

	/* Frees the top frame's child arrays and clears the frame. */
	f = &w->del_stack[--w->del_depth];
	for (j = 0U; j < f->child_count; j++)
		free(f->children[j]);
	free(f->children);
	free(f->child_is_dir);
	free(f->child_is_link);
	memset(f, 0, sizeof(*f));
}

/* Adds a child entry to a delete traversal frame. */
static int
del_frame_add_child(
	struct del_frame *f,
	const char *path,
	int is_dir,
	int is_link)
{
	char **names;
	int *dirs;
	int *links;
	size_t new_cap;
	char *dup;

	/* Grows the child arrays if full. */
	if (f->child_count == f->child_cap) {
		if (f->child_cap == 0U)
			new_cap = 16U;
		else
			new_cap = f->child_cap * 2U;
		names = realloc(f->children, new_cap * sizeof(*names));
		dirs = realloc(f->child_is_dir, new_cap * sizeof(*dirs));
		links = realloc(f->child_is_link, new_cap * sizeof(*links));
		if (names == NULL || dirs == NULL || links == NULL)
			return ENOMEM;
		f->children = names;
		f->child_is_dir = dirs;
		f->child_is_link = links;
		f->child_cap = new_cap;
	}

	/* Duplicates the child path and appends the metadata. */
	dup = malloc(strlen(path) + 1U);
	if (dup == NULL)
		return ENOMEM;
	memcpy(dup, path, strlen(path) + 1U);
	f->children[f->child_count] = dup;
	f->child_is_dir[f->child_count] = is_dir;
	f->child_is_link[f->child_count] = is_link;
	f->child_count++;
	return 0;
}

/* Closes and frees the current sftp_client instance. */
static void
worker_reset_client(
	struct sftp_worker *w)
{
	/* Closes the SFTP protocol client if allocated. */
	if (w->client != NULL) {
		sftp_client_close(w->client);
		w->client = NULL;
	}
}

/* Invokes the UI notify callback at most once every 100 ms unless forced. */
static void
worker_maybe_notify(
	struct sftp_worker *w,
	int force)
{
	uint64_t now;

	/* Skips notification if no callback is registered or nothing changed. */
	if (w->notify == NULL || !w->notify_dirty)
		return;

	/* Throttles non-forced notifications to WORKER_NOTIFY_MS. */
	now = worker_now_ms();
	if (!force && now - w->last_notify_ms < WORKER_NOTIFY_MS)
		return;
	w->notify_dirty = 0;
	w->last_notify_ms = now;
	w->notify(w->notify_data);
}

/* Returns a monotonic millisecond timestamp. */
static uint64_t
worker_now_ms(void)
{
	return kl_clock_us() / 1000U;
}
