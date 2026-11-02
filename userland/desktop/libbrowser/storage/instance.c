/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The instances of the engine (storage/instance.h): browser_instance_create
 * and browser_instance_destroy of <browser/browser.h>, and the process's
 * default instance.
 *
 * Making an instance starts its storage queue over the program's backend,
 * then opens its cookie jar, which reads the persistent cookies through
 * the queue, and (with a backend) its persistent HTTP cache, which reads
 * the cache's index.  Destroying it does everything queued, stops the queue's
 * thread and closes the backend (storage/queue.h), then frees the jar and
 * the Web Storage areas.
 *
 * A view made without an instance uses the process's default instance,
 * made with no backend on the first such view (or page) and kept for the
 * rest of the process.  It remembers nothing, so it needs no ending; the
 * views that share it are made and used on one thread at a time.
 */

#include "storage/instance.h"
#include "storage/queue.h"
#include "base/thread.h"
#include "net/net.h"
#include "page/page.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The process's default instance: NULL until the first view (or page) made
 * without an instance asks for it, then kept for the rest of the process.
 * Read and written only under instance_default_lock.
 */
static struct browser_instance *instance_default;

/* The lock of instance_default: two threads asking at once make only one. */
static struct wb_static_mutex instance_default_lock = WB_STATIC_MUTEX_INIT;

static int instance_make(const struct browser_storage_calls *storage, void *storage_context, size_t cache_limit, struct browser_instance **instance);
static void instance_free(struct browser_instance *instance);

/*
 * Makes an instance of the engine over a storage backend.
 *
 * The backend's close is called exactly once: when the instance is
 * destroyed, or before this returns when it fails.  Returns 0, EINVAL
 * (options or instance NULL), ENOTSUP (options or the backend's table of
 * another version, or a table missing a call), ENOMEM, EAGAIN (the
 * storage thread could not start).  A cookie record that cannot be read
 * leaves the jar without it; only a lack of memory fails.
 */
int
browser_instance_create(
	const struct browser_instance_options *options,
	struct browser_instance **instance)
{
	const struct browser_storage_calls *storage;
	void *storage_context;
	size_t cache_limit;
	int error;

	/* The backend, which is the instance's (or is closed) from now on. */
	storage = NULL;
	storage_context = NULL;
	if (options != NULL) {
		storage = options->storage;
		storage_context = options->storage_context;
	}

	/* Refuses a missing output slot or missing options, and options of another version. */
	error = 0;
	if (instance == NULL || options == NULL) {
		error = EINVAL;
	} else if (options->version != BROWSER_API_VERSION) {
		error = ENOTSUP;
	}
	if (instance != NULL)
		*instance = NULL;
	if (error != 0) {
		if (storage != NULL && storage->close != NULL)
			storage->close(storage_context);
		return error;
	}

	/* The limit of the persistent cache, 0 for the default. */
	cache_limit = options->cache_limit;
	if (cache_limit == 0)
		cache_limit = WB_INSTANCE_CACHE_LIMIT;

	/* The instance, its queue and its jar. */
	error = instance_make(storage, storage_context, cache_limit, instance);
	if (error != 0)
		return error;

	/* Succeeded: the instance is the caller's. */
	return 0;
}

/*
 * Destroys an instance: does everything its storage queue holds, closes
 * its backend and frees its cookies and Web Storage.  Returns 0, EBUSY (a
 * view made with it is not destroyed yet: nothing is done), or EINVAL
 * (NULL, or the process's default instance, which lasts as long as the
 * process).
 */
int
browser_instance_destroy(
	struct browser_instance *instance)
{
	/* Refuses no instance and a default one. */
	if (instance == NULL)
		return EINVAL;
	if (instance->is_default)
		return EINVAL;

	/* Refuses an instance a view still uses. */
	if (instance->view_count != 0)
		return EBUSY;

	/* Succeeded: the instance is gone. */
	instance_free(instance);
	return 0;
}

/*
 * Reports the process's default instance, made on its first use without a
 * backend.  Returns 0, ENOTSUP (the program gave no threads) or ENOMEM.
 */
int
wb_instance_default(
	struct browser_instance **instance)
{
	struct browser_instance *made;
	int error;

	/* The lock of the default. */
	*instance = NULL;
	error = wb_static_mutex_lock(&instance_default_lock);
	if (error != 0)
		return error;

	/* The default made before, or a new one, which remembers nothing. */
	made = instance_default;
	if (made == NULL) {
		error = instance_make(NULL, NULL, WB_INSTANCE_CACHE_LIMIT, &made);
		if (error == 0) {
			made->is_default = 1;
			instance_default = made;
		}
	}

	wb_static_mutex_unlock(&instance_default_lock);

	/* A default that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the process's default instance. */
	*instance = made;
	return 0;
}

/*
 * Makes an instance: its queue over a backend (NULL for none), whose close
 * is called on failure, then its jar, its cache and its empty areas.
 */
static int
instance_make(
	const struct browser_storage_calls *storage,
	void *storage_context,
	size_t cache_limit,
	struct browser_instance **instance)
{
	struct browser_instance *made;
	int error;

	/* The instance, empty. */
	made = calloc(1, sizeof(*made));
	if (made == NULL) {
		if (storage != NULL && storage->close != NULL)
			storage->close(storage_context);
		return ENOMEM;
	}
	made->cache_limit = cache_limit;
	wb_vector_init(&made->storage_areas, sizeof(void *));

	/* Its storage thread over the backend; on failure the backend is the caller's to close (storage/queue.h). */
	error = wb_storage_queue_open(storage, storage_context, &made->queue);
	if (error != 0) {
		free(made);
		if (storage != NULL && storage->close != NULL)
			storage->close(storage_context);
		return error;
	}

	/* Its cookie jar, with the persistent cookies the backend kept; closing the queue closes the backend. */
	error = net_cookie_jar_open(made->queue, &made->cookies);
	if (error != 0) {
		wb_storage_queue_close(made->queue);
		free(made);
		return error;
	}

	/* Its persistent HTTP cache and the index of its records; an instance without a backend keeps none. */
	if (storage != NULL) {
		error = net_cache_open(made->queue, cache_limit, &made->cache);
		if (error != 0) {
			net_cookie_jar_close(made->cookies);
			wb_storage_queue_close(made->queue);
			free(made);
			return error;
		}
	}

	/* Succeeded: the instance is made. */
	*instance = made;
	return 0;
}

/* Frees an instance: its queue done and closed (with the backend), then its jar and its areas. */
static void
instance_free(
	struct browser_instance *instance)
{
	/* The cache's index record, if a use changed it, is queued before the queue is closed. */
	net_cache_close(instance->cache);

	/* Everything queued is written, and the backend closed. */
	wb_storage_queue_close(instance->queue);

	/* The memory of the cookies and of the Web Storage areas. */
	net_cookie_jar_close(instance->cookies);
	page_storage_release(&instance->storage_areas);
	wb_vector_release(&instance->storage_areas);
	free(instance);
}
