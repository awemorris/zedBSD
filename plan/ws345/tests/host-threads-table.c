/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the table of the program's threads
 * (browser_threads_set of <browser/browser.h>), through the public
 * interface only:
 *
 *  - before the table is given, an instance and a view are refused
 *    (ENOTSUP);
 *  - a missing table, one of another version and one missing a call are
 *    refused, the first good one is kept and a second one is refused
 *    (EBUSY);
 *  - a view whose second thread cannot start fails (EAGAIN) and gives back
 *    the thread that did start;
 *  - an instance over a backend and a view start the engine's named
 *    threads (browser-storage, browser-resolve, browser-net) through the
 *    table, and every thread started is joined when they are destroyed.
 *
 * The table wraps the POSIX threads' (userland/tests/browser-threads):
 * it counts the starts and the joins, keeps the names, and can refuse a
 * start.
 *
 *   host-threads-table
 *
 * Run from the repository's root (the fonts are the checked-in ones).
 * Prints one line per failed check and a summary.
 */

#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most names the table keeps. */
#define TABLE_NAMES_MAX		32U

/* The longest name kept, with its NUL. */
#define TABLE_NAME_SIZE		16U

static unsigned checks;
static unsigned failures;

/* The starts and joins through the table, the names of the started threads, and the start to refuse (0 for none). */
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned table_starts;
static unsigned table_joins;
static unsigned table_refuse_at;
static char table_names[TABLE_NAMES_MAX][TABLE_NAME_SIZE];
static unsigned table_name_count;

static int table_start(void *context, const char *name, size_t stack_size, void (*run)(void *argument), void *argument, void **thread);
static void table_join(void *context, void *thread);
static void table_check(int condition, const char *what);
static int table_named(const char *name);
static int fake_read(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
static int fake_write(void *context, unsigned space, const char *key, const void *bytes, size_t length);
static int fake_remove(void *context, unsigned space, const char *key);
static int fake_list(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int fake_clear(void *context, unsigned space);
static void fake_close(void *context);

/* A backend that keeps nothing, so that the instance has a storage thread. */
static const struct browser_storage_calls fake_calls = {
	BROWSER_STORAGE_VERSION,
	fake_read,
	fake_write,
	fake_remove,
	fake_list,
	fake_clear,
	fake_close
};

int
main(
	void)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	struct browser_instance_options instance_options;
	struct browser_view_options view_options;
	struct browser_threads table;
	struct browser_threads wrong;
	struct browser_instance *instance;
	struct browser_view *view;
	unsigned starts;
	unsigned joins;
	int error;

	/* Storage stays under build/. */
	setenv("XDG_DATA_HOME", "build/ws345-threads-data", 1);

	/* The options of the instance and of the view. */
	memset(&instance_options, 0, sizeof(instance_options));
	instance_options.version = BROWSER_API_VERSION;
	instance_options.storage = &fake_calls;
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = &fonts;
	view_options.stack_base = __builtin_frame_address(0);
	view_options.width = 320;
	view_options.height = 200;

	/* Before the table: an instance and a view are refused. */
	instance = NULL;
	error = browser_instance_create(&instance_options, &instance);
	table_check(error == ENOTSUP && instance == NULL, "an instance before the threads is refused (ENOTSUP)");
	view = NULL;
	error = browser_view_create(&view_options, &view);
	table_check(error == ENOTSUP && view == NULL, "a view before the threads is refused (ENOTSUP)");

	/* Tables that are refused. */
	error = browser_threads_set(NULL);
	table_check(error == EINVAL, "no table is refused (EINVAL)");
	wrong = *browser_threads_posix();
	wrong.version++;
	error = browser_threads_set(&wrong);
	table_check(error == ENOTSUP, "a table of another version is refused (ENOTSUP)");
	wrong = *browser_threads_posix();
	wrong.cond_wait_for = NULL;
	error = browser_threads_set(&wrong);
	table_check(error == ENOTSUP, "a table missing a call is refused (ENOTSUP)");

	/* The counting table, kept; a second one is refused. */
	table = *browser_threads_posix();
	table.start = table_start;
	table.join = table_join;
	error = browser_threads_set(&table);
	table_check(error == 0, "the table is kept");
	error = browser_threads_set(&table);
	table_check(error == EBUSY, "a second table is refused (EBUSY)");

	/* A view whose second thread cannot start fails, and gives back the first. */
	table_refuse_at = 2;
	view = NULL;
	error = browser_view_create(&view_options, &view);
	table_check(error == EAGAIN && view == NULL, "a view whose thread cannot start fails (EAGAIN)");
	pthread_mutex_lock(&table_lock);
	starts = table_starts;
	joins = table_joins;
	table_refuse_at = 0;
	pthread_mutex_unlock(&table_lock);
	table_check(starts == 1 && joins == 1, "the thread that started is joined");

	/* An instance over a backend and a view: the engine's named threads. */
	instance = NULL;
	error = browser_instance_create(&instance_options, &instance);
	table_check(error == 0 && instance != NULL, "an instance over a backend is made");
	view_options.instance = instance;
	view = NULL;
	if (instance != NULL)
		error = browser_view_create(&view_options, &view);
	table_check(error == 0 && view != NULL, "a view is made");
	table_check(table_named("browser-storage"), "the storage thread is named browser-storage");
	table_check(table_named("browser-resolve"), "the resolver thread is named browser-resolve");
	table_check(table_named("browser-net"), "the network thread is named browser-net");

	/* Everything goes, and every thread started was joined. */
	if (view != NULL)
		browser_view_destroy(view);
	if (instance != NULL) {
		error = browser_instance_destroy(instance);
		table_check(error == 0, "the instance is destroyed");
	}
	pthread_mutex_lock(&table_lock);
	starts = table_starts;
	joins = table_joins;
	pthread_mutex_unlock(&table_lock);
	printf("host-threads-table: %u threads started, %u joined\n", starts, joins);
	table_check(starts == 4 && joins == 4, "every thread started (the failed view's one, storage, resolver, network) is joined");

	/* The summary. */
	printf("host-threads-table: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Starts a thread through the POSIX table, counted and named, unless it is the start to refuse. */
static int
table_start(
	void *context,
	const char *name,
	size_t stack_size,
	void (*run)(void *argument),
	void *argument,
	void **thread)
{
	unsigned number;
	int error;

	/* The start's number, and whether it is refused. */
	pthread_mutex_lock(&table_lock);
	number = table_starts + 1U;
	if (table_refuse_at != 0 && number == table_refuse_at) {
		pthread_mutex_unlock(&table_lock);
		return EAGAIN;
	}
	pthread_mutex_unlock(&table_lock);

	/* The thread. */
	error = browser_threads_posix()->start(context, name, stack_size, run, argument, thread);
	if (error != 0)
		return error;

	/* Counted, and its name kept. */
	pthread_mutex_lock(&table_lock);
	table_starts++;
	if (name != NULL && table_name_count < TABLE_NAMES_MAX) {
		snprintf(table_names[table_name_count], TABLE_NAME_SIZE, "%s", name);
		table_name_count++;
	}
	pthread_mutex_unlock(&table_lock);
	return 0;
}

/* Joins a thread through the POSIX table, counted. */
static void
table_join(
	void *context,
	void *thread)
{
	/* The join. */
	browser_threads_posix()->join(context, thread);

	/* Counted. */
	pthread_mutex_lock(&table_lock);
	table_joins++;
	pthread_mutex_unlock(&table_lock);
}

/* Counts a check and reports it when it failed. */
static void
table_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* Tells whether a thread of a name was started. */
static int
table_named(
	const char *name)
{
	unsigned index;
	int found;

	found = 0;
	pthread_mutex_lock(&table_lock);
	for (index = 0; index < table_name_count; index++) {
		if (strcmp(table_names[index], name) == 0)
			found = 1;
	}
	pthread_mutex_unlock(&table_lock);
	return found;
}

/* The backend's read: nothing is kept. */
static int
fake_read(
	void *context,
	unsigned space,
	const char *key,
	struct browser_storage_sink *sink,
	int *found)
{
	(void)context;
	(void)space;
	(void)key;
	(void)sink;
	*found = 0;
	return 0;
}

/* The backend's write: dropped. */
static int
fake_write(
	void *context,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	(void)context;
	(void)space;
	(void)key;
	(void)bytes;
	(void)length;
	return 0;
}

/* The backend's remove: nothing to remove. */
static int
fake_remove(
	void *context,
	unsigned space,
	const char *key)
{
	(void)context;
	(void)space;
	(void)key;
	return 0;
}

/* The backend's list: no keys. */
static int
fake_list(
	void *context,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	(void)context;
	(void)space;
	(void)each;
	(void)each_context;
	return 0;
}

/* The backend's clear: nothing to clear. */
static int
fake_clear(
	void *context,
	unsigned space)
{
	(void)context;
	(void)space;
	return 0;
}

/* The backend's close: nothing to free. */
static void
fake_close(
	void *context)
{
	(void)context;
}
