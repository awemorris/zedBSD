/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws140: the dynamic loader past its old fixed limits.
 *
 * The program is linked with forty libraries (libmany00.so to
 * libmany39.so), more than the sixteen dependencies and close to the
 * thirty-two objects the loader once had room for, and then opens three
 * hubs of forty leaf libraries each, which takes the process past 160
 * objects.  It looks symbols up in the whole process and through a hub's
 * graph of 41 objects, closes and reopens, and then loads the three
 * objects U3 is about: a library with 300 TLSDESC relocations, one whose
 * name is longer than 64 bytes, and one with more than sixteen program
 * headers.  Then (p002) it opens 200 handles, past the 64 of the
 * loader's first handle chunk, and forty TLS libraries, which grow each
 * thread's TLS vector past its 33 entries, in two threads.  Last (T1-495)
 * it opens a library by its path, from a directory no search reaches, as
 * Python opens its extension modules.  build-many.sh
 * builds the libraries and this program, and
 * rtld-many.sh runs it in the guest with LD_LIBRARY_PATH set to where the
 * libraries are.
 *
 * Each step prints "RTLD-MANY step=<name> ok", or "RTLD-MANY step=<name>
 * FAIL <detail>" and exits with 1.  The last line is "RTLD-MANY: PASS".
 */

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The libraries the program is linked with, and the leaves of a hub. */
#define MANY_DIRECT 40U
#define MANY_HUBS 3U
#define MANY_LEAVES_PER_HUB 40U

/* The TLS variables of libtlsdesc.so (build-many.sh), each its own TLSDESC. */
#define MANY_TLSDESC 300L

/* The library with a long name, and the value its function returns. */
#define MANY_LONG_NAME \
	"libmany-long-name-past-the-old-sixty-four-byte-limit-of-the-loader-ws140.so"
#define MANY_LONG_VALUE 77

/* The library with extra program headers, and its function's value. */
#define MANY_PHDR_NAME "libphdr.so"
#define MANY_PHDR_VALUE 55

/* The library opened by its path (rtld-many.sh unpacks the archive in /root/ws140), and its function's value. */
#define MANY_PATH_ABSOLUTE "/root/ws140/dynload/libpathmod.so"
#define MANY_PATH_RELATIVE "./dynload/libpathmod.so"
#define MANY_PATH_MISSING "/root/ws140/dynload/libnothere.so"
#define MANY_PATH_VALUE 66

/* The handles opened at once, and the TLS libraries (libtls00.so to libtls39.so). */
#define MANY_HANDLES 200U
#define MANY_TLS 40U

/* What count_callback is asked to count or find. */
struct many_count {
	unsigned objects;
	unsigned phdr_found;
	unsigned phdr_phnum;
	unsigned phdr_dynamic;
};

int many_value_00(void);
int many_value_39(void);

int main(void);
static void step_ok(const char *step);
static void step_fail(const char *step, const char *detail);
static void count_objects(struct many_count *count);
static int count_callback(struct dl_phdr_info *information, size_t size, void *argument);
static int call_symbol(void *handle, const char *name, int *value);
static void check_global(void);
static void check_graph(void **hubs);
static void check_tlsdesc(void);
static void check_long_name(void);
static void check_phdr(const char *step);
static void check_handles(void);
static void open_tls(void);
static void check_tls(const char *step);
static void check_tls_written(const char *step);
static void *tls_thread(void *argument);
static void close_tls(void);
static void check_path(void);

/* The TLS libraries' handles and their functions, which return a variable's address. */
static void *tls_libraries[MANY_TLS];
static int *(*tls_functions[MANY_TLS])(void);

/*
 * Runs every step of the test in order.
 */
int
main(
	void)
{
	struct many_count count;
	char hub_name[sizeof("libhub0.so")];
	void *hubs[MANY_HUBS];
	unsigned startup_objects;
	pthread_t thread;
	unsigned i;
	int value;
	int found;
	int created;

	/* Reached main: every dependency was loaded and relocated. */
	step_ok("startup");

	/* Calls the first and the last direct dependency. */
	value = many_value_00();
	if (value != 1)
		step_fail("direct", "many_value_00 is wrong");

	/* The last, past the 32 objects the loader once had room for. */
	value = many_value_39();
	if (value != 40)
		step_fail("direct", "many_value_39 is wrong");

	/* Both answered. */
	step_ok("direct");

	/* Finds every direct dependency's function in the whole process. */
	check_global();
	step_ok("global");

	/* Counts the objects loaded at startup: the program, 40, libc.so and ld.so. */
	count_objects(&count);
	startup_objects = count.objects;
	printf("RTLD-MANY count-startup=%u\n", startup_objects);
	if (startup_objects < MANY_DIRECT + 3U)
		step_fail("count-startup", "too few objects");

	/* Enough objects. */
	step_ok("count-startup");

	/* Opens the three hubs, which load 120 leaves. */
	for (i = 0; i < MANY_HUBS; i++) {
		/* Opens one hub. */
		snprintf(hub_name, sizeof(hub_name), "libhub%u.so", i);
		hubs[i] = dlopen(hub_name, RTLD_NOW);
		if (hubs[i] == NULL)
			step_fail("hubs", dlerror());
	}

	/* Counts the hubs and their leaves beside the startup objects. */
	count_objects(&count);
	printf("RTLD-MANY count-hubs=%u\n", count.objects);
	if (count.objects < startup_objects + MANY_HUBS * (MANY_LEAVES_PER_HUB + 1U))
		step_fail("hubs", "too few objects");

	/* Every hub and leaf is loaded. */
	step_ok("hubs");

	/* Looks symbols up through the hubs' graphs. */
	check_graph(hubs);
	step_ok("graph");

	/* Closes the hubs, which unloads them and their leaves. */
	for (i = 0; i < MANY_HUBS; i++) {
		/* Closes one hub. */
		value = dlclose(hubs[i]);
		if (value != 0)
			step_fail("close", dlerror());
	}

	/* Back to the objects loaded at startup. */
	count_objects(&count);
	if (count.objects != startup_objects)
		step_fail("close", "objects left after the hubs closed");

	/* Everything the hubs brought is gone. */
	step_ok("close");

	/* Opens a hub again, into the slots the others left. */
	hubs[2] = dlopen("libhub2.so", RTLD_NOW);
	if (hubs[2] == NULL)
		step_fail("reopen", dlerror());

	/* Calls a leaf through it. */
	found = call_symbol(hubs[2], "leaf_value_100", &value);
	if (!found || value != 101)
		step_fail("reopen", "leaf_value_100 is missing or wrong");

	/* Closes it. */
	value = dlclose(hubs[2]);
	if (value != 0)
		step_fail("reopen", dlerror());

	/* The slots were used again. */
	step_ok("reopen");

	/* The three objects past the other fixed limits (U3). */
	check_tlsdesc();
	step_ok("tlsdesc");
	check_long_name();
	step_ok("long-name");
	check_phdr("phdr");
	step_ok("phdr");
	check_phdr("phdr-reopen");
	step_ok("phdr-reopen");

	/* The handles and the TLS modules past their first chunks (p002). */
	check_handles();
	step_ok("handles");
	open_tls();
	check_tls("tls");
	step_ok("tls");

	/* Another thread starts from the initial values and leaves the main thread's alone. */
	created = pthread_create(&thread, NULL, tls_thread, NULL);
	if (created != 0)
		step_fail("tls-thread", "pthread_create failed");

	/* Waits for it. */
	created = pthread_join(thread, NULL);
	if (created != 0)
		step_fail("tls-thread", "pthread_join failed");

	/* The main thread's values are as it wrote them. */
	check_tls_written("tls-thread");
	step_ok("tls-thread");

	/* Closes the forty and opens one again, under an id given before. */
	close_tls();
	step_ok("tls-close");

	/* A library opened by its path, absolute and relative (T1-495). */
	check_path();
	step_ok("path");

	/* Back to the objects loaded at startup once more. */
	count_objects(&count);
	if (count.objects != startup_objects)
		step_fail("end", "objects left at the end");

	/* Succeeded: every step passed. */
	printf("RTLD-MANY: PASS\n");
	return 0;
}

/*
 * Reports that a step passed.
 */
static void
step_ok(
	const char *step)
{
	/* The line rtld-many.sh collects. */
	printf("RTLD-MANY step=%s ok\n", step);
}

/*
 * Reports that a step failed and ends the program.
 */
static void
step_fail(
	const char *step,
	const char *detail)
{
	/* A NULL detail is a dlerror() with nothing to say. */
	if (detail == NULL)
		detail = "(no detail)";

	/* The line rtld-many.sh collects, then the failure exit. */
	printf("RTLD-MANY step=%s FAIL %s\n", step, detail);
	exit(1);
}

/*
 * Counts the loaded objects and finds the one with extra program headers.
 */
static void
count_objects(
	struct many_count *count)
{
	/* Walks every loaded object. */
	memset(count, 0, sizeof(*count));
	dl_iterate_phdr(count_callback, count);
}

/*
 * Counts one object, and records the headers of libphdr.so.
 */
static int
count_callback(
	struct dl_phdr_info *information,
	size_t size,
	void *argument)
{
	struct many_count *count;
	const char *name;
	size_t name_length;
	size_t suffix_length;
	unsigned i;
	int compared;

	/* One more object; the size of the record is not needed. */
	(void)size;
	count = argument;
	count->objects++;

	/* Only libphdr.so is looked at further. */
	name = information->dlpi_name;
	if (name == NULL)
		return 0;

	/* Compares the end of the path with the library's name. */
	name_length = strlen(name);
	suffix_length = strlen(MANY_PHDR_NAME);
	if (name_length < suffix_length)
		return 0;

	/* The path ends with the name. */
	compared = strcmp(name + name_length - suffix_length, MANY_PHDR_NAME);
	if (compared != 0)
		return 0;

	/* Records its count of headers and whether its dynamic one is among them. */
	count->phdr_found = 1;
	count->phdr_phnum = information->dlpi_phnum;
	for (i = 0; i < information->dlpi_phnum; i++) {
		/* The header of its dynamic section. */
		if (information->dlpi_phdr[i].p_type == PT_DYNAMIC)
			count->phdr_dynamic = 1;
	}

	/* Goes on to the next object. */
	return 0;
}

/*
 * Looks a function up through a handle and calls it.  Returns 1 with its
 * value in *value when the symbol is found, 0 otherwise.
 */
static int
call_symbol(
	void *handle,
	const char *name,
	int *value)
{
	int (*function)(void);
	void *symbol;

	/* The function, when the handle's graph has it. */
	symbol = dlsym(handle, name);
	if (symbol == NULL)
		return 0;

	/* Calls it. */
	function = (int (*)(void))symbol;
	*value = function();

	/* Succeeded: the value is in *value. */
	return 1;
}

/*
 * Finds every direct dependency's function through the process's handle.
 */
static void
check_global(
	void)
{
	char name[sizeof("many_value_00")];
	void *process;
	unsigned i;
	int value;
	int found;

	/* The handle of the whole process. */
	process = dlopen(NULL, RTLD_NOW);
	if (process == NULL)
		step_fail("global", dlerror());

	/* Each library's function returns its number plus one. */
	for (i = 0; i < MANY_DIRECT; i++) {
		/* Looks up and calls one. */
		snprintf(name, sizeof(name), "many_value_%02u", i);
		found = call_symbol(process, name, &value);
		if (!found || value != (int)i + 1)
			step_fail("global", name);
	}
}

/*
 * Looks leaves up through the hubs' graphs: found in their own hub only.
 */
static void
check_graph(
	void **hubs)
{
	void *symbol;
	int value;
	int found;

	/* The last leaf of the first hub, the 41st object of its graph. */
	found = call_symbol(hubs[0], "leaf_value_039", &value);
	if (!found || value != 40)
		step_fail("graph", "leaf_value_039 in hub0 is missing or wrong");

	/* The last leaf of the last hub. */
	found = call_symbol(hubs[2], "leaf_value_119", &value);
	if (!found || value != 120)
		step_fail("graph", "leaf_value_119 in hub2 is missing or wrong");

	/* A leaf of another hub is not in the first hub's graph. */
	symbol = dlsym(hubs[0], "leaf_value_119");
	if (symbol != NULL)
		step_fail("graph", "leaf_value_119 found through hub0");
}

/*
 * Opens the library with 300 TLSDESC relocations, reads and writes its
 * variables, and opens it again after closing it to see them fresh.
 */
static void
check_tlsdesc(
	void)
{
	long (*sum)(void);
	void (*add)(void);
	void *library;
	void *symbol;
	long expected;
	long total;
	unsigned round;
	int closed;

	/* The variables start at 1 to 300: their sum. */
	expected = MANY_TLSDESC * (MANY_TLSDESC + 1L) / 2L;

	/* Twice: the second time after the first copy was unloaded. */
	for (round = 0; round < 2U; round++) {
		/* Opens the library, relocating its TLSDESC entries. */
		library = dlopen("libtlsdesc.so", RTLD_NOW);
		if (library == NULL)
			step_fail("tlsdesc", dlerror());

		/* Its two functions. */
		symbol = dlsym(library, "tlsdesc_sum");
		if (symbol == NULL)
			step_fail("tlsdesc", dlerror());

		/* The sum, then the function that adds one to each variable. */
		sum = (long (*)(void))symbol;
		symbol = dlsym(library, "tlsdesc_add");
		if (symbol == NULL)
			step_fail("tlsdesc", dlerror());

		/* Reads the initial values through every descriptor. */
		add = (void (*)(void))symbol;
		total = sum();
		printf("RTLD-MANY tlsdesc round=%u sum=%ld\n", round, total);
		if (total != expected)
			step_fail("tlsdesc", "initial sum is wrong");

		/* Writes each variable through its descriptor and reads it back. */
		add();
		total = sum();
		if (total != expected + MANY_TLSDESC)
			step_fail("tlsdesc", "sum after the writes is wrong");

		/* Unloads it, with its pages of TLSDESC arguments. */
		closed = dlclose(library);
		if (closed != 0)
			step_fail("tlsdesc", dlerror());
	}
}

/*
 * Opens the library whose name is longer than 64 bytes.
 */
static void
check_long_name(
	void)
{
	void *library;
	int value;
	int found;
	int closed;

	/* Opens it by its long name. */
	library = dlopen(MANY_LONG_NAME, RTLD_NOW);
	if (library == NULL)
		step_fail("long-name", dlerror());

	/* Calls its function. */
	found = call_symbol(library, "long_value", &value);
	if (!found || value != MANY_LONG_VALUE)
		step_fail("long-name", "long_value is missing or wrong");

	/* Closes it. */
	closed = dlclose(library);
	if (closed != 0)
		step_fail("long-name", dlerror());
}

/*
 * Opens the library with more than sixteen program headers and sees them
 * through dl_iterate_phdr.  Called twice: the second time it reuses the
 * program table the first copy left.
 */
static void
check_phdr(
	const char *step)
{
	struct many_count count;
	void *library;
	int value;
	int found;
	int closed;

	/* Opens it. */
	library = dlopen(MANY_PHDR_NAME, RTLD_NOW);
	if (library == NULL)
		step_fail(step, dlerror());

	/* Calls its function. */
	found = call_symbol(library, "phdr_value", &value);
	if (!found || value != MANY_PHDR_VALUE)
		step_fail(step, "phdr_value is missing or wrong");

	/* Its headers as dl_iterate_phdr reports them. */
	count_objects(&count);
	printf("RTLD-MANY %s phnum=%u\n", step, count.phdr_phnum);
	if (!count.phdr_found || count.phdr_phnum <= 16U || !count.phdr_dynamic)
		step_fail(step, "program headers are missing or short");

	/* Closes it. */
	closed = dlclose(library);
	if (closed != 0)
		step_fail(step, dlerror());
}

/*
 * Opens 200 handles of one library at once and closes them all.
 */
static void
check_handles(
	void)
{
	void *handles[MANY_HANDLES];
	unsigned i;
	int closed;

	/* Opens them; the table grows instead of running out. */
	for (i = 0; i < MANY_HANDLES; i++) {
		/* One more handle. */
		handles[i] = dlopen("libmany00.so", RTLD_NOW);
		if (handles[i] == NULL)
			step_fail("handles", dlerror());
	}

	/* Closes them. */
	for (i = 0; i < MANY_HANDLES; i++) {
		/* Closes one. */
		closed = dlclose(handles[i]);
		if (closed != 0)
			step_fail("handles", dlerror());
	}
}

/*
 * Opens the forty TLS libraries and finds their functions.
 */
static void
open_tls(
	void)
{
	char library[sizeof("libtls00.so")];
	char function[sizeof("tls_address_00")];
	void *symbol;
	unsigned i;

	/* Each library, and its function. */
	for (i = 0; i < MANY_TLS; i++) {
		/* Opens the library, a TLS module of its own. */
		snprintf(library, sizeof(library), "libtls%02u.so", i);
		tls_libraries[i] = dlopen(library, RTLD_NOW);
		if (tls_libraries[i] == NULL)
			step_fail("tls", dlerror());

		/* Its function. */
		snprintf(function, sizeof(function), "tls_address_%02u", i);
		symbol = dlsym(tls_libraries[i], function);
		if (symbol == NULL)
			step_fail("tls", dlerror());

		/* Kept for the steps. */
		tls_functions[i] = (int *(*)(void))symbol;
	}
}

/*
 * Reads each TLS variable in the calling thread, expecting its initial
 * value (its number plus one), then writes ten times its number and reads
 * it back.
 */
static void
check_tls(
	const char *step)
{
	int *address;
	int *again;
	unsigned i;

	/* Each library's variable. */
	for (i = 0; i < MANY_TLS; i++) {
		/* The thread's copy starts at the initial value. */
		address = tls_functions[i]();
		if (*address != (int)i + 1)
			step_fail(step, "a variable does not start at its initial value");

		/* Written and read back through a second access. */
		*address = (int)i * 10;
		again = tls_functions[i]();
		if (again != address || *again != (int)i * 10)
			step_fail(step, "a variable does not keep what was written");
	}
}

/*
 * Checks that the calling thread's TLS variables still hold what
 * check_tls wrote.
 */
static void
check_tls_written(
	const char *step)
{
	int *address;
	unsigned i;

	/* Each library's variable. */
	for (i = 0; i < MANY_TLS; i++) {
		/* Ten times its number, as written. */
		address = tls_functions[i]();
		if (*address != (int)i * 10)
			step_fail(step, "another thread changed a variable");
	}
}

/*
 * Runs check_tls in a second thread, whose TLS vector grows on its own.
 */
static void *
tls_thread(
	void *argument)
{
	/* The thread's own copies. */
	(void)argument;
	check_tls("tls-thread");

	/* Nothing to give back. */
	return NULL;
}

/*
 * Closes the forty TLS libraries, then opens the last again and reads its
 * variable from its initial value.
 */
static void
close_tls(
	void)
{
	void *library;
	void *symbol;
	int *(*function)(void);
	int *address;
	unsigned i;
	int closed;

	/* Closes them, which frees their ids and blocks. */
	for (i = 0; i < MANY_TLS; i++) {
		/* Closes one. */
		closed = dlclose(tls_libraries[i]);
		if (closed != 0)
			step_fail("tls-close", dlerror());
	}

	/* Opens the last again. */
	library = dlopen("libtls39.so", RTLD_NOW);
	if (library == NULL)
		step_fail("tls-close", dlerror());

	/* Its function. */
	symbol = dlsym(library, "tls_address_39");
	if (symbol == NULL)
		step_fail("tls-close", dlerror());

	/* A fresh block: the initial value again. */
	function = (int *(*)(void))symbol;
	address = function();
	if (*address != 40)
		step_fail("tls-close", "the reopened variable is not at its initial value");

	/* Closes it. */
	closed = dlclose(library);
	if (closed != 0)
		step_fail("tls-close", dlerror());
}

/*
 * Opens a library by its absolute path, from a directory that neither
 * LD_LIBRARY_PATH nor /lib and /usr/lib reach, then by a relative path to
 * the same file (the same object, found by its identity) and by the
 * absolute path again (the same object); a path to no file is refused.
 */
static void
check_path(
	void)
{
	void *absolute;
	void *relative;
	void *again;
	void *missing;
	void *first_symbol;
	void *relative_symbol;
	void *again_symbol;
	int value;
	int found;

	/* By the absolute path: the file outside every search. */
	absolute = dlopen(MANY_PATH_ABSOLUTE, RTLD_NOW);
	if (absolute == NULL)
		step_fail("path", dlerror());

	/* Its function answers. */
	found = call_symbol(absolute, "path_value", &value);
	if (!found || value != MANY_PATH_VALUE)
		step_fail("path", "path_value is wrong");
	first_symbol = dlsym(absolute, "path_value");

	/* By a relative path from the program's directory: the object loaded already. */
	relative = dlopen(MANY_PATH_RELATIVE, RTLD_NOW);
	if (relative == NULL)
		step_fail("path", dlerror());
	relative_symbol = dlsym(relative, "path_value");
	if (relative_symbol != first_symbol)
		step_fail("path", "the relative path loaded another copy");

	/* By the absolute path again: the same object. */
	again = dlopen(MANY_PATH_ABSOLUTE, RTLD_NOW);
	if (again == NULL)
		step_fail("path", dlerror());
	again_symbol = dlsym(again, "path_value");
	if (again_symbol != first_symbol)
		step_fail("path", "the second open loaded another copy");

	/* A path to no file is refused. */
	missing = dlopen(MANY_PATH_MISSING, RTLD_NOW);
	if (missing != NULL)
		step_fail("path", "a path to no file was opened");

	/* Closes the three handles, which unloads the library. */
	(void)dlclose(again);
	(void)dlclose(relative);
	(void)dlclose(absolute);
}
