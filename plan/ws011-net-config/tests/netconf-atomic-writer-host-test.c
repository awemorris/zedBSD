/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * Exercise the production writer's host-libc call order and failure cleanup.
 * This is not an overlay/UFS or target-libc implementation test.  In particular,
 * successful host fsync/rename does not prove target durability or lock progress.
 */
#include "userland/base/net/netconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_open(const char *, int, ...);
FILE *__real_fdopen(int, const char *);
int __real_fflush(FILE *);
int __real_fsync(int);
int __real_fclose(FILE *);
int __real_close(int);
int __real_rename(const char *, const char *);
int __real_unlink(const char *);

struct fault_case {
	const char *name;
	char failed_stage;
	int primary_errno;
	int cleanup_close_error;
	int cleanup_unlink_error;
	const char *events;
};

static char destination[512], temporary[512], events[64];
static const char *prior_bytes, *canonical_bytes;
static size_t prior_length, canonical_length, event_count;
static const struct fault_case *current;
static FILE *writer_stream;
static int writer_descriptor = -1;
static int active, fault_taken, published, flush_completed, sync_completed;

static void
require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "NCOM atomic writer (%s; events=%s): %s\n",
		    current != NULL ? current->name : "setup", events, message);
		exit(1);
	}
}

/* Bypass wrappers when checking that the visible destination is unchanged. */
static void
assert_bytes(const char *path, const char *expected, size_t length)
{
	char buffer[8192];
	size_t done;
	ssize_t count;
	int descriptor;

	require(length < sizeof(buffer), "fixture exceeds read buffer");
	descriptor = __real_open(path, O_RDONLY);
	require(descriptor >= 0, "cannot inspect file bytes");
	done = 0;
	while ((count = read(descriptor, buffer + done,
	    sizeof(buffer) - done)) > 0) {
		done += (size_t)count;
		require(done < sizeof(buffer), "observed file exceeds read buffer");
	}
	require(count == 0 && __real_close(descriptor) == 0,
	    "cannot finish inspecting file bytes");
	require(done == length && memcmp(buffer, expected, length) == 0,
	    "file bytes differ from expected generation");
}

static void
record(char stage)
{
	int saved;

	saved = errno;
	require(!published, "operation continued after successful publication");
	assert_bytes(destination, prior_bytes, prior_length);
	require(event_count + 1U < sizeof(events), "event log overflow");
	events[event_count++] = stage;
	events[event_count] = '\0';
	errno = saved;
}

static int
inject(char stage)
{
	if (current->failed_stage != stage || fault_taken)
		return 0;
	fault_taken = 1;
	errno = current->primary_errno;
	return 1;
}

int
__wrap_open(const char *path, int flags, ...)
{
	va_list arguments;
	mode_t mode;
	int descriptor;

	mode = 0;
	if ((flags & O_CREAT) != 0) {
		va_start(arguments, flags);
		mode = (mode_t)va_arg(arguments, int);
		va_end(arguments);
	}
	if (!active || strcmp(path, temporary) != 0)
		return __real_open(path, flags, mode);
	record('O');
	require(flags == (O_WRONLY | O_CREAT | O_EXCL) && mode == 0644,
	    "temporary creation flags or mode changed");
	if (inject('O'))
		return -1;
	descriptor = __real_open(path, flags, mode);
	writer_descriptor = descriptor;
	return descriptor;
}

FILE *
__wrap_fdopen(int descriptor, const char *mode)
{
	if (!active || descriptor != writer_descriptor)
		return __real_fdopen(descriptor, mode);
	record('D');
	require(strcmp(mode, "w") == 0, "fdopen mode changed");
	if (inject('D'))
		return NULL;
	writer_stream = __real_fdopen(descriptor, mode);
	return writer_stream;
}

int
__wrap_fflush(FILE *stream)
{
	int result;

	if (!active || stream != writer_stream)
		return __real_fflush(stream);
	record('F');
	if (inject('F'))
		return EOF;
	result = __real_fflush(stream);
	if (result == 0) {
		assert_bytes(temporary, canonical_bytes, canonical_length);
		flush_completed = 1;
	}
	return result;
}

int
__wrap_fsync(int descriptor)
{
	int result;

	if (!active || descriptor != writer_descriptor)
		return __real_fsync(descriptor);
	record('S');
	require(flush_completed, "fsync preceded a successful flush");
	if (inject('S'))
		return -1;
	result = __real_fsync(descriptor);
	if (result == 0)
		sync_completed = 1;
	return result;
}

int
__wrap_fclose(FILE *stream)
{
	int result, fail, cleanup_error;

	if (!active || stream != writer_stream)
		return __real_fclose(stream);
	record('C');
	cleanup_error = fault_taken ? current->cleanup_close_error : 0;
	fail = inject('C');
	writer_stream = NULL;
	writer_descriptor = -1;
	result = __real_fclose(stream);
	if (fail || cleanup_error != 0) {
		errno = fail ? current->primary_errno : cleanup_error;
		return EOF;
	}
	return result;
}

int
__wrap_close(int descriptor)
{
	int result, cleanup_error;

	if (!active || descriptor != writer_descriptor)
		return __real_close(descriptor);
	record('X');
	cleanup_error = fault_taken ? current->cleanup_close_error : 0;
	writer_descriptor = -1;
	result = __real_close(descriptor);
	if (cleanup_error != 0) {
		errno = cleanup_error;
		return -1;
	}
	return result;
}

int
__wrap_rename(const char *source, const char *target)
{
	int result;

	if (!active || strcmp(source, temporary) != 0)
		return __real_rename(source, target);
	record('R');
	require(strcmp(target, destination) == 0, "rename target changed");
	require(flush_completed && sync_completed && writer_stream == NULL &&
	    writer_descriptor < 0, "publication preceded flush, sync, or close");
	assert_bytes(temporary, canonical_bytes, canonical_length);
	if (inject('R'))
		return -1;
	result = __real_rename(source, target);
	if (result == 0) {
		published = 1;
		assert_bytes(destination, canonical_bytes, canonical_length);
	}
	return result;
}

int
__wrap_unlink(const char *path)
{
	if (!active || strcmp(path, temporary) != 0)
		return __real_unlink(path);
	record('U');
	if (current->cleanup_unlink_error != 0) {
		errno = current->cleanup_unlink_error;
		return -1;
	}
	return __real_unlink(path);
}

static void
install_prior(void)
{
	int descriptor;
	ssize_t count;
	size_t done;

	descriptor = __real_open(destination, O_WRONLY | O_CREAT | O_TRUNC,
	    0644);
	require(descriptor >= 0, "cannot install prior generation");
	for (done = 0; done < prior_length; done += (size_t)count) {
		count = write(descriptor, prior_bytes + done, prior_length - done);
		require(count > 0, "cannot write prior generation");
	}
	require(__real_close(descriptor) == 0, "cannot close prior generation");
}

static void
run_case(const struct fault_case *test, const struct netconf *candidate)
{
	char error[160];
	struct stat status;
	int result, saved;

	current = test;
	event_count = 0;
	events[0] = '\0';
	fault_taken = published = flush_completed = sync_completed = 0;
	writer_stream = NULL;
	writer_descriptor = -1;
	install_prior();
	strcpy(error, "stale error");
	errno = 0;
	active = 1;
	result = netconf_save_atomic_locked(destination, candidate, error,
	    sizeof(error));
	saved = errno;
	active = 0;
	require(strcmp(events, test->events) == 0, "unexpected stage order");
	require(writer_stream == NULL && writer_descriptor < 0,
	    "writer leaked its stream or descriptor");
	if (test->failed_stage == '\0') {
		require(result == 0 && error[0] == '\0' && published,
		    "successful save did not publish or clear stale error");
		assert_bytes(destination, canonical_bytes, canonical_length);
		require(stat(destination, &status) == 0 &&
		    (status.st_mode & 0777) == 0644,
		    "successful replacement mode changed");
	} else {
		require(result == -1 && fault_taken && !published,
		    "failed save reported success or published");
		require(saved == test->primary_errno &&
		    strcmp(error, strerror(test->primary_errno)) == 0,
		    "cleanup overwrote the primary error");
		assert_bytes(destination, prior_bytes, prior_length);
	}
	if (test->cleanup_unlink_error != 0) {
		require(access(temporary, F_OK) == 0,
		    "unlink failure did not leave the uncommitted temporary");
		require(__real_unlink(temporary) == 0,
		    "cannot clean up injected unlink failure");
	} else {
		require(access(temporary, F_OK) != 0 && errno == ENOENT,
		    "writer left an owned temporary behind");
	}
	printf("NCOM atomic writer: %s PASS (%s)\n", test->name, events);
}

int
main(int argc, char **argv)
{
	static const struct fault_case cases[] = {
		{"success", '\0', 0, 0, 0, "ODFSCR"},
		{"open failure", 'O', EACCES, 0, 0, "O"},
		{"fdopen failure", 'D', ENOMEM, 0, 0, "ODXU"},
		{"flush failure", 'F', ENOSPC, 0, 0, "ODFCU"},
		{"fsync failure", 'S', EIO, 0, 0, "ODFSCU"},
		{"fclose failure", 'C', EDQUOT, 0, 0, "ODFSCU"},
		{"rename failure", 'R', EROFS, 0, 0, "ODFSCRU"},
		{"fdopen and cleanup failures", 'D', ENOMEM, EBADF,
		    EACCES, "ODXU"},
		{"fsync and cleanup failures", 'S', EIO, EPIPE,
		    EACCES, "ODFSCU"},
		{"rename and unlink failures", 'R', EROFS, 0,
		    EACCES, "ODFSCRU"}
	};
	struct netconf prior, candidate;
	FILE *stream;
	char error[160], *old_text, *new_text;
	size_t index, old_length, new_length;

	require(argc == 3, "usage: TEST CONFIGURATION DISPOSABLE_DIRECTORY");
	alarm(15);
	(void)umask(022);
	require(snprintf(destination, sizeof(destination), "%s/net.conf", argv[2])
	    < (int)sizeof(destination), "destination path overflow");
	require(snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", destination,
	    (long)getpid()) < (int)sizeof(temporary), "temporary path overflow");
	require(netconf_load(argv[1], &prior, error, sizeof(error)) == 0,
	    "cannot load candidate fixture");
	candidate = prior;
	for (index = 0; index < candidate.interface_count; index++) {
		if (strcmp(candidate.interfaces[index].name, "ne0") == 0)
			break;
	}
	require(index < candidate.interface_count &&
	    candidate.interfaces[index].address_count == 1,
	    "fixture does not contain one ne0 address");
	strcpy(candidate.interfaces[index].addresses[0].address, "10.0.2.17");
	old_text = new_text = NULL;
	old_length = new_length = 0;
	stream = open_memstream(&old_text, &old_length);
	require(stream != NULL && netconf_write(stream, &prior) == 0 &&
	    fclose(stream) == 0, "cannot serialize prior fixture");
	stream = open_memstream(&new_text, &new_length);
	require(stream != NULL && netconf_write(stream, &candidate) == 0 &&
	    fclose(stream) == 0, "cannot serialize canonical fixture");
	require(old_length != new_length ||
	    memcmp(old_text, new_text, old_length) != 0,
	    "candidate is not different from prior generation");
	prior_bytes = old_text;
	prior_length = old_length;
	canonical_bytes = new_text;
	canonical_length = new_length;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		run_case(&cases[index], &candidate);
	require(__real_unlink(destination) == 0, "cannot clean final fixture");
	free(old_text);
	free(new_text);
	puts("WS011 atomic writer host contract: PASS (not target overlay coverage)");
	return 0;
}
