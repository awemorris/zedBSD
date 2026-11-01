/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An instance of the engine (instance.c): what its views share and what
 * it remembers.  The program makes one with browser_instance_create of
 * <browser/browser.h> and gives it to the views it makes; a view made
 * without one uses the default instance of its thread, which remembers
 * nothing beyond the process (no backend).
 *
 * An instance holds its storage queue (storage/queue.h, the one thread
 * that calls its backend), its cookie jar (net/cookie.c, over the queue),
 * its persistent HTTP cache (net/cache.c, over the queue),
 * the Web Storage areas of its pages (page/storage.c, which reads and
 * writes the localStorage areas through the queue), the limit of its
 * persistent HTTP cache, and how many views use it.
 *
 * An instance and its views are used from one thread, the one that made
 * it (for a default instance, the thread it is the default of): the Web
 * Storage areas and the count of views are that thread's alone.  The
 * loaders' network threads use only the jar (under its lock) and the queue.
 */

#ifndef KEILAND_BROWSER_STORAGE_INSTANCE_H
#define KEILAND_BROWSER_STORAGE_INSTANCE_H

#include "base/base.h"

#include <browser/browser.h>

#include <stddef.h>

/* The limit of the persistent HTTP cache when the options give 0 (256 MiB). */
#define WB_INSTANCE_CACHE_LIMIT		((size_t)256U * 1024U * 1024U)

struct wb_storage_queue;
struct net_cookie_jar;
struct net_cache;

/*
 * One instance: made by browser_instance_create (or wb_instance_default)
 * and destroyed by browser_instance_destroy (or the end of its thread),
 * once no view uses it.
 *
 * queue:         the storage thread and queue over the backend (a queue
 *                without a backend when the instance has none).
 * cookies:       the cookie jar, over queue.
 * storage_areas: the Web Storage areas its pages made (page/storage.c
 *                owns what the pointers point to).
 * cache:        the persistent HTTP cache, over queue (net/cache.c); NULL
 *                for an instance without a backend, which keeps nothing.
 * cache_limit:   bytes of the persistent HTTP cache.
 * view_count:    the views made with it and not yet destroyed; an
 *                instance with views is not destroyed.
 * is_default:    1 for a thread's default instance, which the program
 *                does not see and browser_instance_destroy refuses.
 */
struct browser_instance {
	struct wb_storage_queue *queue;
	struct net_cookie_jar *cookies;
	struct net_cache *cache;
	struct wb_vector storage_areas;
	size_t cache_limit;
	size_t view_count;
	int is_default;
};

/*
 * Reports the calling thread's default instance, made the first time (no
 * backend: it remembers nothing beyond the process).  It is destroyed when
 * the thread ends if no view uses it then, and otherwise kept until the
 * process ends.  Returns 0 or ENOMEM.
 */
int wb_instance_default(struct browser_instance **instance);

#endif
