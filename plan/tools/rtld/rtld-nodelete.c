/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dynamic loader keeps a NODELETE object mapped after its last
 * dlclose().
 *
 * libnodelete-keep.so is linked with -z nodelete (DF_1_NODELETE in its
 * DT_FLAGS_1) and needs libnodelete-dep.so; libnodelete-flag.so is a
 * plain library opened with RTLD_NODELETE; libnodelete-plain.so is the
 * control, which dlclose() unloads.  For keep and flag the program saves
 * the functions, has the library register a destructor of a thread's data
 * and an atexit() function, closes the handle, and then calls through the
 * saved pointers and ends a thread whose data the library's destructor
 * destroys: the code must still be there.  At exit the libraries' atexit()
 * functions and destructors run; rtld-nodelete.sh checks their lines come
 * after the program's last one, the dependency's after keep's.
 *
 * Each step prints "RTLD-NODELETE step=<name> ok", or "RTLD-NODELETE
 * step=<name> FAIL <detail>" and exits with 1.  The last line the
 * program itself prints is "RTLD-NODELETE: PASS".  Every line goes out
 * with write(), unbuffered, so the libraries' lines keep their places.
 */

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What each library's lib_value returns (nodelete-lib.c, nodelete-dep.c). */
#define NODELETE_KEEP_VALUE 42
#define NODELETE_FLAG_VALUE 7
#define NODELETE_PLAIN_VALUE 5

/* The functions of a library the test saves before it closes the handle. */
struct nodelete_library {
	const char *name;
	void *handle;
	int (*value)(void);
	int (*do_register)(void);
	int (*set)(void);
	int (*thread_count)(void);
	void *(*dep_address)(void);
};

int main(void);
static void say(const char *line);
static void step_ok(const char *step);
static void step_fail(const char *step, const char *detail);
static unsigned count_objects(void);
static int count_callback(struct dl_phdr_info *information, size_t size, void *argument);
static void *find(const char *step, void *handle, const char *name);
static void open_library(const char *step, struct nodelete_library *library, int flags);
static void check_mapped(const char *step, const void *address, const char *file);
static void check_kept(const char *step, struct nodelete_library *library, unsigned objects);
static void *thread_main(void *argument);
static void check_thread(const char *step, struct nodelete_library *library);
static void check_reopen(const char *step, struct nodelete_library *library, unsigned objects);
static void check_plain(unsigned objects);

/*
 * Runs every step of the test in order.
 */
int
main(
	void)
{
	struct nodelete_library keep;
	struct nodelete_library flag;
	unsigned startup_objects;

	/* Counts the objects loaded at startup, which every step compares with. */
	startup_objects = count_objects();
	step_ok("startup");

	/* The control: a plain library goes with its last dlclose(). */
	check_plain(startup_objects);
	step_ok("plain");

	/* Opens keep, which DF_1_NODELETE marks, and has it register its callbacks. */
	open_library("keep-open", &keep, RTLD_NOW);
	step_ok("keep-open");

	/* Closes it: keep and its dependency stay. */
	check_kept("keep-close", &keep, startup_objects + 2U);
	step_ok("keep-close");

	/* Ends a thread whose data keep's destructor destroys, after the close. */
	check_thread("keep-thread", &keep);
	step_ok("keep-thread");

	/* Opens it again: the same mapping, which stays after the close. */
	check_reopen("keep-reopen", &keep, startup_objects + 2U);
	step_ok("keep-reopen");

	/* Opens flag with RTLD_NODELETE: the same, though its file has no flag. */
	open_library("flag-open", &flag, RTLD_NOW | RTLD_NODELETE);
	step_ok("flag-open");

	/* Closes it: flag stays. */
	check_kept("flag-close", &flag, startup_objects + 3U);
	step_ok("flag-close");

	/* Ends a thread whose data flag's destructor destroys. */
	check_thread("flag-thread", &flag);
	step_ok("flag-thread");

	/* Opens it again without the flag: it stays all the same. */
	check_reopen("flag-reopen", &flag, startup_objects + 3U);
	step_ok("flag-reopen");

	/* Every step passed; the libraries' callbacks run at exit, after this line. */
	say("RTLD-NODELETE: PASS\n");

	/* The process exits normally, which runs the atexit() functions and destructors. */
	return 0;
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
	snprintf(line, sizeof(line), "RTLD-NODELETE step=%s ok\n", step);
	say(line);
}

/* Prints why a step failed and ends the program. */
static void
step_fail(
	const char *step,
	const char *detail)
{
	char line[320];

	/* The step's line, with what went wrong. */
	snprintf(line, sizeof(line), "RTLD-NODELETE step=%s FAIL %s\n", step,
		 detail != NULL ? detail : "(no detail)");
	say(line);

	/* Stops without the exit handlers, which may be what is broken. */
	_exit(1);
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
 * Opens a library, saves its functions, checks its value and has it
 * register its callbacks.
 */
static void
open_library(
	const char *step,
	struct nodelete_library *library,
	int flags)
{
	int expected;
	int value;
	int error;

	/* Opens the library the step names. */
	memset(library, 0, sizeof(*library));
	library->name = flags & RTLD_NODELETE ?
	    "libnodelete-flag.so" : "libnodelete-keep.so";
	library->handle = dlopen(library->name, flags);
	if (library->handle == NULL)
		step_fail(step, dlerror());

	/* Saves the functions the later steps call after the close. */
	library->value = (int (*)(void))find(step, library->handle, "lib_value");
	library->do_register =
	    (int (*)(void))find(step, library->handle, "lib_register");
	library->set = (int (*)(void))find(step, library->handle, "lib_set");
	library->thread_count =
	    (int (*)(void))find(step, library->handle, "lib_thread_count");
	library->dep_address =
	    (void *(*)(void))find(step, library->handle, "lib_dep_address");

	/* Checks the library's value, through its dependency for keep. */
	expected = flags & RTLD_NODELETE ?
	    NODELETE_FLAG_VALUE : NODELETE_KEEP_VALUE;
	value = library->value();
	if (value != expected)
		step_fail(step, "lib_value is wrong");

	/* Registers the thread data's destructor and the atexit() function. */
	error = library->do_register();
	if (error != 0)
		step_fail(step, "lib_register failed");
}

/* Checks that an address still lies in a loaded object of the named file. */
static void
check_mapped(
	const char *step,
	const void *address,
	const char *file)
{
	Dl_info information;
	const char *name;
	int found;

	/* Finds the object the address lies in. */
	found = dladdr(address, &information);
	if (found == 0)
		step_fail(step, "the address is in no loaded object");

	/* It is the file expected. */
	name = information.dli_fname != NULL ? information.dli_fname : "";
	if (strstr(name, file) == NULL)
		step_fail(step, name);
}

/*
 * Closes a NODELETE library's handle and checks that it, and its
 * dependency, are still there and still answer.
 */
static void
check_kept(
	const char *step,
	struct nodelete_library *library,
	unsigned objects)
{
	void *dependency;
	unsigned count;
	int value;
	int error;

	/* Closes the only handle on the library. */
	error = dlclose(library->handle);
	if (error != 0)
		step_fail(step, dlerror());

	/* Nothing was unloaded. */
	count = count_objects();
	if (count != objects)
		step_fail(step, "the object count changed with the close");

	/* The library's code is where it was. */
	check_mapped(step, (const void *)library->value, library->name);

	/* Its dependency, if it has one, too. */
	dependency = library->dep_address();
	if (dependency != NULL)
		check_mapped(step, dependency, "libnodelete-dep.so");

	/* The saved function still answers. */
	value = library->value();
	if (value != (dependency != NULL ?
		      NODELETE_KEEP_VALUE : NODELETE_FLAG_VALUE))
		step_fail(step, "lib_value is wrong after the close");
}

/* A thread that gives itself data the library's destructor destroys. */
static void *
thread_main(
	void *argument)
{
	struct nodelete_library *library;
	int error;

	/* Stores the data through the function saved before the close. */
	library = argument;
	error = library->set();

	/* Ends the thread, which runs the library's destructor. */
	return error == 0 ? argument : NULL;
}

/* Ends a thread whose data the closed library's destructor destroys. */
static void
check_thread(
	const char *step,
	struct nodelete_library *library)
{
	pthread_t thread;
	void *result;
	int error;
	int count;

	/* Starts the thread. */
	error = pthread_create(&thread, NULL, thread_main, library);
	if (error != 0)
		step_fail(step, "pthread_create failed");

	/* Waits for it to end. */
	error = pthread_join(thread, &result);
	if (error != 0 || result == NULL)
		step_fail(step, "the thread failed");

	/* The library's destructor ran once, in its code still mapped. */
	count = library->thread_count();
	if (count != 1)
		step_fail(step, "the thread data's destructor did not run once");
}

/*
 * Opens a kept library again without RTLD_NODELETE, checks it is the
 * same mapping, and closes it, which still leaves it.
 */
static void
check_reopen(
	const char *step,
	struct nodelete_library *library,
	unsigned objects)
{
	void *address;
	unsigned count;
	int error;

	/* Opens the library again. */
	library->handle = dlopen(library->name, RTLD_NOW);
	if (library->handle == NULL)
		step_fail(step, dlerror());

	/* The same object, not a new copy. */
	address = find(step, library->handle, "lib_value");
	if (address != (void *)library->value)
		step_fail(step, "a new copy of the library");

	/* Closes it. */
	error = dlclose(library->handle);
	if (error != 0)
		step_fail(step, dlerror());

	/* It is still there. */
	count = count_objects();
	if (count != objects)
		step_fail(step, "the object count changed with the close");
	check_mapped(step, (const void *)library->value, library->name);
}

/* Opens and closes the plain library, which goes with the close. */
static void
check_plain(
	unsigned objects)
{
	int (*value_function)(void);
	Dl_info information;
	void *handle;
	unsigned count;
	int value;
	int error;
	int found;

	/* Opens the control library. */
	handle = dlopen("libnodelete-plain.so", RTLD_NOW);
	if (handle == NULL)
		step_fail("plain", dlerror());

	/* Calls it while it is open. */
	value_function = (int (*)(void))find("plain", handle, "lib_value");
	value = value_function();
	if (value != NODELETE_PLAIN_VALUE)
		step_fail("plain", "lib_value is wrong");

	/* One more object while it is open. */
	count = count_objects();
	if (count != objects + 1U)
		step_fail("plain", "the open added no object");

	/* Closes it; its destructor runs now, before this step's line. */
	error = dlclose(handle);
	if (error != 0)
		step_fail("plain", dlerror());

	/* It is gone. */
	count = count_objects();
	if (count != objects)
		step_fail("plain", "the object is still loaded after the close");

	/* Its address lies in no loaded object now. */
	found = dladdr((const void *)value_function, &information);
	if (found != 0)
		step_fail("plain", "the address is still in a loaded object");
}
