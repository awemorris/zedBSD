/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Imports and lists media without dispatching callbacks or modifying the UI's library. */
#include "media-worker.h"
#include <keiland/keiland.h>
#include <keiland/keiland_thread.h>
#include <wayland-client.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One operation is admitted at a time; its result must be consumed before another is queued. */
struct media_worker {
	struct kl_mutex lock;
	struct kl_cond wake;
	struct kl_thread *thread;
	char path[4096];
	int running;
	int stop;
	int queued;
	int busy;
	int ready;
	int notice;
	int removing;
	/* A duplicate Wayland descriptor permits close to interrupt a blocked request. */
	int cancel;
	struct ph_media_result result;
};

/* The Photos process owns one worker; its mutex protects jobs, cancellation and FD handoff. */
static struct media_worker worker = {KL_MUTEX_INIT,
				     {{{0}}},
				     NULL,
				     {0},
				     0,
				     0,
				     0,
				     0,
				     0,
				     0,
				     0,
				     -1,
				     {-1, 0, 0, 0, 0}};

/* Whether the worker's condition was made (0) or why it was not; set once by media_init. */
static struct kl_once media_once = KL_ONCE_INIT;
static int media_wake_error;

static void media_init(void);
static int media_ready(void);
static void media_main(void *unused);
static void media_run(const char *path, int notice, int removing, struct ph_media_result *result);
static int media_queue(const char *path, int notice, int removing);

/* Makes the worker's condition, once for the program. */
static void
media_init(void)
{
	/* The condition (a static one cannot be made by its bytes). */
	media_wake_error = kl_cond_init(&worker.wake);
}

/* Makes the worker's condition the first time: 0, or why it cannot be. */
static int
media_ready(void)
{
	/* Once for the program; every later call sees the first one's outcome. */
	kl_once(&media_once, media_init);
	return media_wake_error;
}

/*
 * Starts the background compositor client before any import is queued.
 */
int
ph_media_worker_start(
	void)
{
	struct kl_thread_options options;
	int error;

	/* A started worker retains its synchronization objects across requests. */
	if (worker.running)
		return 0;

	/* The condition the worker waits on is made before the worker exists. */
	error = media_ready();
	if (error != 0)
		return error;

	/* The thread: not stopped, no result held, its name, what it runs and its priority. */
	worker.stop = 0;
	worker.result.descriptor = -1;
	memset(&options, 0, sizeof(options));
	options.name = "photos-import";
	options.run = media_main;
	options.argument = NULL;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &worker.thread);
	if (error != 0)
		return error;

	/* The worker runs. */
	worker.running = 1;

	/* Succeeded: the UI can enqueue paths without performing any media IO. */
	return 0;
}

/*
 * Stops background requests and releases an unconsumed metadata snapshot.
 */
void
ph_media_worker_stop(
	void)
{
	/* Cancellation shuts down only this worker's Wayland connection, never the UI's. */
	if (!worker.running)
		return;

	/* The worker is told to stop and woken. */
	kl_mutex_lock(&worker.lock);

	worker.stop = 1;
	if (worker.cancel >= 0)
		(void)shutdown(worker.cancel, SHUT_RDWR);
	kl_cond_signal(&worker.wake);

	kl_mutex_unlock(&worker.lock);

	/* Waits for the worker to end. */
	(void)kl_thread_join(worker.thread);
	worker.thread = NULL;

	/* The worker no longer owns any live result or outstanding original path. */
	if (worker.result.descriptor >= 0)
		close(worker.result.descriptor);
	worker.result.descriptor = -1;
	worker.running = 0;
	worker.busy = 0;
	worker.ready = 0;
	worker.queued = 0;
}

/*
 * Enqueues an import path, or a metadata refresh when path is NULL.
 */
int
ph_media_worker_queue(
	const char *path,
	int notice)
{
	int error;

	/* Import and refresh retain the existing one-slot worker ownership. */
	error = media_queue(path, notice, 0);
	if (error != 0)
		return error;

	/* Succeeded: the worker owns the import or refresh request. */
	return 0;
}

/*
 * Enqueues deletion by identity without blocking the application's thread.
 */
int
ph_media_worker_remove(
	const char *id)
{
	int error;

	/* Copies the selected stable identity, never its mutable array index. */
	if (id == NULL || id[0] == '\0')
		return EINVAL;
	error = media_queue(id, 1, 1);
	if (error != 0)
		return error;

	/* Succeeded: only the private compositor client performs deletion. */
	return 0;
}

/*
 * Transfers a completed snapshot to the UI without waiting for a request.
 */
int
ph_media_worker_take(
	struct ph_media_result *result)
{
	/* A result becomes visible only after every worker write has completed. */
	kl_mutex_lock(&worker.lock);

	if (!worker.ready) {
		kl_mutex_unlock(&worker.lock);
		return 0;
	}

	/* The receiver takes the descriptor, allowing the next request to own the slot. */
	*result = worker.result;
	worker.result.descriptor = -1;
	worker.ready = 0;
	worker.busy = 0;

	kl_mutex_unlock(&worker.lock);

	/* Succeeded: the UI owns the completed request and its snapshot. */
	return 1;
}

/*
 * Reports whether a queued, running or completed request still occupies the
 * worker.
 */
int
ph_media_worker_busy(
	void)
{
	int busy;

	/* A short lock samples job state; it never spans compositor IO. */
	kl_mutex_lock(&worker.lock);

	busy = worker.busy;

	kl_mutex_unlock(&worker.lock);

	/* Succeeded: callers can select a short polling interval while work is pending. */
	return busy;
}

/* Copies one import, refresh or deletion job into the idle worker slot. */
static int
media_queue(
	const char *path,
	int notice,
	int removing)
{
	size_t length;
	int ready;

	/* Copies a chooser path or stable identity before its caller-owned storage expires. */
	length = 0U;
	if (path != NULL)
		length = strlen(path);
	if (length >= sizeof(worker.path))
		return ENAMETOOLONG;
	if (!worker.running)
		return ENOTSUP;

	/* The condition exists once the worker runs; a failure to make it leaves the signal out. */
	ready = media_ready();

	/* A new job cannot overwrite the identity, path or descriptor of the preceding request. */
	kl_mutex_lock(&worker.lock);

	if (worker.busy || worker.stop) {
		kl_mutex_unlock(&worker.lock);
		return EBUSY;
	}

	/* A single job is owned until its result is taken, including on failure. */
	worker.path[0] = '\0';
	if (path != NULL)
		memcpy(worker.path, path, length + 1U);
	worker.notice = notice;
	worker.removing = removing;
	worker.queued = 1;
	worker.busy = 1;
	if (ready == 0)
		kl_cond_signal(&worker.wake);

	kl_mutex_unlock(&worker.lock);

	/* Succeeded: only the copied job input crosses to the worker. */
	return 0;
}

/* Runs jobs using local copies while the UI continues drawing and processing input. */
static void
media_main(
	void *unused)
{
	char path[sizeof(worker.path)];
	struct ph_media_result result;
	int notice;
	int removing;

	/* The thread entry has no caller-owned context. */
	(void)unused;

	/* The worker sleeps until a job or close request is available. */
	for (;;) {
		kl_mutex_lock(&worker.lock);

		/* Spurious wakes do not turn an empty path into an unsolicited refresh. */
		while (!worker.stop && !worker.queued)
			kl_cond_wait(&worker.wake, &worker.lock);
		if (worker.stop) {
			kl_mutex_unlock(&worker.lock);
			break;
		}

		/* Private copies permit IO without holding the job lock. */
		memcpy(path, worker.path, sizeof(path));
		notice = worker.notice;
		removing = worker.removing;
		worker.queued = 0;

		kl_mutex_unlock(&worker.lock);

		/* Runs the job without the lock. */
		media_run(path, notice, removing, &result);

		/* Publishes a complete descriptor and status as one result. */
		kl_mutex_lock(&worker.lock);

		worker.result = result;
		worker.ready = 1;

		kl_mutex_unlock(&worker.lock);
	}

	/* Succeeded: close can now release any result not taken by the UI. */
}

/* Uses the public media API on a connection which is exclusively owned by this thread. */
static void
media_run(
	const char *path,
	int notice,
	int removing,
	struct ph_media_result *result)
{
	struct wl_display *display;
	struct kl_system *system;
	const char *paths[1];
	int cancel;
	int stopped;
	int error;

	/* Failed or partially successful imports still attempt to provide fresh metadata. */
	memset(result, 0, sizeof(*result));
	result->descriptor = -1;
	result->notice = notice;
	if (removing)
		result->removed = 1;
	else if (path[0] != '\0')
		result->imported = 1;
	display = wl_display_connect(NULL);
	if (display == NULL) {
		result->error = errno;
		return;
	}

	/* A duplicate FD lets UI close interrupt this private connection safely. */
	cancel = dup(wl_display_get_fd(display));
	if (cancel < 0) {
		result->error = errno;
		wl_display_disconnect(display);
		return;
	}

	/* Publish cancellation before the first blocking compositor handshake. */
	kl_mutex_lock(&worker.lock);

	worker.cancel = cancel;
	stopped = worker.stop;
	if (stopped)
		(void)shutdown(cancel, SHUT_RDWR);

	kl_mutex_unlock(&worker.lock);

	/* No shared display, system queue, widgets or library are dispatched here. */
	system = kl_system_open(display);
	if (system == NULL) {
		result->error = errno;
		if (result->error == 0)
			result->error = ENOTSUP;
	} else {
		if (result->removed) {
			/* The CLI resolves this identity in its latest locked database. */
			paths[0] = path;
			result->error = kl_system_media_remove_ids(system, paths, 1U, &result->descriptor);
		} else if (result->imported) {
			paths[0] = path;
			result->error = kl_system_media_add_paths(
			    system, paths, 1U, &result->descriptor);
		}

		/* Partial import failure must not hide originals that were already saved. */
		if (result->descriptor < 0) {
			error =
			    kl_system_media_list(system, &result->descriptor);
			if (result->error == 0)
				result->error = error;
		}

		/* Release the worker-owned protocol objects before disconnecting. */
		kl_system_close(system);
	}

	/* Retires the cancellation descriptor under the same lock used by close. */
	kl_mutex_lock(&worker.lock);

	worker.cancel = -1;
	close(cancel);

	kl_mutex_unlock(&worker.lock);
	wl_display_disconnect(display);
}
