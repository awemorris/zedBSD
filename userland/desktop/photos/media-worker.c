/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Imports and lists media without dispatching callbacks or modifying the UI's library. */
#include "media-worker.h"
#include <keiland/keiland.h>
#include <wayland-client.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One operation is admitted at a time; its result must be consumed before another is queued. */
struct media_worker {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	pthread_t thread;
	char path[4096];
	int running;
	int stop;
	int queued;
	int busy;
	int ready;
	int notice;
	/* A duplicate Wayland descriptor permits close to interrupt a blocked request. */
	int cancel;
	struct ph_media_result result;
};

/* The Photos process owns one worker; its mutex protects jobs, cancellation and FD handoff. */
static struct media_worker worker = {PTHREAD_MUTEX_INITIALIZER,
				     PTHREAD_COND_INITIALIZER,
				     0,
				     {0},
				     0,
				     0,
				     0,
				     0,
				     0,
				     0,
				     -1,
				     {-1, 0, 0, 0}};

static void *media_main(void *unused);
static void media_run(const char *path, int notice, struct ph_media_result *result);

/*
 * Starts the background compositor client before any import is queued.
 */
int
ph_media_worker_start(
	void)
{
	int error;

	/* A started worker retains its synchronization objects across requests. */
	if (worker.running)
		return 0;
	worker.stop = 0;
	worker.result.descriptor = -1;
	error = pthread_create(&worker.thread, NULL, media_main, NULL);
	if (error != 0)
		return error;
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
	pthread_mutex_lock(&worker.lock);

	worker.stop = 1;
	if (worker.cancel >= 0)
		(void)shutdown(worker.cancel, SHUT_RDWR);
	pthread_cond_signal(&worker.wake);

	pthread_mutex_unlock(&worker.lock);
	pthread_join(worker.thread, NULL);

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
	size_t length;

	/* Copies the chooser's path before its storage is destroyed. */
	length = 0U;
	if (path != NULL)
		length = strlen(path);
	if (length >= sizeof(worker.path))
		return ENAMETOOLONG;
	if (!worker.running)
		return ENOTSUP;
	pthread_mutex_lock(&worker.lock);

	/* An import cannot overwrite the path or descriptor of the preceding request. */
	if (worker.busy || worker.stop) {
		pthread_mutex_unlock(&worker.lock);
		return EBUSY;
	}

	/* A single job is owned until its result is taken, including on failure. */
	worker.path[0] = '\0';
	if (path != NULL)
		memcpy(worker.path, path, length + 1U);
	worker.notice = notice;
	worker.queued = 1;
	worker.busy = 1;
	pthread_cond_signal(&worker.wake);

	pthread_mutex_unlock(&worker.lock);

	/* Succeeded: only the copied path crosses to the worker. */
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
	pthread_mutex_lock(&worker.lock);

	if (!worker.ready) {
		pthread_mutex_unlock(&worker.lock);
		return 0;
	}

	/* The receiver takes the descriptor, allowing the next request to own the slot. */
	*result = worker.result;
	worker.result.descriptor = -1;
	worker.ready = 0;
	worker.busy = 0;

	pthread_mutex_unlock(&worker.lock);

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
	pthread_mutex_lock(&worker.lock);

	busy = worker.busy;

	pthread_mutex_unlock(&worker.lock);

	/* Succeeded: callers can select a short polling interval while work is pending. */
	return busy;
}

/* Runs jobs using local copies while the UI continues drawing and processing input. */
static void *
media_main(
	void *unused)
{
	char path[sizeof(worker.path)];
	struct ph_media_result result;
	int notice;

	/* The thread entry has no caller-owned context. */
	(void)unused;

	/* The worker sleeps until a job or close request is available. */
	for (;;) {
		pthread_mutex_lock(&worker.lock);

		/* Spurious wakes do not turn an empty path into an unsolicited refresh. */
		while (!worker.stop && !worker.queued)
			pthread_cond_wait(&worker.wake, &worker.lock);
		if (worker.stop) {
			pthread_mutex_unlock(&worker.lock);
			break;
		}

		/* Private copies permit IO without holding the job lock. */
		memcpy(path, worker.path, sizeof(path));
		notice = worker.notice;
		worker.queued = 0;

		pthread_mutex_unlock(&worker.lock);
		media_run(path, notice, &result);
		pthread_mutex_lock(&worker.lock);

		/* Publishes a complete descriptor and status as one result. */
		worker.result = result;
		worker.ready = 1;

		pthread_mutex_unlock(&worker.lock);
	}

	/* Succeeded: close can now release any result not taken by the UI. */
	return NULL;
}

/* Uses the public media API on a connection which is exclusively owned by this thread. */
static void
media_run(
	const char *path,
	int notice,
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
	if (path[0] != '\0')
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
	pthread_mutex_lock(&worker.lock);

	worker.cancel = cancel;
	stopped = worker.stop;
	if (stopped)
		(void)shutdown(cancel, SHUT_RDWR);

	pthread_mutex_unlock(&worker.lock);

	/* No shared display, system queue, widgets or library are dispatched here. */
	system = kl_system_open(display);
	if (system == NULL) {
		result->error = errno;
		if (result->error == 0)
			result->error = ENOTSUP;
	} else {
		if (result->imported) {
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
	pthread_mutex_lock(&worker.lock);

	worker.cancel = -1;
	close(cancel);

	pthread_mutex_unlock(&worker.lock);
	wl_display_disconnect(display);
}
