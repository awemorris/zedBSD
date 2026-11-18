/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The spool (plan/ws145/design.md §5.2): $XDG_RUNTIME_DIR/keiland-print/
 * holds a directory for each daemon, named by its pid and a random number,
 * beside a lock file the daemon holds with flock while it lives.  At the
 * start the directories whose lock nobody holds (left by a daemon that
 * died) are removed.  A job's document is copied into job-<job>.pdf, at
 * most its size when it came and 256 MiB, and must start with "%PDF-" in
 * its first 1024 bytes.
 */

#include "printd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The piece copied at once. */
#define SPOOL_CHUNK		(64U * 1024U)

/* The lock file's descriptor, held while the daemon lives. */
static int spool_lock = -1;

static int spool_base(char *base, size_t size);
static void spool_remove_dir(const char *dir);
static void spool_clean(const char *base);

/*
 * Makes the daemon's spool directory and writes its path.  Returns 0, or
 * an errno value when there is no runtime directory of the user's own.
 */
int
pd_spool_open(
	char *dir,
	size_t size)
{
	char base[384];
	char lock[512];
	unsigned tries;
	int status;
	int fd;

	/* The base under the runtime directory. */
	status = spool_base(base, sizeof(base));
	if (status != 0)
		return status;

	/* The directories left by daemons that died. */
	spool_clean(base);

	/* A name nobody has: its lock file made and held, then the directory. */
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (tries = 0; tries < 16U; tries++) {
		(void)snprintf(lock, sizeof(lock), "%s/%ld-%d.lock", base, (long)getpid(), rand() % 100000);
		fd = open(lock, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0 && errno == EEXIST)
			continue;
		if (fd < 0)
			return errno;

		/* Held; the directory beside it. */
		(void)flock(fd, LOCK_EX);
		spool_lock = fd;
		(void)snprintf(dir, size, "%.*s", (int)(strlen(lock) - 5U), lock);
		status = mkdir(dir, 0700);
		if (status != 0)
			return errno;
		return 0;
	}

	/* No name could be had. */
	return EEXIST;
}

/*
 * Copies a job's document from its descriptor into the spool; *detail is
 * the word of a refusal (toobig, busy, format, io).  used is the spool's
 * bytes in use.  Returns 0 (job->file and job->size set) or an errno value.
 */
int
pd_spool_copy(
	const char *dir,
	struct pd_job *job,
	int fd,
	uint64_t used,
	const char **detail)
{
	unsigned char head[1024];
	unsigned char *chunk;
	struct stat status;
	uint64_t want;
	uint64_t done;
	ssize_t got;
	ssize_t wrote;
	size_t take;
	int out;
	int error;
	int found;
	int regular;
	int closed;
	size_t index;

	/* A regular file of a size taken. */
	*detail = "io";
	error = fstat(fd, &status);
	if (error != 0)
		return EINVAL;
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return EINVAL;
	want = (uint64_t)status.st_size;
	*detail = "toobig";
	if (want == 0U || want > PD_FILE_MAX)
		return EFBIG;
	*detail = "busy";
	if (used + want > PD_SPOOL_MAX)
		return EBUSY;

	/* A PDF: "%PDF-" in its first 1024 bytes. */
	*detail = "format";
	got = pread(fd, head, sizeof(head), 0);
	if (got <= 0)
		return EINVAL;
	found = 0;
	for (index = 0; index + 5U <= (size_t)got && !found; index++)
		found = memcmp(head + index, "%PDF-", 5U) == 0;
	if (!found)
		return EINVAL;

	/* The spool file, new. */
	*detail = "io";
	(void)snprintf(job->file, sizeof(job->file), "%s/job-%lu.pdf", dir, (unsigned long)job->job);
	out = open(job->file, O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0600);
	if (out < 0)
		return errno;
	chunk = malloc(SPOOL_CHUNK);
	if (chunk == NULL) {
		(void)close(out);
		(void)unlink(job->file);
		return ENOMEM;
	}

	/* Copied piece by piece, no more than the size it had. */
	done = 0;
	error = 0;
	while (done < want) {
		take = SPOOL_CHUNK;
		if (want - done < take)
			take = (size_t)(want - done);
		got = pread(fd, chunk, take, (off_t)done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		wrote = write(out, chunk, (size_t)got);
		if (wrote != got) {
			error = EIO;
			break;
		}

		/* Copied so far. */
		done += (uint64_t)got;
	}

	/* The room goes. */
	free(chunk);

	/* Closed; a failure leaves nothing. */
	closed = close(out);
	if (closed != 0 && error == 0)
		error = EIO;
	if (error != 0 || done == 0U) {
		(void)unlink(job->file);
		return EIO;
	}

	/* Succeeded: the bytes copied are the job's size. */
	job->size = done;
	*detail = "";
	return 0;
}

/*
 * Removes the daemon's spool directory and lets its lock go.
 */
void
pd_spool_close(
	const char *dir)
{
	char lock[512];

	/* The directory and its lock file. */
	if (dir[0] == '\0')
		return;
	spool_remove_dir(dir);
	(void)snprintf(lock, sizeof(lock), "%s.lock", dir);
	(void)unlink(lock);
	if (spool_lock >= 0)
		(void)close(spool_lock);
	spool_lock = -1;
}

/*
 * Writes $XDG_RUNTIME_DIR/keiland-print, made when it is not there; the
 * runtime directory must be the user's, with no access for others.
 */
static int
spool_base(
	char *base,
	size_t size)
{
	struct stat status;
	const char *runtime;
	uid_t user;
	int directory;
	int error;

	/* The runtime directory, the user's own and closed to others. */
	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] != '/')
		return ENOENT;
	error = stat(runtime, &status);
	if (error != 0)
		return errno;
	directory = S_ISDIR(status.st_mode);
	user = getuid();
	if (!directory || status.st_uid != user || (status.st_mode & 077) != 0)
		return EPERM;

	/* keiland-print in it. */
	(void)snprintf(base, size, "%s/keiland-print", runtime);
	error = mkdir(base, 0700);
	if (error != 0 && errno != EEXIST)
		return errno;
	return 0;
}

/* Removes a spool directory's files and the directory. */
static void
spool_remove_dir(
	const char *dir)
{
	struct dirent *entry;
	char path[768];
	DIR *opened;

	/* Each file. */
	opened = opendir(dir);
	if (opened != NULL) {
		for (;;) {
			entry = readdir(opened);
			if (entry == NULL)
				break;
			if (entry->d_name[0] == '.')
				continue;
			(void)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
			(void)unlink(path);
		}

		/* Read through. */
		(void)closedir(opened);
	}

	/* The directory. */
	(void)rmdir(dir);
}

/* Removes the directories whose lock file nobody holds (their daemon died). */
static void
spool_clean(
	const char *base)
{
	struct dirent *entry;
	char path[768];
	size_t length;
	DIR *opened;
	int status;
	int lock;
	int fd;

	/* Each lock file. */
	opened = opendir(base);
	if (opened == NULL)
		return;
	for (;;) {
		entry = readdir(opened);
		if (entry == NULL)
			break;
		length = strlen(entry->d_name);
		if (length < 6U)
			continue;
		lock = strcmp(entry->d_name + length - 5U, ".lock");
		if (lock != 0)
			continue;

		/* A lock that can be taken: its daemon is gone, its directory goes. */
		(void)snprintf(path, sizeof(path), "%s/%s", base, entry->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0)
			continue;
		status = flock(fd, LOCK_EX | LOCK_NB);
		if (status == 0) {
			path[strlen(path) - 5U] = '\0';
			spool_remove_dir(path);
			(void)snprintf(path, sizeof(path), "%s/%s", base, entry->d_name);
			(void)unlink(path);
		}

		/* Its lock let go again. */
		(void)close(fd);
	}

	/* Read through. */
	(void)closedir(opened);
}
