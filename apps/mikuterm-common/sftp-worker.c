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

	*worker_out = NULL;
	w = calloc(1U, sizeof(*w));
	if (w == NULL)
		return ENOMEM;

	error = sftp_transfer_queue_create(preserve_times, &w->transfers);
	if (error != 0) {
		free(w);
		return error;
	}
	error = kl_mutex_init(&w->lock);
	if (error != 0) {
		sftp_transfer_queue_destroy(w->transfers);
		free(w);
		return error;
	}
	error = kl_cond_init(&w->done_cond);
	if (error != 0) {
		kl_mutex_destroy(&w->lock);
		sftp_transfer_queue_destroy(w->transfers);
		free(w);
		return error;
	}

	sftp_link_retain(link);
	w->link = link;
	w->notify = notify;
	w->notify_data = notify_data;
	w->next_stream_id = 1U;
	w->state = SFTP_WORKER_OPENING;
	sftp_link_request_open(link);

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

	*worker_out = w;
	return 0;
}

/* Stops the worker thread, joins it, and frees all resources. */
void
sftp_worker_stop(
	struct sftp_worker *w)
{
	struct urgent_req *req;

	if (w == NULL)
		return;

	kl_mutex_lock(&w->lock);
	w->stop = 1;
	kl_mutex_unlock(&w->lock);
	sftp_link_request_close(w->link);
	sftp_link_wake_worker(w->link);

	(void)kl_thread_join(w->thread);

	while (w->urgent_head != NULL) {
		req = w->urgent_head;
		w->urgent_head = req->next;
		req->done = 1;
		req->error = ECANCELED;
	}
	del_stack_clear(w);
	cache_clear(w);
	worker_reset_client(w);
	sftp_transfer_queue_destroy(w->transfers);
	sftp_link_release(w->link);
	kl_cond_destroy(&w->done_cond);
	kl_mutex_destroy(&w->lock);
	free(w);
}

/* Retries opening the SFTP subsystem channel after a failure. */
void
sftp_worker_retry(
	struct sftp_worker *w)
{
	kl_mutex_lock(&w->lock);
	if (w->state == SFTP_WORKER_FAILED && !sftp_link_is_ended(w->link)) {
		w->retry_wanted = 1;
		w->state = SFTP_WORKER_OPENING;
		w->error_text[0] = '\0';
	}
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/* Reads the current worker state, home directory, and error message. */
void
sftp_worker_status(
	struct sftp_worker *w,
	enum sftp_worker_state *state,
	char *home,
	size_t home_cap,
	char *error_text,
	size_t err_cap)
{
	kl_mutex_lock(&w->lock);
	*state = w->state;
	if (home != NULL && home_cap != 0U)
		(void)snprintf(home, home_cap, "%s", w->home);
	if (error_text != NULL && err_cap != 0U)
		(void)snprintf(error_text, err_cap, "%s", w->error_text);
	kl_mutex_unlock(&w->lock);
}

/* Lists a remote directory, using the cache unless force_refresh is set. */
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

	*listing_out = NULL;
	if (!force_refresh) {
		kl_mutex_lock(&w->lock);
		slot = cache_find(w, path);
		if (slot != NULL) {
			*listing_out = listing_clone(slot);
			kl_mutex_unlock(&w->lock);
			return *listing_out != NULL ? 0 : ENOMEM;
		}
		kl_mutex_unlock(&w->lock);
	}

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_LIST_DIR;
	req.flag = force_refresh;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error != 0) {
		sftp_dir_listing_free(req.listing);
		return error;
	}
	*listing_out = req.listing;
	return 0;
}

/* Frees a directory listing returned by sftp_worker_list_dir. */
void
sftp_dir_listing_free(
	struct sftp_dir_listing *listing)
{
	if (listing == NULL)
		return;
	free(listing->entries);
	free(listing);
}

/* Creates a remote directory. */
int
sftp_worker_mkdir(
	struct sftp_worker *w,
	const char *path,
	char *err,
	size_t err_cap)
{
	struct urgent_req req;
	int error;

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_MKDIR;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	return error;
}

/* Renames a remote file or directory. */
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

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_RENAME;
	(void)snprintf(req.path, sizeof(req.path), "%s", old_path);
	(void)snprintf(req.path2, sizeof(req.path2), "%s", new_path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	return error;
}

/* Counts items and folders under path before recursive deletion (does not follow symlinks). */
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

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_DELETE_COUNT;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error == 0) {
		*items = req.items;
		*folders = req.folders;
		*is_symlink = req.is_symlink;
	}
	return error;
}

/* Recursively deletes path (symlinks are removed without following their target). */
int
sftp_worker_delete(
	struct sftp_worker *w,
	const char *path,
	char *err,
	size_t err_cap)
{
	struct urgent_req req;
	int error;

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_DELETE;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (err != NULL && err_cap != 0U)
		(void)snprintf(err, err_cap, "%s", req.error_text);
	return error;
}

/* Queries attributes of path (follow_links=1 uses STAT, 0 uses LSTAT). */
int
sftp_worker_stat(
	struct sftp_worker *w,
	const char *path,
	int follow_links,
	struct sftp_dir_entry *entry)
{
	struct urgent_req req;
	int error;

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STAT;
	req.flag = follow_links;
	(void)snprintf(req.path, sizeof(req.path), "%s", path);
	error = worker_submit_urgent(w, &req);
	if (error == 0)
		*entry = req.entry;
	return error;
}

/* Enqueues a download into local_dir. */
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

	slash = strrchr(remote_path, '/');
	base = slash != NULL ? slash + 1 : remote_path;
	error = sftp_local_join_path(local_path, sizeof(local_path), local_dir, base);
	if (error != 0)
		return error;

	kl_mutex_lock(&w->lock);
	error = sftp_transfer_enqueue(w->transfers, SFTP_TRANSFER_DOWNLOAD, local_path, remote_path, is_dir, id_out);
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
	return error;
}

/* Enqueues an upload into remote_dir. */
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

	slash = strrchr(local_path, '/');
	bslash = strrchr(local_path, '\\');
	base = slash;
	if (bslash != NULL && (base == NULL || bslash > base))
		base = bslash;
	base = base != NULL ? base + 1 : local_path;

	error = sftp_transfer_join_remote(remote_path, sizeof(remote_path), remote_dir, base);
	if (error != 0)
		return error;

	kl_mutex_lock(&w->lock);
	error = sftp_transfer_enqueue(w->transfers, SFTP_TRANSFER_UPLOAD, local_path, remote_path, is_dir, id_out);
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
	return error;
}

/* Resolves an overwrite conflict for transfer id. */
void
sftp_worker_resolve_conflict(
	struct sftp_worker *w,
	uint64_t id,
	enum sftp_conflict_action action)
{
	kl_mutex_lock(&w->lock);
	sftp_transfer_resolve_conflict(w->transfers, id, action);
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/* Cancels one transfer item. */
void
sftp_worker_cancel_transfer(
	struct sftp_worker *w,
	uint64_t id)
{
	kl_mutex_lock(&w->lock);
	sftp_transfer_cancel(w->transfers, id);
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/* Cancels all queued and active transfers. */
void
sftp_worker_cancel_all(
	struct sftp_worker *w)
{
	kl_mutex_lock(&w->lock);
	sftp_transfer_cancel_all(w->transfers);
	kl_mutex_unlock(&w->lock);
	sftp_link_wake_worker(w->link);
}

/* Copies a snapshot of the transfer queue. */
int
sftp_worker_transfers(
	struct sftp_worker *w,
	struct sftp_transfer_snapshot **items,
	size_t *count,
	size_t *active_or_queued)
{
	int error;

	kl_mutex_lock(&w->lock);
	error = sftp_transfer_snapshot_all(w->transfers, items, count, active_or_queued);
	kl_mutex_unlock(&w->lock);
	return error;
}

/* Frees a transfer snapshot array. */
void
sftp_worker_transfers_free(
	struct sftp_transfer_snapshot *items)
{
	free(items);
}

/* Opens a remote file for urgent Explorer stream reads. */
int
sftp_worker_stream_open(
	struct sftp_worker *w,
	const char *remote_path,
	uint64_t *stream_id)
{
	struct urgent_req req;
	int error;

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STREAM_OPEN;
	(void)snprintf(req.path, sizeof(req.path), "%s", remote_path);
	error = worker_submit_urgent(w, &req);
	if (error == 0)
		*stream_id = req.stream_id;
	return error;
}

/* Reads bytes at offset from an open Explorer stream (prioritized ahead of transfers). */
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

	*got = 0U;
	kl_mutex_lock(&w->lock);
	w->active_stream_reads++;
	kl_mutex_unlock(&w->lock);

	memset(&req, 0, sizeof(req));
	req.kind = URGENT_STREAM_READ;
	req.stream_id = stream_id;
	req.offset = offset;
	req.buf = buf;
	req.size = size;
	error = worker_submit_urgent(w, &req);
	if (error == 0)
		*got = req.got;

	kl_mutex_lock(&w->lock);
	w->active_stream_reads--;
	kl_mutex_unlock(&w->lock);
	return error;
}

/* Closes an open Explorer stream handle. */
void
sftp_worker_stream_close(
	struct sftp_worker *w,
	uint64_t stream_id)
{
	struct urgent_req req;

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
	struct sftp_worker *w = arg;

	for (;;) {
		kl_mutex_lock(&w->lock);
		if (w->stop) {
			kl_mutex_unlock(&w->lock);
			break;
		}
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
	int error;

	if (sftp_link_is_ended(w->link)) {
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
		worker_maybe_notify(w, 1);
		return;
	}

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
			if (code != 0U)
				(void)snprintf(w->error_text, sizeof(w->error_text),
				    "The server refused another session (MaxSessions on the server may be 1).");
			else if (reason[0] != '\0')
				(void)snprintf(w->error_text, sizeof(w->error_text), "%s", reason);
			else
				(void)snprintf(w->error_text, sizeof(w->error_text),
				    "The server has no SFTP subsystem.");
			w->notify_dirty = 1;
			kl_mutex_unlock(&w->lock);
			worker_maybe_notify(w, 1);
			return;
		}
		if (w->client == NULL) {
			error = sftp_client_open(&w->client);
			if (error != 0) {
				kl_mutex_lock(&w->lock);
				w->state = SFTP_WORKER_FAILED;
				(void)snprintf(w->error_text, sizeof(w->error_text), "Out of memory.");
				w->notify_dirty = 1;
				kl_mutex_unlock(&w->lock);
				worker_maybe_notify(w, 1);
				return;
			}
		}
	}

	if (w->client == NULL)
		return;

	worker_pump_io(w);

	if (w->state == SFTP_WORKER_READY) {
		kl_mutex_lock(&w->lock);
		if (w->active_urgent == NULL && w->urgent_head != NULL && sftp_client_room(w->client) > 0U)
			worker_start_urgent(w);
		if (sftp_transfer_has_work(w->transfers)) {
			unsigned max_reqs = w->active_stream_reads > 0U ?
			    SFTP_TRANSFER_SHARED_REQUESTS : SFTP_CLIENT_REQUESTS;
			sftp_transfer_step(w->transfers, w->client, max_reqs, &w->notify_dirty);
		}
		kl_mutex_unlock(&w->lock);
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
	size_t taken;
	size_t out_size;
	size_t written;
	const uint8_t *out_bytes;
	int eof;
	int closed;
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
			size_t offset = 0U;
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
						if (!w->version_seen)
							(void)snprintf(w->error_text, sizeof(w->error_text),
							    "The server did not answer as an SFTP server (a shell startup file may print text).");
						else
							(void)snprintf(w->error_text, sizeof(w->error_text),
							    "The SFTP protocol stream failed.");
						sftp_transfer_abort_active(w->transfers, w->error_text, &w->notify_dirty);
						if (w->active_urgent != NULL)
							worker_finish_urgent(w, EPROTO, w->error_text);
						w->notify_dirty = 1;
						kl_mutex_unlock(&w->lock);
						sftp_link_request_close(w->link);
						worker_maybe_notify(w, 1);
						return;
					}
					worker_handle_reply(w, &reply);
				}
				if (taken == 0U)
					break;
			}
		}
		if ((eof || closed) && got == 0U) {
			if (w->state != SFTP_WORKER_FAILED && w->state != SFTP_WORKER_ENDED) {
				kl_mutex_lock(&w->lock);
				w->state = sftp_link_is_ended(w->link) ? SFTP_WORKER_ENDED : SFTP_WORKER_FAILED;
				if (w->error_text[0] == '\0')
					(void)snprintf(w->error_text, sizeof(w->error_text), "The SFTP channel closed.");
				sftp_transfer_abort_active(w->transfers, w->error_text, &w->notify_dirty);
				if (w->active_urgent != NULL)
					worker_finish_urgent(w, ECONNRESET, w->error_text);
				w->notify_dirty = 1;
				kl_mutex_unlock(&w->lock);
				worker_maybe_notify(w, 1);
			}
			break;
		}
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

	if (reply->type == SSH_FXP_VERSION) {
		w->version_seen = 1;
		if (sftp_client_realpath(w->client, (const uint8_t *)".", 1U, &w->home_req_id) == 0) {
			w->home_pending = 1;
		} else {
			kl_mutex_lock(&w->lock);
			w->state = SFTP_WORKER_READY;
			(void)snprintf(w->home, sizeof(w->home), "/");
			w->notify_dirty = 1;
			kl_mutex_unlock(&w->lock);
			worker_maybe_notify(w, 1);
		}
		return;
	}

	if (w->home_pending && reply->id == w->home_req_id) {
		w->home_pending = 0;
		kl_mutex_lock(&w->lock);
		if (reply->error == 0 && reply->type == SSH_FXP_NAME &&
		    sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs) == 0 &&
		    name_size < sizeof(w->home)) {
			memcpy(w->home, name, name_size);
			w->home[name_size] = '\0';
		} else {
			(void)snprintf(w->home, sizeof(w->home), "/");
		}
		w->state = SFTP_WORKER_READY;
		w->notify_dirty = 1;
		kl_mutex_unlock(&w->lock);
		worker_maybe_notify(w, 1);
		return;
	}

	kl_mutex_lock(&w->lock);
	if (w->active_urgent != NULL && reply->id == w->urgent_req_id) {
		worker_urgent_reply(w, reply);
		kl_mutex_unlock(&w->lock);
		return;
	}
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
	int error;

	req = w->urgent_head;
	w->urgent_head = req->next;
	if (w->urgent_head == NULL)
		w->urgent_tail = NULL;
	req->next = NULL;
	w->active_urgent = req;
	w->urgent_phase = 0;

	switch (req->kind) {
	case URGENT_LIST_DIR:
		w->building_listing = calloc(1U, sizeof(*w->building_listing));
		if (w->building_listing == NULL) {
			worker_finish_urgent(w, ENOMEM, "Out of memory.");
			return;
		}
		(void)snprintf(w->building_listing->path, sizeof(w->building_listing->path), "%s", req->path);
		w->building_cap = 0U;
		error = sftp_client_opendir(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		if (error != 0) {
			sftp_dir_listing_free(w->building_listing);
			w->building_listing = NULL;
			worker_finish_urgent(w, error, "Could not open directory.");
			return;
		}
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
		error = sftp_client_rename(w->client, (const uint8_t *)req->path, strlen(req->path),
		    (const uint8_t *)req->path2, strlen(req->path2), &w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not rename.");
		break;

	case URGENT_DELETE_COUNT:
	case URGENT_DELETE:
		del_stack_clear(w);
		cache_invalidate(w, req->path);
		if (del_stack_push(w, req->path) != 0) {
			worker_finish_urgent(w, ENOMEM, "Out of memory.");
			return;
		}
		error = sftp_client_lstat(w->client, (const uint8_t *)req->path, strlen(req->path), &w->urgent_req_id);
		if (error != 0) {
			del_stack_clear(w);
			worker_finish_urgent(w, error, "Could not inspect path.");
			return;
		}
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
		error = sftp_client_open_file(w->client, (const uint8_t *)req->path, strlen(req->path),
		    SSH_FXF_READ, NULL, &w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not open stream.");
		break;

	case URGENT_STREAM_READ:
		for (i = 0U; i < WORKER_STREAMS_MAX; i++) {
			if (w->streams[i].used && w->streams[i].id == req->stream_id)
				break;
		}
		if (i == WORKER_STREAMS_MAX) {
			worker_finish_urgent(w, EBADF, "Invalid stream.");
			return;
		}
		error = sftp_client_read(w->client, w->streams[i].handle, w->streams[i].handle_size,
		    req->offset, (uint32_t)(req->size > SFTP_CLIENT_CHUNK ? SFTP_CLIENT_CHUNK : req->size),
		    &w->urgent_req_id);
		if (error != 0)
			worker_finish_urgent(w, error, "Could not read stream.");
		break;

	case URGENT_STREAM_CLOSE:
		for (i = 0U; i < WORKER_STREAMS_MAX; i++) {
			if (w->streams[i].used && w->streams[i].id == req->stream_id)
				break;
		}
		if (i == WORKER_STREAMS_MAX) {
			worker_finish_urgent(w, 0, "");
			return;
		}
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
	struct urgent_req *req = w->active_urgent;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *long_name;
	size_t long_size;
	struct ssh_sftp_attrs attrs;
	struct del_frame *f;
	size_t i;

	switch (req->kind) {
	case URGENT_LIST_DIR:
		if (w->urgent_phase == 1) {
			if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(w->urgent_handle)) {
				sftp_dir_listing_free(w->building_listing);
				w->building_listing = NULL;
				worker_finish_urgent(w, EIO, "Could not open directory.");
				return;
			}
			memcpy(w->urgent_handle, reply->handle, reply->handle_size);
			w->urgent_handle_size = reply->handle_size;
			w->urgent_phase = 2;
			if (sftp_client_readdir(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id) != 0) {
				(void)sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id);
				w->urgent_phase = 3;
			}
			return;
		}
		if (w->urgent_phase == 2) {
			if (reply->type == SSH_FXP_STATUS) {
				w->urgent_phase = 3;
				if (sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id) != 0) {
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, 0, "");
				}
				return;
			}
			if (reply->error != 0 || reply->type != SSH_FXP_NAME) {
				w->building_listing->error = EPROTO;
				w->urgent_phase = 3;
				if (sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id) != 0) {
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, EPROTO, "Malformed directory listing.");
				}
				return;
			}
			while (sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs) == 0) {
				struct sftp_dir_entry *e;
				char nbuf[SSH_SFTP_PATH_MAX];

				if (name_size == 0U || name_size >= sizeof(nbuf))
					continue;
				memcpy(nbuf, name, name_size);
				nbuf[name_size] = '\0';
				if (strcmp(nbuf, ".") == 0 || strcmp(nbuf, "..") == 0)
					continue;
				if (w->building_listing->count >= SFTP_CACHE_ENTRIES_MAX) {
					w->building_listing->truncated = 1;
					continue;
				}
				if (w->building_listing->count == w->building_cap) {
					size_t new_cap = w->building_cap == 0U ? 64U : w->building_cap * 2U;
					struct sftp_dir_entry *grown = realloc(w->building_listing->entries, new_cap * sizeof(*grown));
					if (grown == NULL) {
						w->building_listing->truncated = 1;
						continue;
					}
					w->building_listing->entries = grown;
					w->building_cap = new_cap;
				}
				e = &w->building_listing->entries[w->building_listing->count++];
				memset(e, 0, sizeof(*e));
				(void)snprintf(e->name, sizeof(e->name), "%s", nbuf);
				if (long_name != NULL && long_size != 0U) {
					size_t copy = long_size < sizeof(e->long_name) - 1U ? long_size : sizeof(e->long_name) - 1U;
					memcpy(e->long_name, long_name, copy);
					e->long_name[copy] = '\0';
				}
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
			if (w->building_listing->truncated ||
			    sftp_client_readdir(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id) != 0) {
				w->urgent_phase = 3;
				if (sftp_client_close_handle(w->client, w->urgent_handle, w->urgent_handle_size, &w->urgent_req_id) != 0) {
					cache_store(w, w->building_listing);
					req->listing = w->building_listing;
					w->building_listing = NULL;
					worker_finish_urgent(w, 0, "");
				}
			}
			return;
		}
		/* urgent_phase == 3: CLOSEDIR complete */
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
			worker_finish_urgent(w, 0, "");
		} else {
			worker_finish_urgent(w, ENOENT, "Stat failed.");
		}
		return;

	case URGENT_DELETE_COUNT:
	case URGENT_DELETE:
		f = &w->del_stack[w->del_depth - 1U];
		if (w->urgent_phase == 1) {
			/* LSTAT result for top of del_stack */
			int is_dir;
			int is_link;

			if (reply->error != 0 || reply->type != SSH_FXP_ATTRS) {
				del_stack_clear(w);
				worker_finish_urgent(w, ENOENT, "Could not inspect path.");
				return;
			}
			is_dir = (reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (reply->attrs.permissions & 0170000U) == 0040000U;
			is_link = (reply->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
			    (reply->attrs.permissions & 0170000U) == 0120000U;
			req->items++;
			if (w->del_depth == 1U)
				req->is_symlink = is_link;
			if (is_dir && !is_link) {
				req->folders++;
				if (sftp_client_opendir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id) != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not open directory.");
					return;
				}
				w->urgent_phase = 2;
				return;
			}
			/* File or symlink: if deleting, REMOVE it; otherwise pop. */
			if (req->kind == URGENT_DELETE) {
				if (sftp_client_remove(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id) != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not remove item.");
					return;
				}
				w->urgent_phase = 5;
				return;
			}
			del_stack_pop(w);
			if (w->del_depth == 0U) {
				worker_finish_urgent(w, 0, "");
				return;
			}
			/* Advance parent's child_index */
			goto advance_del_parent;
		}
		if (w->urgent_phase == 2) {
			/* OPENDIR result */
			if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(f->handle)) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not open directory.");
				return;
			}
			memcpy(f->handle, reply->handle, reply->handle_size);
			f->handle_size = reply->handle_size;
			f->handle_open = 1;
			w->urgent_phase = 3;
			if (sftp_client_readdir(w->client, f->handle, f->handle_size, &w->urgent_req_id) != 0) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not read directory.");
			}
			return;
		}
		if (w->urgent_phase == 3) {
			/* READDIR result */
			if (reply->type == SSH_FXP_STATUS) {
				f->handle_open = 0;
				w->urgent_phase = 4;
				if (sftp_client_close_handle(w->client, f->handle, f->handle_size, &w->urgent_req_id) != 0) {
					del_stack_clear(w);
					worker_finish_urgent(w, EIO, "Could not close directory.");
				}
				return;
			}
			if (reply->error != 0 || reply->type != SSH_FXP_NAME) {
				del_stack_clear(w);
				worker_finish_urgent(w, EPROTO, "Malformed directory listing.");
				return;
			}
			while (sftp_client_name_next(w->client, &name, &name_size, &long_name, &long_size, &attrs) == 0) {
				char nbuf[SSH_SFTP_PATH_MAX];
				char child[SSH_SFTP_PATH_MAX];
				int cdir;
				int clink;

				if (name_size == 0U || name_size >= sizeof(nbuf))
					continue;
				memcpy(nbuf, name, name_size);
				nbuf[name_size] = '\0';
				if (strcmp(nbuf, ".") == 0 || strcmp(nbuf, "..") == 0)
					continue;
				if (sftp_transfer_join_remote(child, sizeof(child), f->path, nbuf) != 0)
					continue;
				cdir = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
				    (attrs.permissions & 0170000U) == 0040000U;
				clink = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0U &&
				    (attrs.permissions & 0170000U) == 0120000U;
				(void)del_frame_add_child(f, child, cdir, clink);
			}
			if (sftp_client_readdir(w->client, f->handle, f->handle_size, &w->urgent_req_id) != 0) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Could not read directory.");
			}
			return;
		}
		if (w->urgent_phase == 4) {
			/* CLOSEDIR complete: process children of top frame. */
			goto process_del_children;
		}
		if (w->urgent_phase == 5) {
			/* REMOVE or RMDIR complete */
			if (reply->error != 0 || reply->type != SSH_FXP_STATUS || reply->code != SSH_FX_OK) {
				del_stack_clear(w);
				worker_finish_urgent(w, EIO, "Delete failed.");
				return;
			}
			del_stack_pop(w);
			if (w->del_depth == 0U) {
				worker_finish_urgent(w, 0, "");
				return;
			}
		}

advance_del_parent:
process_del_children:
		while (w->del_depth > 0U) {
			f = &w->del_stack[w->del_depth - 1U];
			if (f->child_index < f->child_count) {
				size_t idx = f->child_index++;
				int cdir = f->child_is_dir[idx];
				int clink = f->child_is_link[idx];
				const char *cpath = f->children[idx];

				if (cdir && !clink) {
					req->items++;
					req->folders++;
					if (del_stack_push(w, cpath) != 0) {
						del_stack_clear(w);
						worker_finish_urgent(w, ENOMEM, "Out of memory.");
						return;
					}
					f = &w->del_stack[w->del_depth - 1U];
					if (sftp_client_opendir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id) != 0) {
						del_stack_clear(w);
						worker_finish_urgent(w, EIO, "Could not open directory.");
						return;
					}
					w->urgent_phase = 2;
					return;
				}
				req->items++;
				if (req->kind == URGENT_DELETE) {
					if (del_stack_push(w, cpath) != 0 ||
					    sftp_client_remove(w->client, (const uint8_t *)cpath, strlen(cpath), &w->urgent_req_id) != 0) {
						del_stack_clear(w);
						worker_finish_urgent(w, EIO, "Could not remove item.");
						return;
					}
					w->urgent_phase = 5;
					return;
				}
			} else {
				if (req->kind == URGENT_DELETE) {
					if (sftp_client_rmdir(w->client, (const uint8_t *)f->path, strlen(f->path), &w->urgent_req_id) != 0) {
						del_stack_clear(w);
						worker_finish_urgent(w, EIO, "Could not remove directory.");
						return;
					}
					w->urgent_phase = 5;
					return;
				}
				del_stack_pop(w);
			}
		}
		worker_finish_urgent(w, 0, "");
		return;

	case URGENT_STREAM_OPEN:
		if (reply->error != 0 || reply->type != SSH_FXP_HANDLE || reply->handle_size > sizeof(w->streams[0].handle)) {
			worker_finish_urgent(w, EIO, "Could not open stream.");
			return;
		}
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
		worker_finish_urgent(w, ENOSPC, "Too many open streams.");
		return;

	case URGENT_STREAM_READ:
		if (reply->error == 0 && reply->type == SSH_FXP_DATA) {
			size_t copy = reply->data_size < req->size ? reply->data_size : req->size;
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
		return;

	case URGENT_STREAM_CLOSE:
		worker_finish_urgent(w, 0, "");
		return;
	}
}

/* Marks the active urgent request complete and wakes its caller. */
static void
worker_finish_urgent(
	struct sftp_worker *w,
	int error,
	const char *err_text)
{
	struct urgent_req *req = w->active_urgent;

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
	kl_mutex_lock(&w->lock);
	if (w->stop || w->state != SFTP_WORKER_READY) {
		kl_mutex_unlock(&w->lock);
		return ECONNRESET;
	}
	req->next = NULL;
	if (w->urgent_tail != NULL)
		w->urgent_tail->next = req;
	else
		w->urgent_head = req;
	w->urgent_tail = req;
	sftp_link_wake_worker(w->link);

	while (!req->done && !w->stop)
		kl_cond_wait(&w->done_cond, &w->lock);
	kl_mutex_unlock(&w->lock);
	return req->done ? req->error : ECANCELED;
}

/* Finds a cached directory listing by path and bumps its LRU stamp. */
static struct cache_slot *
cache_find(
	struct sftp_worker *w,
	const char *path)
{
	size_t i;

	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (w->cache[i].used && strcmp(w->cache[i].path, path) == 0) {
			w->cache[i].stamp = ++w->cache_clock;
			return &w->cache[i];
		}
	}
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

	if (listing == NULL || listing->error != 0)
		return;
	cache_invalidate(w, listing->path);

	bytes = listing->count * sizeof(*listing->entries);
	if (listing->count > SFTP_CACHE_ENTRIES_MAX || bytes > SFTP_CACHE_BYTES_MAX)
		return;

	while (w->cache_entries_total + listing->count > SFTP_CACHE_ENTRIES_MAX ||
	    w->cache_bytes_total + bytes > SFTP_CACHE_BYTES_MAX) {
		cache_evict_oldest(w);
	}

	slot = NULL;
	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (!w->cache[i].used) {
			slot = &w->cache[i];
			break;
		}
	}
	if (slot == NULL) {
		cache_evict_oldest(w);
		for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
			if (!w->cache[i].used) {
				slot = &w->cache[i];
				break;
			}
		}
	}
	if (slot == NULL)
		return;

	copy = NULL;
	if (listing->count != 0U) {
		copy = malloc(bytes);
		if (copy == NULL)
			return;
		memcpy(copy, listing->entries, bytes);
	}

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

	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (!w->cache[i].used)
			continue;
		if (strcmp(w->cache[i].path, path) == 0 ||
		    strncmp(w->cache[i].path, path, strlen(w->cache[i].path)) == 0) {
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

	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (w->cache[i].used) {
			free(w->cache[i].entries);
			memset(&w->cache[i], 0, sizeof(w->cache[i]));
		}
	}
	w->cache_entries_total = 0U;
	w->cache_bytes_total = 0U;
}

/* Evicts the least recently used cache slot. */
static void
cache_evict_oldest(
	struct sftp_worker *w)
{
	size_t i;
	size_t oldest = WORKER_CACHE_SLOTS;

	for (i = 0U; i < WORKER_CACHE_SLOTS; i++) {
		if (w->cache[i].used && (oldest == WORKER_CACHE_SLOTS || w->cache[i].stamp < w->cache[oldest].stamp))
			oldest = i;
	}
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

	out = calloc(1U, sizeof(*out));
	if (out == NULL)
		return NULL;
	(void)snprintf(out->path, sizeof(out->path), "%s", slot->path);
	out->count = slot->count;
	out->truncated = slot->truncated;
	if (slot->count != 0U) {
		out->entries = malloc(slot->count * sizeof(*out->entries));
		if (out->entries == NULL) {
			free(out);
			return NULL;
		}
		memcpy(out->entries, slot->entries, slot->count * sizeof(*out->entries));
	}
	return out;
}

/* Clears the recursive delete traversal stack. */
static void
del_stack_clear(
	struct sftp_worker *w)
{
	size_t i;
	size_t j;

	for (i = 0U; i < w->del_cap; i++) {
		for (j = 0U; j < w->del_stack[i].child_count; j++)
			free(w->del_stack[i].children[j]);
		free(w->del_stack[i].children);
		free(w->del_stack[i].child_is_dir);
		free(w->del_stack[i].child_is_link);
	}
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

	if (w->del_depth == w->del_cap) {
		new_cap = w->del_cap == 0U ? 8U : w->del_cap * 2U;
		grown = realloc(w->del_stack, new_cap * sizeof(*w->del_stack));
		if (grown == NULL)
			return ENOMEM;
		memset(grown + w->del_cap, 0, (new_cap - w->del_cap) * sizeof(*w->del_stack));
		w->del_stack = grown;
		w->del_cap = new_cap;
	}
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

	if (w->del_depth == 0U)
		return;
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

	if (f->child_count == f->child_cap) {
		new_cap = f->child_cap == 0U ? 16U : f->child_cap * 2U;
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

	if (w->notify == NULL || !w->notify_dirty)
		return;
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
