/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dynamic loader runs a library's exit handlers when dlclose()
 * unloads it, and the C library runs what is left at exit.
 *
 * libfinalize-plain.so (finalize-lib.c, finalize-cxx.cc) has a C++
 * static object with a destructor, and registers on request an atexit()
 * function, a destructor in the C++ ABI's form with its __dso_handle, an
 * atexit() function through a tail call, a C++-form destructor without a
 * handle, and 32 atexit() functions from four threads at once.  dlclose()
 * must run all of them, newest first, and then the library's destructor,
 * before it returns: after it they would call unmapped code.  Opening and
 * closing it again runs the new registrations once and the old ones not
 * again.  libfinalize-keep.so is the same library linked with -z
 * nodelete: its handlers run only at exit, after the program's last
 * line, newest first, before the program's own atexit() function, which
 * was registered first; rtld-finalize.sh checks those lines.
 *
 * Every handler reports to finalize_record() or finalize_count(), which
 * the program exports to the libraries.  Each step prints
 * "RTLD-FINALIZE step=<name> ok", or "RTLD-FINALIZE step=<name> FAIL
 * <detail>" and exits with 1.  The last line the program itself prints is
 * "RTLD-FINALIZE: PASS"; a record made after it is printed as
 * "RTLD-FINALIZE run <event>".  Every line goes out with write().
 */

#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The atexit() functions the threads of a library register in all. */
#define FINALIZE_THREAD_HANDLERS 32

/* What one unload of a library runs, newest registration first. */
#define FINALIZE_PLAIN_SEQUENCE \
	"plain cxa-null|plain atexit-tail|plain cxa-handle|plain atexit|" \
	"plain c++-static|plain fini|"

/* The functions of a library the steps call. */
struct finalize_library {
	void *handle;
	int (*do_register)(void);
	int (*register_tail)(void);
	int (*register_null)(void);
	int (*register_threads)(void);
};

int main(void);
void finalize_record(const char *event);
void finalize_count(void);
static void say(const char *line);
static void step_ok(const char *step);
static void step_fail(const char *step, const char *detail);
static void program_exit(void);
static void record_clear(void);
static unsigned count_objects(void);
static int count_callback(struct dl_phdr_info *information, size_t size, void *argument);
static void *find(const char *step, void *handle, const char *name);
static void open_library(const char *step, struct finalize_library *library, const char *name, int threads);
static void check_plain(const char *step, unsigned objects);
static void check_keep(const char *step, unsigned objects);

/* The events recorded since the last clear, each followed by '|'. */
static char record[1024];

/* The length of the record. */
static size_t record_length;

/* The thread handlers that ran since the last clear. */
static int record_count;

/* Whether the program printed its last line, after which records are printed. */
static int record_printing;

/*
 * Runs every step of the test in order.
 */
int
main(
	void)
{
	unsigned startup_objects;
	int error;

	/* Registers the program's own function, first of all, so it runs last. */
	error = atexit(program_exit);
	if (error != 0)
		step_fail("startup", "atexit failed");

	/* Counts the objects loaded at startup, which every step compares with. */
	startup_objects = count_objects();
	step_ok("startup");

	/* A plain library runs its exit handlers at dlclose(). */
	check_plain("plain", startup_objects);
	step_ok("plain");

	/* Again: the new registrations run once and the old ones not again. */
	check_plain("plain-reopen", startup_objects);
	step_ok("plain-reopen");

	/* A NODELETE library keeps its exit handlers for exit. */
	check_keep("keep", startup_objects);
	step_ok("keep");

	/* Every step passed; keep's handlers and the program's run after this line. */
	record_printing = 1;
	say("RTLD-FINALIZE: PASS\n");

	/* The process exits normally, which runs the handlers left. */
	return 0;
}

/*
 * Records that an exit handler or a destructor ran.
 */
void
finalize_record(
	const char *event)
{
	char line[160];
	size_t length;

	/* After the last line, prints the event for rtld-finalize.sh. */
	if (record_printing) {
		snprintf(line, sizeof(line), "RTLD-FINALIZE run %s\n", event);
		say(line);
		return;
	}

	/* Appends the event and its separator, if there is room. */
	length = strlen(event);
	if (record_length + length + 2U > sizeof(record))
		step_fail("record", "the record is full");

	memcpy(record + record_length, event, length);
	record_length += length;
	record[record_length++] = '|';
	record[record_length] = '\0';
}

/*
 * Counts a run of the threads' atexit() function.
 */
void
finalize_count(
	void)
{
	/* The thread handlers all run in the thread that unloads the library. */
	record_count++;
}

/* Writes one line, unbuffered. */
static void
say(
	const char *line)
{
	/* Writes the whole line. */
	(void)write(1, line, strlen(line));
}

/* Prints that a step passed. */
static void
step_ok(
	const char *step)
{
	char line[128];

	/* The step's line. */
	snprintf(line, sizeof(line), "RTLD-FINALIZE step=%s ok\n", step);
	say(line);
}

/* Prints why a step failed and ends the program. */
static void
step_fail(
	const char *step,
	const char *detail)
{
	char line[1280];

	/* The step's line, with what went wrong. */
	snprintf(line, sizeof(line), "RTLD-FINALIZE step=%s FAIL %s\n", step,
		 detail != NULL ? detail : "(no detail)");
	say(line);

	/* Stops without the exit handlers, which may be what is broken. */
	_exit(1);
}

/* The program's own atexit() function, registered before any library's. */
static void
program_exit(
	void)
{
	/* Reports it like the libraries' handlers. */
	finalize_record("atexit main");
}

/* Forgets what was recorded. */
static void
record_clear(
	void)
{
	/* Empties the record and the count. */
	record_length = 0;
	record[0] = '\0';
	record_count = 0;
}

/* Counts the objects the loader reports now. */
static unsigned
count_objects(
	void)
{
	unsigned count;

	/* Walks every loaded object. */
	count = 0;
	(void)dl_iterate_phdr(count_callback, &count);

	/* The objects counted. */
	return count;
}

/* Counts one object. */
static int
count_callback(
	struct dl_phdr_info *information,
	size_t size,
	void *argument)
{
	unsigned *count;

	/* The object is only counted. */
	(void)information;
	(void)size;
	count = argument;
	(*count)++;

	/* Goes on to the next object. */
	return 0;
}

/* Looks a function up through a handle, ending the program without it. */
static void *
find(
	const char *step,
	void *handle,
	const char *name)
{
	void *address;

	/* Looks the name up in the library's graph. */
	address = dlsym(handle, name);
	if (address == NULL)
		step_fail(step, name);

	/* The function's address. */
	return address;
}

/*
 * Opens a library and has it register its exit handlers, with the
 * threads' ones when asked.
 */
static void
open_library(
	const char *step,
	struct finalize_library *library,
	const char *name,
	int threads)
{
	int error;

	/* Opens the library, which constructs its C++ static object. */
	memset(library, 0, sizeof(*library));
	library->handle = dlopen(name, RTLD_NOW);
	if (library->handle == NULL)
		step_fail(step, dlerror());

	/* Finds the registering functions. */
	library->do_register =
	    (int (*)(void))find(step, library->handle, "lib_register");
	library->register_tail =
	    (int (*)(void))find(step, library->handle, "lib_register_tail");
	library->register_null =
	    (int (*)(void))find(step, library->handle, "lib_register_null");
	library->register_threads =
	    (int (*)(void))find(step, library->handle, "lib_register_threads");

	/* Registers the handlers, oldest first. */
	error = library->do_register();
	if (error != 0)
		step_fail(step, "lib_register failed");

	error = library->register_tail();
	if (error != 0)
		step_fail(step, "lib_register_tail failed");

	error = library->register_null();
	if (error != 0)
		step_fail(step, "lib_register_null failed");

	/* The threads' ones, newest of all. */
	if (threads) {
		error = library->register_threads();
		if (error != 0)
			step_fail(step, "lib_register_threads failed");
	}

	/* Nothing ran while the library was open. */
	if (record_length != 0 || record_count != 0)
		step_fail(step, record);
}

/*
 * Opens and closes the plain library: its exit handlers run in the close,
 * newest first, then its destructor, and it is gone.
 */
static void
check_plain(
	const char *step,
	unsigned objects)
{
	struct finalize_library library;
	Dl_info information;
	void *address;
	unsigned count;
	int error;
	int found;

	/* Opens the library and registers its handlers, the threads' too. */
	record_clear();
	open_library(step, &library, "libfinalize-plain.so", 1);

	/* One more object while it is open. */
	count = count_objects();
	if (count != objects + 1U)
		step_fail(step, "the open added no object");

	address = (void *)library.do_register;

	/* Closes it, which runs the handlers before it returns. */
	error = dlclose(library.handle);
	if (error != 0)
		step_fail(step, dlerror());

	/* Every thread's function ran, once. */
	if (record_count != FINALIZE_THREAD_HANDLERS)
		step_fail(step, "the threads' atexit functions did not all run once");

	/* The others ran newest first, the C++ object's last, then the destructor. */
	if (strcmp(record, FINALIZE_PLAIN_SEQUENCE) != 0)
		step_fail(step, record);

	/* It is gone. */
	count = count_objects();
	if (count != objects)
		step_fail(step, "the object is still loaded after the close");

	found = dladdr(address, &information);
	if (found != 0)
		step_fail(step, "the address is still in a loaded object");
}

/*
 * Opens and closes the NODELETE library: nothing runs in the close, and it
 * stays for exit.
 */
static void
check_keep(
	const char *step,
	unsigned objects)
{
	struct finalize_library library;
	unsigned count;
	int error;

	/* Opens the library and registers its handlers. */
	record_clear();
	open_library(step, &library, "libfinalize-keep.so", 0);

	/* Closes it: nothing runs, nothing is unloaded. */
	error = dlclose(library.handle);
	if (error != 0)
		step_fail(step, dlerror());

	if (record_length != 0 || record_count != 0)
		step_fail(step, record);

	count = count_objects();
	if (count != objects + 1U)
		step_fail(step, "the object count changed with the close");
}
